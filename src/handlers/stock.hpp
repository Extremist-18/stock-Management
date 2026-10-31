#pragma once
#include "auth/guard.hpp"
#include <regex>

inline void addTxn(Pool &pool,const std::string &type,Request &req, Response &res){
    nlohmann::json j;
    if(!jsonBody(req,res,j))    return;
    if(j.is_discarded() || !j.contains("site_id") || !j.contains("quantity") || !j.contains("material_id")){
        res.error(400,"need essential terms, site_id, quantity, material_id");
        return;
    }

    int site = j["site_id"].get<int>();
    int mat = j["material_id"].get<int>();
    double qnty = j["quantity"].get<double>();
    std::string note = j.value("note", "");
    if(qnty<=0){ res.error(400,"Quantity must be greater than 0"); return; }

    std::string ref = j.value("client_ref","");
    if(!ref.empty()){
        static const std::regex uuidRe("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$");
        if(!std::regex_match(ref,uuidRe)){ res.error(400,"client_ref must be a UUID"); return; }
    }

    auto conn = pool.acquire();
    pqxx::work txn(*conn);
    if(!canAccessSite(txn,req,site)){ res.error(403,"No access to this site");    return; }
    auto x = txn.exec("SELECT (SELECT is_active FROM sites WHERE id = $1) AS s, "
    "(SELECT is_active FROM materials WHERE id = $2) AS m", pqxx::params{site,mat});

    if(x[0]["s"].is_null() || !x[0]["s"].as<bool>() || x[0]["m"].is_null() || !x[0]["m"].as<bool>()) {res.error(400,"Unknown or inactive material/site"); return;}
    setUser(txn,req.user_id);

    if(!ref.empty()){ 
        auto dup = txn.exec("SELECT id, created_by FROM stock_txns WHERE client_ref = $1::uuid", pqxx::params{ref});
        if(!dup.empty()){
            if(dup[0]["created_by"].as<int>()!=req.user_id){ res.error(409,"client_ref already used"); return; }
            res.json(200,{{"id",dup[0]["id"].as<long long>()},{"duplicate",true}});
            return;
        }
    }

    auto r = txn.exec("INSERT INTO stock_txns (site_id, material_id, type, quantity, note, created_by, client_ref) "
    "VALUES ($1,$2,$3,$4,$5,$6,NULLIF($7,'')::uuid) RETURNING id", pqxx::params{site,mat,type,qnty,note,req.user_id,ref});
    txn.commit();
    res.json(201,{{"id",r[0][0].as<long long>()}});
}

inline void registerStockRoutes(App &app, Pool &pool){
    
    app.get("/sites",requireAuth(pool,[&pool](Request &req, Response& res){
        // int id;
        // try{
            //     id = std::stoi(req.params["id"]);
            // }catch(...){ res.error(400,"invalid id"); return;}
            
        auto conn  =pool.acquire();
        pqxx::work tx(*conn);
        pqxx::result rows=(req.role=="admin")?tx.exec("SELECT id, name, location FROM sites WHERE is_active ORDER BY id"):
        tx.exec("SELECT s.id, s.name, s.location FROM user_sites us JOIN sites s ON s.id = us.site_id "
            "WHERE us.user_id = $1 AND s.is_active ORDER BY s.id", pqxx::params{req.user_id});
            
        tx.commit();
        nlohmann::json arr = nlohmann::json::array();
        for(auto x:rows){
            arr.push_back({{"id", x["id"].as<int>()},
            {"name",x["name"].as<std::string>()}, {"location", x["location"].is_null()?nlohmann::json(nullptr):
                nlohmann::json(x["location"].as<std::string>())}});
        }
        res.json(200,arr);
    }));

    app.get("/materials",requireAuth(pool,[&pool](Request &req,Response &res){
        auto conn=pool.acquire();
        pqxx::work tx(*conn);
        auto x=tx.exec("SELECT id, name, unit FROM materials WHERE is_active ORDER BY name");
        tx.commit();
        nlohmann::json arr=nlohmann::json::array();
        for(auto i:x){
            arr.push_back({{"id",i["id"].as<int>()},{"name",i["name"].as<std::string>()},{"unit",i["unit"].as<std::string>()}});
        }
        res.json(200,arr);
    }));    

    app.post("/usage",requireAuth(pool,[&pool](Request &req, Response &res){addTxn(pool,"USAGE",req,res);}));
    app.post("/stock-in",requireAuth(pool,[&pool](Request &req, Response &res){addTxn(pool,"IN",req,res);}));

    // Current balance for 1 site
    app.get("/sites/:id/stock",requireAuth(pool,[&pool](Request &req,Response &res){
        int id;
        if(!idParam(req,res,"id",id))   return;
        auto conn=pool.acquire();
        pqxx::work tx(*conn);
        if(!canAccessSite(tx,req,id)){ res.error(403,"No access to this site");    return; }
        auto x = tx.exec("SELECT material_id AS id, material AS name, unit, balance "
            "FROM stock_overview WHERE site_id = $1 ORDER BY material", pqxx::params{id});
        tx.commit();
        nlohmann::json arr=nlohmann::json::array();
        for(auto i:x){
            arr.push_back({{"material_id",i["id"].as<int>()},{"name",i["name"].as<std::string>()},
            {"unit",i["unit"].as<std::string>()},{"balance",i["balance"].as<double>()}});
        }
        res.json(200,arr);
    }));

    app.get("/bootstrap",requireAuth(pool,[&pool](Request &req,Response &res){
        const bool admin=(req.role=="admin");
        auto conn=pool.acquire();
        pqxx::work tx(*conn);
        auto me=tx.exec("SELECT name, username FROM users WHERE id=$1",pqxx::params{req.user_id});
        auto sites=tx.exec("SELECT id, name, location FROM sites WHERE is_active AND "
            "($1 OR id IN (SELECT site_id FROM user_sites WHERE user_id=$2)) ORDER BY id",pqxx::params{admin,req.user_id});
        auto mats=tx.exec("SELECT id, name, unit FROM materials WHERE is_active ORDER BY name");
        auto stock=tx.exec("SELECT site_id, material_id, balance FROM stock_overview WHERE "
            "($1 OR site_id IN (SELECT site_id FROM user_sites WHERE user_id=$2)) ORDER BY site_id, material",pqxx::params{admin,req.user_id});
        nlohmann::json pending=nullptr;
        if(admin){
            auto p=tx.exec("SELECT (SELECT count(*) FROM transfer_requests WHERE status='PENDING') AS transfers, "
                "(SELECT count(*) FROM user_requests WHERE status='PENDING') AS signups");
            pending={{"transfers",p[0]["transfers"].as<int>()},{"signups",p[0]["signups"].as<int>()}};
        }
        tx.commit();

        nlohmann::json sj=nlohmann::json::array(), mj=nlohmann::json::array(), stj=nlohmann::json::array();
        for(auto i:sites)
            sj.push_back({{"id",i["id"].as<int>()},{"name",i["name"].as<std::string>()},{"location",nullableText(i["location"])}});
        for(auto i:mats)
            mj.push_back({{"id",i["id"].as<int>()},{"name",i["name"].as<std::string>()},{"unit",i["unit"].as<std::string>()}});
        for(auto i:stock)
            stj.push_back({{"site_id",i["site_id"].as<int>()},{"material_id",i["material_id"].as<int>()},{"balance",i["balance"].as<double>()}});

        res.json(200,{{"user",{{"id",req.user_id},{"role",req.role},{"name",me[0]["name"].as<std::string>()},{"username",me[0]["username"].as<std::string>()}}},
            {"sites",sj},{"materials",mj},{"stock",stj},{"pending",pending}});
    }));

    // history, newest first.  ?from=YYYY-MM-DD&to=YYYY-MM-DD&limit=100
    app.get("/sites/:id/txns",requireAuth(pool,[&pool](Request &req, Response &res){
        int id;
        if(!idParam(req,res,"id",id))   return;
        std::string from = req.query.count("from") && !req.query["from"].empty()?req.query["from"]:"2025-01-01";
        std::string to = req.query.count("to") && !req.query["to"].empty() ? req.query["to"] : "9999-12-31";
        int limit = 200;
        if(req.query.count("limit")) { try { limit = std::stoi(req.query["limit"]); } catch (...) {} }
        if(limit<1) limit = 1;
        if(limit>500) limit = 500;

        auto conn=pool.acquire();
        pqxx::work tx(*conn);
        if(!canAccessSite(tx,req,id)){ res.error(403,"No access to this site"); return;}
        auto x = tx.exec(
            "SELECT t.id, t.material_id, m.name AS material, m.unit, t.type, t.quantity, "
            "t.txn_date::text AS txn_date, t.note, t.created_by, u.name AS by_name, "
            "t.created_at::text AS created_at, "
            "(t.transfer_ref IS NULL AND t.production_id IS NULL AND now() - t.created_at <= edit_window()) AS can_edit "
            "FROM stock_txns t JOIN materials m ON m.id = t.material_id JOIN users u ON u.id = t.created_by "
            "WHERE t.site_id = $1 AND t.txn_date >= $2::date AND t.txn_date <= $3::date "
            "ORDER BY t.created_at DESC, t.id DESC LIMIT $4", pqxx::params{id,from,to,limit});
        tx.commit();    
        nlohmann::json arr=nlohmann::json::array();
        for(auto i:x){
            bool canEdit = i["can_edit"].as<bool>();
            bool mine = i["created_by"].as<int>() == req.user_id;
            arr.push_back({{"id", i["id"].as<long long>()}, {"material_id", i["material_id"].as<int>()},
            {"material", i["material"].as<std::string>()}, {"unit", i["unit"].as<std::string>()},
            {"type", i["type"].as<std::string>()}, {"quantity", i["quantity"].as<double>()}, {"date", i["txn_date"].as<std::string>()}, {"note", i["note"].is_null() ? "" : i["note"].as<std::string>()},
            {"by", i["by_name"].as<std::string>()},{"created_at", i["created_at"].as<std::string>()}, {"editable", mine && canEdit}});
        }
        res.json(200,arr);
    }));

    // edit quantity/note: creator only, first hour only (the database enforces both)
    app.put("/txns/:id",requireAuth(pool,[&pool](Request &req, Response &res){
        int id;
        if(!idParam(req,res,"id",id))   return;
        nlohmann::json j;
        if(!jsonBody(req,res,j))    return;

        auto conn=pool.acquire();
        pqxx::work tx(*conn);
        setUser(tx,req.user_id);
        auto curr = tx.exec("SELECT quantity,note from stock_txns where id=$1",pqxx::params{id});
        if(curr.empty()){res.error(404,"Entry not found!"); return;}
        
        double qnty = j.contains("quantity") ? j["quantity"].get<double>():curr[0]["quantity"].as<double>();
        std::string note = j.contains("note") ? j["note"].get<std::string>():(curr[0]["note"].is_null() ? "" : curr[0]["note"].as<std::string>());
        if(qnty<=0){res.error(400,"quantity must be greater than 0");return;}

        tx.exec("UPDATE stock_txns SET quantity = $2,note = $3 WHERE id=$1", pqxx::params{id,qnty,note});
        tx.commit();
        res.json(200, {{"ok", true}});
    }));

    app.del("/txns/:id",requireAuth(pool,[&pool](Request& req, Response& res){
        int id;
        if(!idParam(req,res,"id",id)) return;
        auto conn = pool.acquire();
        pqxx::work tx(*conn);
        setUser(tx,req.user_id);
        auto x = tx.exec("DELETE FROM stock_txns WHERE id=$1 RETURNING id", pqxx::params{id});
        if(x.empty()){ res.error(404,"Entry not found"); return;}
        tx.commit();
        res.json(200, {{"ok", true}});
    }));

    app.post("/sites",requireAdmin(pool,[&pool](Request& req, Response& res){
        nlohmann::json j;
        if(!jsonBody(req,res,j)) return;
        std::string name = j.value("name",""), loc = j.value("location","");
        if(name.empty()){res.error(400, "Name required"); return;}
        auto conn = pool.acquire();
        pqxx::work tx(*conn);
        auto x =tx.exec("INSERT INTO sites (name, location) VALUES ($1,$2) RETURNING id", pqxx::params{name, loc});
        audit(tx, "sites", x[0][0].as<long long>(),"SITE_CREATED",req.user_id,j);
        tx.commit();
        res.json(201,{{"id", x[0][0].as<int>()}});
    }));

    app.post("/materials",requireAdmin(pool,[&pool](Request& req,Response& res){
        nlohmann::json j;
        if(!jsonBody(req,res,j)) return;
        std::string name = j.value("name",""), unit = j.value("unit","");
        if(name.empty() || unit.empty()){res.error(400,"name and unit required"); return;}
        auto conn = pool.acquire();
        pqxx::work tx(*conn);
        auto x=tx.exec("INSERT INTO materials (name, unit) VALUES ($1,$2) RETURNING id",pqxx::params{name,unit});
        audit(tx,"materials",x[0][0].as<long long>(),"MATERIAL_CREATED",req.user_id,j);
        tx.commit();
        res.json(201,{{"id", x[0][0].as<int>()}});
    }));
}