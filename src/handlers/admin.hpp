#pragma once
#include "auth/guard.hpp"
#include "auth/password.hpp"
#include "handlers/authorization.hpp"

inline void registerAdminRoutes(App &app, Pool &pool){
    //admin only 
    app.get("/users",requireAdmin(pool,[&pool](Request&,Response& res){
        auto conn=pool.acquire();
        pqxx::work tx(*conn);
        auto x = tx.exec(
            "SELECT u.id, u.name, u.username, u.role, u.is_active,COALESCE(json_agg(json_build_object('id', s.id, 'name', s.name)) "
            "FILTER (WHERE s.id IS NOT NULL), '[]') AS sites FROM users u LEFT JOIN user_sites us ON us.user_id = u.id "
            "LEFT JOIN sites s ON s.id = us.site_id GROUP BY u.id ORDER BY u.id");
        tx.commit();
        nlohmann::json arr=nlohmann::json::array();
        for(auto i:x)
            arr.push_back({{"id", i["id"].as<int>()}, {"name", i["name"].as<std::string>()},
            {"username", i["username"].as<std::string>()}, {"role", i["role"].as<std::string>()}, {"is_active", i["is_active"].as<bool>()}, {"sites", nlohmann::json::parse(i["sites"].as<std::string>())}});
        res.json(200, arr);
    }));

    // create user , admins should be able to create user directly! -> no approvals
    app.post("/users",requireAdmin(pool,[&pool](Request &req, Response &res){
        nlohmann::json j;
        if(!jsonBody(req,res,j))    return;
        std::string name=j.value("name",""), username=j.value("username",""),password=j.value("password",""), role=j.value("role","supervisor");
        if(name.empty() || !validUsername(username)){res.error(400,"Name and Valid Username Required"); return;}
        if(password.size()<6){res.error(400,"Password must have atleast 6 characters"); return;}
        if((role!="admin") && (role!="supervisor")) {res.error(401,"Role must be supervisor or admin only"); return;}

        auto conn=pool.acquire();
        pqxx::work tx(*conn);
        // clear messages before the (slow) password hashing and the insert
        auto dup=tx.exec("SELECT 1 FROM users WHERE username=$1 UNION ALL SELECT 1 FROM user_requests WHERE username=$1 AND status='PENDING'",pqxx::params{username});
        if(!dup.empty()){res.error(409,"Username \""+username+"\" is already taken. Choose a different username."); return;}
        if(j.contains("site_ids")){
            if(!j["site_ids"].is_array()){res.error(400,"site_ids must be a list"); return;}
            for(auto &s:j["site_ids"]){
                if(!s.is_number_integer()){res.error(400,"site_ids must be numbers"); return;}
                auto ok=tx.exec("SELECT 1 FROM sites WHERE id=$1 AND is_active",pqxx::params{s.get<int>()});
                if(ok.empty()){res.error(400,"Site number "+std::to_string(s.get<int>())+" does not exist or is switched off."); return;}
            }
        }
        std::string hash =pw::hash(password);
        auto x=tx.exec("INSERT INTO users (name,username,password_hash,role) VALUES ($1,$2,$3,$4) RETURNING id",pqxx::params{name, username, hash, role});
        int id = x[0][0].as<int>();
        if(j.contains("site_ids"))
            for(auto &s:j["site_ids"])
                tx.exec("INSERT INTO user_sites (user_id,site_id) VALUES ($1,$2)",pqxx::params{id,s.get<int>()});
        
        audit(tx,"users",id,"USER_CREATED",req.user_id,{{"username",username},{"role",role}});
        tx.commit();
        res.json(201,{{"id",id}});
    }));

    auto setActive=[&pool](bool active){
        return requireAdmin(pool,[&pool,active](Request &req,Response &res){
            int id;
            if(!idParam(req,res,"id",id))  return;
            if(id==req.user_id && !active){res.error(400,"you cannot disable yourself");return;}

            auto conn =pool.acquire();
            pqxx::work tx(*conn);
            auto x=tx.exec("UPDATE users SET is_active=$2 WHERE id=$1 RETURNING id",pqxx::params{id,active});
            if(x.empty()){res.error(404,"User not found"); return;}
            audit(tx,"users",id,active?"USER_ACTIVATED":"USER_DEACTIVATED",req.user_id,nlohmann::json::object());
            tx.commit();
            res.json(200,{{"ok",true}});
        });
    };

    app.post("/users/:id/deactivate",setActive(false));
    app.post("/users/:id/activate",setActive(true));

    app.post("/users/:id/reset-password", requireAdmin(pool, [&pool](Request& req, Response& res) {
        int id;
        if(!idParam(req,res,"id",id)) return;
        nlohmann::json j;
        if(!jsonBody(req,res,j)) return;
        std::string password=j.value("password","");
        if(password.size()<6){res.error(400,"password must be at least 6 characters");return;}
        std::string hash=pw::hash(password);
        auto conn = pool.acquire();
        pqxx::work tx(*conn);
        auto x=tx.exec("UPDATE users SET password_hash=$2 WHERE id=$1 RETURNING id", pqxx::params{id,hash});
        if(x.empty()){res.error(404,"user not found");return;}
        audit(tx,"users",id,"PASSWORD_RESET",req.user_id,nlohmann::json::object());
        tx.commit();
        res.json(200,{{"ok",true}});
    }));

    app.post("/users/:id/sites",requireAdmin(pool,[&pool](Request& req,Response& res){
        int id;
        if(!idParam(req,res,"id",id)) return;
        nlohmann::json j;
        if(!jsonBody(req,res,j)) return;
        int site=j.value("site_id",0);
        auto conn = pool.acquire();
        pqxx::work tx(*conn);
        tx.exec("INSERT INTO user_sites (user_id,site_id) VALUES ($1,$2) ON CONFLICT DO NOTHING", pqxx::params{id,site});
        audit(tx,"user_sites",id,"SITE_ASSIGNED",req.user_id,{{"site_id",site}});
        tx.commit();
        res.json(200,{{"ok",true}});
    }));

    app.del("/users/:id/sites/:site_id",requireAdmin(pool,[&pool](Request& req,Response& res) {
        int id, site;
        if(!idParam(req,res,"id",id) || !idParam(req,res,"site_id",site)) return;
        auto conn = pool.acquire();
        pqxx::work tx(*conn);
        tx.exec("DELETE FROM user_sites WHERE user_id=$1 AND site_id=$2", pqxx::params{id,site});
        audit(tx,"user_sites",id,"SITE_UNASSIGNED",req.user_id,{{"site_id",site}});
        tx.commit();
        res.json(200,{{"ok",true}});
    }));

    // NEW-SUPERVISOR REQUESTS (admin approves)
    app.get("/user-requests",requireAdmin(pool,[&pool](Request &req,Response &res){
        std::string status=req.query.count("status")?req.query["status"]:"";
        auto conn=pool.acquire();
        pqxx::work tx(*conn);
        auto x=tx.exec("SELECT r.id,r.name,r.username,r.status,r.requested_at::text AS at, "
        "s.name AS site, r.reject_reason FROM user_requests r JOIN sites s ON s.id=r.site_id "
        "WHERE ($1='' OR r.status=$1) ORDER BY r.id DESC LIMIT 200",pqxx::params{status});
        tx.commit();
        nlohmann::json arr=nlohmann::json::array();
        for(auto i:x){
            arr.push_back({{"id",i["id"].as<long long>()},{"name",i["name"].as<std::string>()},{"username",i["username"].as<std::string>()},
            {"status",i["status"].as<std::string>()},{"site",i["site"].as<std::string>()},{"requested_at",i["requested_at"].as<std::string>()} });
        }
        res.json(200,arr);
    }));

    app.post("/user-requests/:id/approve",requireAdmin(pool,[&pool](Request &req,Response &res){
        int id;
        if(!idParam(req,res,"id",id))  return;
        auto conn = pool.acquire();
        pqxx::work tx(*conn);

        auto x=tx.exec("UPDATE user_requests SET status='APPROVED',decided_by=$2,decided_at=now() "
        "WHERE id=$1 AND status='PENDING' RETURNING name,username,password_hash,site_id",pqxx::params{id,req.user_id});
        if(x.empty()){res.error(409,"request not found or already decided");return;}
        auto u=tx.exec("INSERT INTO users (name,username,password_hash,role) VALUES ($1,$2,$3,'supervisor') RETURNING id",
        pqxx::params{x[0]["name"].as<std::string>(), x[0]["username"].as<std::string>(),x[0]["password_hash"].as<std::string>()});
        int uid = u[0][0].as<int>();
        tx.exec("INSERT INTO user_sites (user_id, site_id) VALUES ($1,$2)",pqxx::params{uid,x[0]["site_id"].as<int>()});
        audit(tx,"user_requests",id,"USER_REQUEST_APPROVED",req.user_id,{{"new_user_id",uid}});
        tx.commit();
        res.json(200,{{"ok",true},{"user_id",uid}});
    }));

    app.post("/user-requests/:id/reject",requireAdmin(pool,[&pool](Request& req,Response& res){
        int id;
        if(!idParam(req,res,"id",id)) return;
        nlohmann::json j = nlohmann::json::parse(req.body,nullptr,false);
        std::string reason =(j.is_object())?j.value("reason",""):"";
        auto conn = pool.acquire();
        pqxx::work tx(*conn);
        auto x = tx.exec("UPDATE user_requests SET status='REJECTED',decided_by=$2, decided_at=now(),"
        "reject_reason=$3 WHERE id=$1 AND status='PENDING' RETURNING id",pqxx::params{id,req.user_id,reason});
        if(x.empty()){res.error(409,"request not found or already decided");return;}
        audit(tx,"user_requests",id,"USER_REQUEST_REJECTED",req.user_id,{{"reason",reason}});
        tx.commit();
        res.json(200, {{"ok", true}});
    }));        

    // INTERNAL TRANSFERS (admin approves)
    app.post("/transfer-requests",requireAuth(pool,[&pool](Request& req,Response& res){
        nlohmann::json j;
        if(!jsonBody(req,res,j)) return;
        int from=j.value("from_site_id",0),to=j.value("to_site_id",0),mat=j.value("material_id",0);
        double qty = j.value("quantity",0.0);
        if(from<=0 ||to<=0 ||mat<=0|| qty<=0) {res.error(400, "need from_site_id,to_site_id, material_id, quantity > 0"); return; }
        std::string note = j.value("note","");
        if(from == to) { res.error(400, "source and destination must differ"); return; }
        auto conn = pool.acquire();
        pqxx::work tx(*conn);
        if(!canAccessSite(tx,req,from)){res.error(403, "no access to the source site"); return;}
        auto ok = tx.exec("SELECT (SELECT count(*) FROM sites WHERE id IN ($1,$2) AND is_active) AS s, "
            "(SELECT count(*) FROM materials WHERE id = $3 AND is_active) AS m", pqxx::params{from,to,mat});
        if (ok[0]["s"].as<int>()!=2 || ok[0]["m"].as<int>()!=1){res.error(400, "unknown or inactive site/material"); return;}
        auto r = tx.exec("INSERT INTO transfer_requests (from_site_id,to_site_id,material_id,quantity,note, requested_by) "
        "VALUES ($1,$2,$3,$4,$5,$6) RETURNING id", pqxx::params{from, to, mat, qty, note, req.user_id});
        audit(tx,"transfer_requests",r[0][0].as<long long>(),"TRANSFER_REQUESTED",req.user_id,j);
        tx.commit();
        res.json(201,{{"id",r[0][0].as<long long>()},{"status","PENDING"}});
    }));

    app.get("/transfer-requests",requireAuth(pool,[&pool](Request& req,Response& res){
        std::string status = req.query.count("status")?req.query["status"]:"";
        auto conn = pool.acquire();
        pqxx::work tx(*conn);
        // admin sees everything; a supervisor sees transfers touching their own sites
        auto x = tx.exec(
            "SELECT t.id, fs.name AS from_site, ts.name AS to_site, m.name AS material, m.unit, t.quantity, "
            "t.note, t.status, u.name AS requested_by, t.requested_at::text AS at, t.reject_reason "
            "FROM transfer_requests t JOIN sites fs ON fs.id=t.from_site_id JOIN sites ts ON ts.id=t.to_site_id "
            "JOIN materials m ON m.id = t.material_id JOIN users u ON u.id = t.requested_by "
            "WHERE ($1='' OR t.status=$1) AND ($2 OR t.from_site_id IN (SELECT site_id FROM user_sites WHERE user_id=$3) "
            "OR t.to_site_id IN (SELECT site_id FROM user_sites WHERE user_id = $3)) "
            "ORDER BY t.id DESC LIMIT 200",pqxx::params{status,req.role == "admin",req.user_id});
        tx.commit();
        nlohmann::json arr = nlohmann::json::array();
        for(auto i:x){
            arr.push_back({{"id", i["id"].as<long long>()}, {"from_site", i["from_site"].as<std::string>()}, {"to_site", i["to_site"].as<std::string>()}, {"material", i["material"].as<std::string>()},
            {"unit", i["unit"].as<std::string>()}, {"quantity", i["quantity"].as<double>()}, {"status", i["status"].as<std::string>()}, {"requested_by", i["requested_by"].as<std::string>()}, {"requested_at", i["at"].as<std::string>()}});
        }
        res.json(200,arr);
    }));

    app.post("/transfer-requests/:id/approve", requireAdmin(pool, [&pool](Request& req, Response& res) {
        int id;
        if(!idParam(req,res,"id",id)) return;
        auto conn=pool.acquire();
        pqxx::work tx(*conn);
        setUser(tx,req.user_id);
        auto r = tx.exec("UPDATE transfer_requests SET status = 'APPROVED', decided_by = $2, decided_at = now(), "
        "transfer_ref = gen_random_uuid() WHERE id = $1 AND status = 'PENDING' RETURNING id", pqxx::params{id, req.user_id});
        if(r.empty()){res.error(409,"request not found or already decided");return;}
        // two ledger rows sharing one transfer_ref; the triggers refuse it if the source lacks stock
        tx.exec("INSERT INTO stock_txns (site_id, material_id, type, quantity, note, transfer_ref, created_by) "
        "SELECT from_site_id, material_id, 'TRANSFER_OUT', quantity, note, transfer_ref, $2::int "
        "FROM transfer_requests WHERE id = $1", pqxx::params{id,req.user_id});
        tx.exec("INSERT INTO stock_txns (site_id, material_id, type, quantity, note, transfer_ref, created_by) "
        "SELECT to_site_id, material_id, 'TRANSFER_IN', quantity, note, transfer_ref, $2::int "
        "FROM transfer_requests WHERE id = $1", pqxx::params{id,req.user_id});
        audit(tx,"transfer_requests",id,"TRANSFER_APPROVED",req.user_id,nlohmann::json::object());
        tx.commit();   // all of it, or nothing
        res.json(200,{{"ok",true}});
    }));

    app.post("/transfer-requests/:id/reject", requireAdmin(pool, [&pool](Request& req, Response& res) {
        int id;
        if(!idParam(req,res,"id",id)) return;
        nlohmann::json j = nlohmann::json::parse(req.body, nullptr, false);
        std::string reason = (j.is_object())?j.value("reason",""):"";
        auto conn = pool.acquire();
        pqxx::work tx(*conn);
        auto r=tx.exec("UPDATE transfer_requests SET status='REJECTED', decided_by=$2, decided_at=now(), "
        "reject_reason=$3 WHERE id=$1 AND status='PENDING' RETURNING id", pqxx::params{id, req.user_id, reason});
        if(r.empty()) {res.error(409,"request not found or already decided"); return;}
        audit(tx,"transfer_requests",id,"TRANSFER_REJECTED",req.user_id,{{"reason",reason}});
        tx.commit();
        res.json(200,{{"ok",true}});
    }));

    app.post("/transfer-requests/:id/cancel", requireAuth(pool, [&pool](Request& req, Response& res) {
        int id;
        if(!idParam(req,res,"id",id)) return;
        auto conn = pool.acquire();
        pqxx::work tx(*conn);
        auto r=tx.exec("UPDATE transfer_requests SET status='CANCELLED' "
        "WHERE id=$1 AND status='PENDING' AND requested_by=$2 RETURNING id",pqxx::params{id, req.user_id});
        if(r.empty()) {res.error(409,"not found,not yours,or already decided"); return; }
        audit(tx,"transfer_requests",id,"TRANSFER_CANCELLED",req.user_id,nlohmann::json::object());
        tx.commit();
        res.json(200,{{"ok",true}});
    }));
    
}