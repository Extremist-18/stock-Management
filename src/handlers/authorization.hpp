#pragma once
#include <regex>
#include "auth/guard.hpp"
#include "auth/password.hpp"
#include "http/RateLimit.hpp"

inline bool validUsername(const std::string& u){
    static const std::regex re("^[A-Za-z0-9_.]{3,32}$");
    return std::regex_match(u, re);
}

inline void registerAuthRoutes(App &app, Pool &pool){
    app.post("/login",safe([&pool](Request &req, Response &res){
        nlohmann::json j;
        if(!jsonBody(req,res,j))    return;

        std::string username = j.value("username","");
        std::string password = j.value("password","");
        if(username.empty() || password.empty()){
            res.error(400,"Username and Password are Required!");
            return;
        }
        // rate limits: too many tries from one IP, or too many FAILED tries for this IP+username
        if(!loginIpLimit().allow(req.ip)){ tooMany(res, loginIpLimit().retryAfter(req.ip)); return;}
        const std::string tryKey = req.ip + "|" + username;
        if(loginFailLimit().blocked(tryKey)){ tooMany(res,loginFailLimit().retryAfter(tryKey)); return;}

        int id=0;
        std::string stored,name, role;
        bool found=false;
        {
            auto conn=pool.acquire();
            pqxx::work tx(*conn);
            auto x =tx.exec("SELECT id, name, role, password_hash FROM users "
            "WHERE username = $1 AND is_active",pqxx::params{username}); 
            tx.commit();
            if(!x.empty()){
                found=true;
                id =x[0]["id"].as<int>();
                name =x[0]["name"].as<std::string>();
                role=x[0]["role"].as<std::string>();
                stored = x[0]["password_hash"].as<std::string>();
            }
        }

        bool ok;
        {
            HashSlot slot;                   
            if(!slot.ok){ res.error(503,"server busy, try again in a moment"); return; }
            if(found){
                ok = pw::verify(stored,password);
            }else{
                static const std::string dummy=pw::hash("dummy-password-for-timing");
                pw::verify(dummy,password);  
                ok = false;
            }
         }
        if(ok) loginFailLimit().clear(tryKey); else loginFailLimit().hit(tryKey);

        {
            auto conn=pool.acquire();
            pqxx::work tx(*conn);
            if (ok)
                tx.exec("INSERT INTO audit_log (table_name, record_id, action, changed_by) "
                "VALUES ('users', $1::int, 'LOGIN', $1::int)", pqxx::params{id});
            else
                tx.exec("INSERT INTO audit_log (table_name, action, new_data) "
                "VALUES ('users', 'LOGIN_FAILED', jsonb_build_object('username', $1::text))", pqxx::params{username});
            tx.commit();
        }

        if(!ok){ res.error(401,"Incorrect Credentials!"); return; }
        res.json(200,{{"token",jwt::create(jwtSecret(),id,12*3600)}, {"user",{{"id",id},{"name",name},{"role",role} }}});
    })); 
    
    app.get("/me",requireAuth(pool, [&pool](Request &req, Response &res){
        auto conn= pool.acquire();
        pqxx::work tx(*conn);
        auto x = tx.exec("SELECT name, username FROM users WHERE id = $1",pqxx::params{req.user_id});
        pqxx::result s=(req.role=="admin")?tx.exec("SELECT id, name FROM sites WHERE is_active ORDER BY id"):
        tx.exec("SELECT s.id, s.name FROM user_sites us JOIN sites s ON s.id = us.site_id "
            "WHERE us.user_id = $1 AND s.is_active ORDER BY s.id",pqxx::params{req.user_id});
        
        tx.commit();
        nlohmann::json sites = nlohmann::json::array();
        for(auto i:s){
            sites.push_back({{"id", i["id"].as<int>()}, {"name", i["name"].as<std::string>()}});
        }

        res.json(200, {{"id", req.user_id}, {"role", req.role}, {"name", x[0]["name"].as<std::string>()},
            {"username", x[0]["username"].as<std::string>()}, {"sites", sites}});
    }));

    // POST /signup-requests (public, needs admin approval) 
    app.post("/signup-requests",safe([&pool](Request &req,Response &res){
        if(!signupLimit().allow(req.ip)){ tooMany(res, signupLimit().retryAfter(req.ip)); return;}
        nlohmann::json j;
        if(!jsonBody(req,res,j))    return;
        std::string name = j.value("name","");
        std::string username =j.value("username","");
        std::string password =j.value("password","");
        int site_id = j.value("site_id",0);

        if(name.empty() || name.size()>100){res.error(401,"Name Required! (Max 100 char)"); return;}
        if(!validUsername(username)){ res.error(400,"username: 3-32 letters, digits, _ or ."); return;}
        if(password.size()<6)  {res.error(400,"Password must have atleast 6 characters"); return;}
        if(site_id<=0) {res.error(400,"Invalid Site id"); return;}

        std::string hash;
        {
            HashSlot slot;
            if(!slot.ok){ res.error(503,"server busy, try again in a moment"); return; }
            hash = pw::hash(password);
        }
        auto conn=pool.acquire();
        pqxx::work tx(*conn);
        auto dup = tx.exec("SELECT 1 FROM users WHERE username = $1 "
        "UNION ALL SELECT 1 FROM user_requests WHERE username = $1 AND status = 'PENDING'", pqxx::params{username});

        if(!dup.empty()) {res.error(409,"Username already pending"); return;}
        auto site = tx.exec("SELECT 1 FROM sites WHERE id = $1 AND is_active", pqxx::params{site_id});
        if(site.empty()) { res.error(400, "unknown site"); return;}   
        
        auto r = tx.exec("INSERT INTO user_requests (name, username, password_hash, site_id) "
        "VALUES ($1,$2,$3,$4) RETURNING id", pqxx::params{name, username, hash, site_id});
        tx.commit();
        res.json(201,{{"id",r[0][0].as<long long>()}, {"status","PENDING"}});
    }));
}