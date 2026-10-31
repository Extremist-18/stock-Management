#pragma once
#include <cmath> 
#include "auth/guard.hpp"
#include <regex>

inline void registerPalletRoutes(App& app, Pool& pool){
    // pallet types 
    app.get("/pallet-types", requireAuth(pool, [&pool](Request& req, Response& res){
        auto conn = pool.acquire(); pqxx::work tx(*conn);
        auto x = tx.exec(
          "SELECT pt.id, pt.name, pt.is_active, "
          "       cft_for(pt.id, (now() AT TIME ZONE 'Asia/Kolkata')::date) AS cft_per_pallet "
          "FROM pallet_types pt WHERE pt.is_active ORDER BY pt.name");
        tx.commit();
        nlohmann::json arr = nlohmann::json::array();
        for (auto i : x) {
            nlohmann::json cft = i["cft_per_pallet"].is_null()?nlohmann::json(nullptr) : nlohmann::json(i["cft_per_pallet"].as<double>());
            arr.push_back({{"id", i["id"].as<int>()}, {"name", i["name"].as<std::string>()}, {"cft_per_pallet", cft}});
        }
        res.json(200, arr);
    }));

    app.post("/pallet-types", requireAdmin(pool, [&pool](Request& req, Response& res){
        nlohmann::json j;
        if(!jsonBody(req, res, j)) return;
        std::string name = j.value("name", "");
        if(name.empty() || (int)name.size()>100) {
            res.error(400, "name required (max 100 chars)"); return;
        }
        auto conn = pool.acquire(); pqxx::work tx(*conn);
        auto r = tx.exec("INSERT INTO pallet_types (name) VALUES ($1) RETURNING id",pqxx::params{name});
        int id = r[0][0].as<int>();
        audit(tx, "pallet_types", id, "PALLET_TYPE_CREATED", req.user_id, j);
        tx.commit();
        res.json(201, {{"id", id}});
    }));

    // Set the CFT factor
    app.post("/pallet-types/:id/cft", requireAdmin(pool, [&pool](Request& req, Response& res){
        int id; if (!idParam(req, res, "id", id)) return;
        nlohmann::json j;
        if (!jsonBody(req, res, j)) return;
        if (!j.contains("cft_per_pallet") || !j["cft_per_pallet"].is_number()) {
            res.error(400, "cft_per_pallet must be a number"); return;
        }
        double cft = j["cft_per_pallet"].get<double>();
        if (!(cft > 0 && cft <= 1000)) {
            res.error(400, "cft_per_pallet must be in (0, 1000]"); return;
        }
        // effective_from defaults to today (India); future dates allowed for planned changes
        std::string ef = j.value("effective_from", "");
        if(!ef.empty() && !isIsoDate(ef)) {
            res.error(400, "effective_from must be YYYY-MM-DD"); return;
        }
        std::string note = j.value("note", "");

        auto conn = pool.acquire(); pqxx::work tx(*conn);
        auto r = tx.exec(
          "INSERT INTO pallet_type_cft "
          "  (pallet_type_id, cft_per_pallet, effective_from, note, created_by) "
          "VALUES ($1, $2, COALESCE(NULLIF($3::text,'')::date, "
          "        (now() AT TIME ZONE 'Asia/Kolkata')::date), $4, $5) "
          "RETURNING id",
          pqxx::params{id, cft, ef, note, req.user_id});
        int cft_id = r[0][0].as<int>();
        audit(tx, "pallet_type_cft", cft_id, "PALLET_CFT_SET", req.user_id,{{"pallet_type_id", id}, {"cft_per_pallet", cft}, {"effective_from", ef}});
        tx.commit();
        res.json(201, {{"id", cft_id}});
    }));

    app.get("/pallet-types/:id/cft", requireAuth(pool, [&pool](Request& req, Response& res){
        int id; if (!idParam(req, res, "id", id)) return;
        auto conn = pool.acquire(); pqxx::work tx(*conn);
        auto x = tx.exec(
          "SELECT id, cft_per_pallet, effective_from::text AS from_date, note, created_at::text AS at "
          "FROM pallet_type_cft WHERE pallet_type_id = $1 ORDER BY effective_from DESC, id DESC",
          pqxx::params{id});
        tx.commit();
        nlohmann::json arr = nlohmann::json::array();
        for (auto i : x) arr.push_back({
            {"id", i["id"].as<long long>()},{"cft_per_pallet", i["cft_per_pallet"].as<double>()},{"effective_from", i["from_date"].as<std::string>()},
            {"note", sv(i["note"])},{"created_at", i["at"].as<std::string>()}
        });
        res.json(200, arr);
    }));

    // production
    app.post("/pallet-production", requireAuth(pool, [&pool](Request& req, Response& res){
        nlohmann::json j;
        if(!jsonBody(req, res, j)) return;

        if(!j.contains("site_id") || !j.contains("pallet_type_id") ||
            !j.contains("quantity") || !j.contains("wood_material_id")) {
            res.error(400, "need site_id, pallet_type_id, quantity, wood_material_id"); return;
        }
        int site = j["site_id"].get<int>(), pt   = j["pallet_type_id"].get<int>();
        int wood = j["wood_material_id"].get<int>(),qty  = j["quantity"].get<int>();
        std::string note = j.value("note", "");
        std::string ref  = j.value("client_ref", "");

        if(qty<=0 || qty>1000000) { res.error(400, "quantity must be 1..1000000"); return; }
        if(!ref.empty() && !isUuid(ref)) { res.error(400, "client_ref must be a UUID"); return; }

        auto conn = pool.acquire(); pqxx::work tx(*conn);
        if(!canAccessSite(tx, req, site)) { res.error(403, "no access to this site"); return; }
        setUser(tx, req.user_id);

        // idempotency
        if(!ref.empty()){
            auto dup = tx.exec("SELECT id FROM pallet_production WHERE client_ref = $1::uuid",pqxx::params{ref});
            if(!dup.empty()){
                res.json(200, {{"id", dup[0][0].as<long long>()}, {"duplicate", true}});
                return;
            }
        }
        // lock the type & wood material & verify they're active
        auto chk = tx.exec(
          "SELECT (SELECT is_active FROM pallet_types WHERE id=$1) AS pt, (SELECT is_active FROM materials   WHERE id=$2) AS w",
          pqxx::params{pt, wood});
        if(chk[0]["pt"].is_null() || !chk[0]["pt"].as<bool>() || chk[0]["w"].is_null()  || !chk[0]["w"].as<bool>()){
            res.error(400, "unknown or inactive pallet_type / wood material"); return;
        }

        auto cft_r = tx.exec("SELECT cft_for($1::int, (now() AT TIME ZONE 'Asia/Kolkata')::date) AS c",pqxx::params{pt});
        if(cft_r[0]["c"].is_null()){
            res.error(409, "no CFT factor set for this pallet type; set one first"); return;
        }
        double cft_per = cft_r[0]["c"].as<double>();
        double consumed = std::round(cft_per * qty * 1000.0) / 1000.0;

        // 1) insert production row (immutable snapshot)
        auto pr = tx.exec(
          "INSERT INTO pallet_production "
          "  (site_id, pallet_type_id, quantity, cft_per_pallet, cft_consumed, wood_material_id, note, client_ref, created_by) "
          "VALUES ($1,$2,$3,$4,$5,$6,$7,NULLIF($8,'')::uuid,$9) RETURNING id",pqxx::params{site, pt, qty, cft_per, consumed, wood, note, ref, req.user_id});
        long long prod_id = pr[0][0].as<long long>();

        // 2) debit wood via the NORMAL stock ledger — trigger refuses if insufficient
        tx.exec(
          "INSERT INTO stock_txns (site_id, material_id, type, quantity, note, created_by) VALUES ($1,$2,'USAGE',$3,$4,$5)",
          pqxx::params{site, wood, consumed,"pallet production #" + std::to_string(prod_id), req.user_id});

        // 3) credit pallets
        tx.exec(
          "INSERT INTO pallet_txns (site_id, pallet_type_id, type, quantity, note, production_id, created_by) VALUES ($1,$2,'PRODUCED',$3,$4,$5,$6)",
          pqxx::params{site, pt, qty,"pallet production #" + std::to_string(prod_id),(long long)prod_id, req.user_id});

        audit(tx, "pallet_production", prod_id, "PALLET_PRODUCED", req.user_id,{{"site_id", site}, {"pallet_type_id", pt}, {"quantity", qty},{"cft_per_pallet", cft_per}, {"cft_consumed", consumed}});
        tx.commit(); 
        res.json(201, {{"id", prod_id}, {"cft_consumed", consumed}});
    }));

    // pallet stock-in (bought finished pallets) 
    app.post("/pallet-stock-in", requireAuth(pool, [&pool](Request& req, Response& res){
        nlohmann::json j;
        if (!jsonBody(req, res, j)) return;
        if (!j.contains("site_id") || !j.contains("pallet_type_id") || !j.contains("quantity")) {
            res.error(400, "need site_id, pallet_type_id, quantity"); return;
        }
        int site = j["site_id"].get<int>();
        int pt   = j["pallet_type_id"].get<int>(),qty  = j["quantity"].get<int>();
        std::string party = j.value("party", "");
        std::string note  = j.value("note", "");
        std::string ref   = j.value("client_ref", "");
        if(qty <= 0) { res.error(400, "quantity must be > 0"); return; }
        if(!ref.empty() && !isUuid(ref)) { res.error(400, "client_ref must be a UUID"); return; }

        auto conn = pool.acquire(); pqxx::work tx(*conn);
        if(!canAccessSite(tx, req, site)) { res.error(403, "no access to this site"); return; }

        auto chk = tx.exec(
          "SELECT (SELECT is_active FROM pallet_types WHERE id=$1) AS pt, "
          "       (SELECT is_active FROM sites        WHERE id=$2) AS s",
          pqxx::params{pt, site});
        if (chk[0]["pt"].is_null() || !chk[0]["pt"].as<bool>() ||
            chk[0]["s"].is_null()  || !chk[0]["s"].as<bool>()) {
            res.error(400, "unknown or inactive pallet_type / site"); return;
        }

        auto r = tx.exec(
          "INSERT INTO pallet_txns "
          "  (site_id, pallet_type_id, type, quantity, party, note, client_ref, created_by) "
          "VALUES ($1,$2,'IN',$3,$4,$5,NULLIF($6,'')::uuid,$7) RETURNING id",
          pqxx::params{site, pt, qty, party, note, ref, req.user_id});
        audit(tx, "pallet_txns", r[0][0].as<long long>(), "PALLET_STOCK_IN",req.user_id, j);
        tx.commit();
        res.json(201, {{"id", r[0][0].as<long long>()}});
    }));

    // pallet issue / sale 
    app.post("/pallet-usage", requireAuth(pool, [&pool](Request& req, Response& res){
        nlohmann::json j;
        if(!jsonBody(req, res, j)) return;
        if(!j.contains("site_id") || !j.contains("pallet_type_id") || !j.contains("quantity")) {
            res.error(400, "need site_id, pallet_type_id, quantity"); return;
        }
        int site = j["site_id"].get<int>();
        int pt   = j["pallet_type_id"].get<int>(),qty  = j["quantity"].get<int>();
        std::string party = j.value("party", "");
        std::string note  = j.value("note", "");
        std::string ref   = j.value("client_ref", "");
        if(qty <= 0) { res.error(400, "quantity must be > 0"); return; }
        if(!ref.empty() && !isUuid(ref)) { res.error(400, "client_ref must be a UUID"); return; }

        auto conn = pool.acquire(); pqxx::work tx(*conn);
        if(!canAccessSite(tx, req, site)) { res.error(403, "no access to this site"); return; }

        auto r = tx.exec(
          "INSERT INTO pallet_txns "
          "  (site_id, pallet_type_id, type, quantity, party, note, client_ref, created_by) VALUES ($1,$2,'OUT',$3,$4,$5,NULLIF($6,'')::uuid,$7) RETURNING id",
          pqxx::params{site, pt, qty, party, note, ref, req.user_id});
        audit(tx, "pallet_txns", r[0][0].as<long long>(), "PALLET_USED",req.user_id, j);
        tx.commit();
        res.json(201, {{"id", r[0][0].as<long long>()}});
    }));

    // current pallet stock, one site 
    app.get("/sites/:id/pallets", requireAuth(pool, [&pool](Request& req, Response& res){
        int id; if (!idParam(req, res, "id", id)) return;
        auto conn = pool.acquire(); pqxx::work tx(*conn);
        if (!canAccessSite(tx, req, id)) { res.error(403, "no access to this site"); return; }
        auto x = tx.exec(
          "SELECT pallet_type_id, pallet_type, balance FROM pallet_overview "
          "WHERE site_id = $1 ORDER BY pallet_type", pqxx::params{id});
        tx.commit();
        nlohmann::json arr = nlohmann::json::array();
        for (auto i : x) arr.push_back({
            {"pallet_type_id", i["pallet_type_id"].as<int>()},{"name", i["pallet_type"].as<std::string>()},{"balance",i["balance"].as<int>()}
        });
        res.json(200, arr);
    }));

    // production history, one site 
    app.get("/sites/:id/pallet-production", requireAuth(pool, [&pool](Request& req, Response& res){
        int id; if (!idParam(req, res, "id", id)) return;
        std::string from = req.query.count("from") ? req.query["from"] : "2025-01-01";
        std::string to   = req.query.count("to")   ? req.query["to"]   : "9999-12-31";
        auto conn = pool.acquire(); pqxx::work tx(*conn);
        if(!canAccessSite(tx, req, id)) { res.error(403, "no access to this site"); return; }
        auto x = tx.exec(
          "SELECT pp.id, pt.name AS pallet_type, pp.quantity, pp.cft_per_pallet, "
          "       pp.cft_consumed, m.name AS wood, pp.txn_date::text AS date, pp.note, u.name AS by_name "
          "FROM pallet_production pp "
          "JOIN pallet_types pt ON pt.id = pp.pallet_type_id "
          "JOIN materials m    ON m.id  = pp.wood_material_id "
          "JOIN users u        ON u.id  = pp.created_by "
          "WHERE pp.site_id = $1 AND pp.txn_date >= $2::date AND pp.txn_date <= $3::date "
          "ORDER BY pp.created_at DESC, pp.id DESC LIMIT 200",
          pqxx::params{id, from, to});
        tx.commit();
        nlohmann::json arr = nlohmann::json::array();
        for (auto i : x) arr.push_back({
            {"id", i["id"].as<long long>()},{"pallet_type", i["pallet_type"].as<std::string>()},
            {"quantity", i["quantity"].as<int>()}, {"cft_per_pallet", i["cft_per_pallet"].as<double>()},
            {"cft_consumed", i["cft_consumed"].as<double>()}, {"wood", i["wood"].as<std::string>()},{"date", i["date"].as<std::string>()},
            {"note", sv(i["note"])},{"by", i["by_name"].as<std::string>()}
        });
        res.json(200, arr);
    }));

    // admin overview: pallet stock at every site 
    app.get("/admin/pallet-overview", requireAdmin(pool, [&pool](Request&, Response& res){
        auto conn = pool.acquire(); pqxx::work tx(*conn);
        auto x = tx.exec("SELECT site_id, site, pallet_type_id, pallet_type, balance FROM pallet_overview ORDER BY site_id, pallet_type");
        tx.commit();
        nlohmann::json arr = nlohmann::json::array();
        for (auto i : x) arr.push_back({
            {"site_id", i["site_id"].as<int>()}, {"site", i["site"].as<std::string>()},{"pallet_type_id", i["pallet_type_id"].as<int>()},
            {"pallet_type", i["pallet_type"].as<std::string>()},{"balance", i["balance"].as<int>()}
        });
        res.json(200, arr);
    }));


        // ---------- pallet entry history (shows which entries can still be edited) ----------
    app.get("/sites/:id/pallet-txns", requireAuth(pool, [&pool](Request& req, Response& res){
        int id; if(!idParam(req,res,"id",id)) return;
        std::string from = req.query.count("from") && !req.query["from"].empty() ? req.query["from"] : "1900-01-01";
        std::string to   = req.query.count("to")   && !req.query["to"].empty()   ? req.query["to"]   : "9999-12-31";
        int limit = 200;
        if(req.query.count("limit")){ try{ limit = std::stoi(req.query["limit"]); }catch(...){} }
        if(limit<1) limit=1;
        if(limit>500) limit=500;
        auto conn=pool.acquire(); pqxx::work tx(*conn);
        if(!canAccessSite(tx,req,id)){ res.error(403,"no access to this site"); return; }
        auto x=tx.exec(
          "SELECT t.id, pt.name AS pallet_type, t.type, t.quantity, t.party, t.note, t.txn_date::text AS date, "
          "u.name AS by_name, t.created_by, t.created_at::text AS created_at, "
          "(t.production_id IS NULL AND t.type IN ('IN','OUT') AND now() - t.created_at <= edit_window()) AS can_edit "
          "FROM pallet_txns t JOIN pallet_types pt ON pt.id=t.pallet_type_id JOIN users u ON u.id=t.created_by "
          "WHERE t.site_id=$1 AND t.txn_date >= $2::date AND t.txn_date <= $3::date "
          "ORDER BY t.created_at DESC, t.id DESC LIMIT $4", pqxx::params{id,from,to,limit});
        tx.commit();
        nlohmann::json arr=nlohmann::json::array();
        for(auto i:x){
            bool mine = i["created_by"].as<int>()==req.user_id;
            arr.push_back({{"id",i["id"].as<long long>()},{"pallet_type",i["pallet_type"].as<std::string>()},
                {"type",i["type"].as<std::string>()},{"quantity",i["quantity"].as<int>()},
                {"party",sv(i["party"])},{"note",sv(i["note"])},{"date",i["date"].as<std::string>()},
                {"by",i["by_name"].as<std::string>()},{"created_at",i["created_at"].as<std::string>()},
                {"editable", mine && i["can_edit"].as<bool>()}});
        }
        res.json(200,arr);
    }));

    // edit / delete a bought or issued pallet entry 
    app.put("/pallet-txns/:id", requireAuth(pool, [&pool](Request& req, Response& res){
        int id; if(!idParam(req,res,"id",id)) return;
        nlohmann::json j; if(!jsonBody(req,res,j)) return;
        auto conn=pool.acquire(); pqxx::work tx(*conn);
        setUser(tx,req.user_id);
        auto cur=tx.exec("SELECT quantity, note, party FROM pallet_txns WHERE id=$1",pqxx::params{id});
        if(cur.empty()){ res.error(404,"entry not found"); return; }
        int qty = j.contains("quantity") ? j["quantity"].get<int>() : cur[0]["quantity"].as<int>();
        std::string note  = j.contains("note")  ? j["note"].get<std::string>()  : (cur[0]["note"].is_null()  ? "" : cur[0]["note"].as<std::string>());
        std::string party = j.contains("party") ? j["party"].get<std::string>() : (cur[0]["party"].is_null() ? "" : cur[0]["party"].as<std::string>());
        if(qty<=0 || qty>1000000){ res.error(400,"quantity must be 1..1000000"); return; }
        tx.exec("UPDATE pallet_txns SET quantity=$2, note=$3, party=$4 WHERE id=$1",pqxx::params{id,qty,note,party});
        tx.commit();
        res.json(200,{{"ok",true}});
    }));

    app.del("/pallet-txns/:id", requireAuth(pool, [&pool](Request& req, Response& res){
        int id; if(!idParam(req,res,"id",id)) return;
        auto conn=pool.acquire(); pqxx::work tx(*conn);
        setUser(tx,req.user_id);
        auto r=tx.exec("DELETE FROM pallet_txns WHERE id=$1 RETURNING id",pqxx::params{id});
        if(r.empty()){ res.error(404,"entry not found"); return; }
        tx.commit();
        res.json(200,{{"ok",true}});
    }));

    // admin: correct pallet stock (adds a NEW row, never edits history) 
    app.post("/admin/pallet-adjustments", requireAdmin(pool, [&pool](Request& req, Response& res){
        nlohmann::json j; if(!jsonBody(req,res,j)) return;
        int site=j.value("site_id",0), pt=j.value("pallet_type_id",0), qty=j.value("quantity",0);
        std::string dir=j.value("direction",""), reason=j.value("reason","");
        if(site<=0||pt<=0||qty<=0||qty>1000000){ res.error(400,"need site_id, pallet_type_id, quantity (1..1000000)"); return; }
        if(dir!="PLUS" && dir!="MINUS"){ res.error(400,"direction must be PLUS or MINUS"); return; }
        if(reason.size()<5 || reason.size()>300){ res.error(400,"reason is required (5-300 characters)"); return; }
        auto conn=pool.acquire(); pqxx::work tx(*conn);
        setUser(tx,req.user_id);
        auto r=tx.exec("INSERT INTO pallet_txns (site_id,pallet_type_id,type,quantity,note,created_by) VALUES ($1,$2,$3,$4,$5,$6) RETURNING id",
                       pqxx::params{site,pt,std::string("ADJUST_")+dir,qty,"Correction: "+reason,req.user_id});
        audit(tx,"pallet_txns",r[0][0].as<long long>(),"PALLET_ADJUSTMENT",req.user_id,j);
        tx.commit();
        res.json(201,{{"id",r[0][0].as<long long>()}});
    }));

    //  admin: undo a whole production run (pallets out, wood back in), once 
    app.post("/admin/pallet-production/:id/reverse", requireAdmin(pool, [&pool](Request& req, Response& res){
        int id; if(!idParam(req,res,"id",id)) return;
        nlohmann::json j; if(!jsonBody(req,res,j)) return;
        std::string reason=j.value("reason","");
        if(reason.size()<5 || reason.size()>300){ res.error(400,"reason is required (5-300 characters)"); return; }
        auto conn=pool.acquire(); pqxx::work tx(*conn);
        setUser(tx,req.user_id);
        auto pr=tx.exec("SELECT site_id, pallet_type_id, quantity, wood_material_id, cft_consumed "
                        "FROM pallet_production WHERE id=$1 FOR SHARE",pqxx::params{id});
        if(pr.empty()){ res.error(404,"production entry not found"); return; }
        auto done=tx.exec("SELECT 1 FROM pallet_production_reversals WHERE production_id=$1",pqxx::params{id});
        if(!done.empty()){ res.error(409,"this production was already reversed"); return; }
        tx.exec("INSERT INTO pallet_production_reversals (production_id, reversed_by, reason) VALUES ($1,$2,$3)",pqxx::params{id,req.user_id,reason});
        std::string note="Reversal of pallet production #"+std::to_string(id)+": "+reason;
        // pallets leave stock (the database refuses this if they were already sold)
        tx.exec("INSERT INTO pallet_txns (site_id,pallet_type_id,type,quantity,note,production_id,created_by) VALUES ($1,$2,'ADJUST_MINUS',$3,$4,$5,$6)",
                pqxx::params{pr[0]["site_id"].as<int>(),pr[0]["pallet_type_id"].as<int>(),pr[0]["quantity"].as<int>(),note,id,req.user_id});
        // wood comes back
        tx.exec("INSERT INTO stock_txns (site_id,material_id,type,quantity,note,production_id,created_by) VALUES ($1,$2,'ADMIN_PLUS',$3,$4,$5,$6)",
                pqxx::params{pr[0]["site_id"].as<int>(),pr[0]["wood_material_id"].as<int>(),pr[0]["cft_consumed"].as<double>(),note,id,req.user_id});
        audit(tx,"pallet_production",id,"PALLET_PRODUCTION_REVERSED",req.user_id,j);
        tx.commit();
        res.json(200,{{"ok",true}});
    }));

    //  admin: "how much wood do I need for this plan?"  /admin/wood-plan?site_id=1&plan=1:500,2:300 
    app.get("/admin/wood-plan", requireAdmin(pool, [&pool](Request& req, Response& res){
        int site=0;
        try{ site=std::stoi(req.query.count("site_id")?req.query["site_id"]:"0"); }catch(...){}
        std::string plan=req.query.count("plan")?req.query["plan"]:"";
        if(site<=0||plan.empty()){ res.error(400,"need site_id and plan=typeId:count,typeId:count"); return; }
        std::vector<std::pair<int,int>> items;
        { std::stringstream ss(plan); std::string part;
          while(std::getline(ss,part,',')){
              auto c=part.find(':');
              if(c==std::string::npos || items.size()>=50){ res.error(400,"bad plan format"); return; }
              try{ int t=std::stoi(part.substr(0,c)), q=std::stoi(part.substr(c+1));
                   if(t<=0||q<=0||q>1000000){ throw 1; }
                   items.push_back({t,q}); }
              catch(...){ res.error(400,"bad plan format"); return; }
          } }
        auto conn=pool.acquire(); pqxx::work tx(*conn);
        nlohmann::json arr=nlohmann::json::array(); double need=0; bool missing=false;
        for(auto &it:items){
            auto r=tx.exec("SELECT name, cft_for(id,(now() AT TIME ZONE 'Asia/Kolkata')::date) AS c FROM pallet_types WHERE id=$1 AND is_active",pqxx::params{it.first});
            if(r.empty()){ res.error(400,"unknown pallet type "+std::to_string(it.first)); return; }
            if(r[0]["c"].is_null()){ missing=true; arr.push_back({{"pallet_type_id",it.first},{"name",r[0]["name"].as<std::string>()},{"pallets",it.second},{"cft_per_pallet",nullptr},{"cft_needed",nullptr}}); continue; }
            double f=r[0]["c"].as<double>(), n=std::round(f*it.second*1000.0)/1000.0; need+=n;
            arr.push_back({{"pallet_type_id",it.first},{"name",r[0]["name"].as<std::string>()},{"pallets",it.second},{"cft_per_pallet",f},{"cft_needed",n}});
        }
        auto b=tx.exec("SELECT COALESCE(SUM(cs.balance),0) AS b FROM current_stock cs JOIN materials m ON m.id=cs.material_id "
                       "WHERE cs.site_id=$1 AND upper(m.unit)='CFT'",pqxx::params{site});
        tx.commit();
        double bal=b[0]["b"].as<double>();
        res.json(200,{{"items",arr},{"total_cft_needed",need},{"wood_balance_cft",bal},
                      {"shortfall_cft",need>bal?need-bal:0.0},{"missing_factor",missing}});
    }));

    //  admin: pallet report  /admin/pallet-report?from=2026-10-01&to=2026-10-31&site_id=0 
    app.get("/admin/pallet-report", requireAdmin(pool, [&pool](Request& req, Response& res){
        std::string from=req.query.count("from")&&!req.query["from"].empty()?req.query["from"]:"1900-01-01";
        std::string to  =req.query.count("to")&&!req.query["to"].empty()?req.query["to"]:"9999-12-31";
        int site=0; try{ site=std::stoi(req.query.count("site_id")?req.query["site_id"]:"0"); }catch(...){}
        auto conn=pool.acquire(); pqxx::work tx(*conn);
        auto x=tx.exec(
          "SELECT s.name AS site, p.name AS pallet_type, "
          " COALESCE(SUM(CASE WHEN t.txn_date < $1::date THEN pallet_sign(t.type)*t.quantity END),0) AS opening, "
          " COALESCE(SUM(CASE WHEN t.txn_date BETWEEN $1::date AND $2::date AND t.type='PRODUCED' THEN t.quantity END),0) AS produced, "
          " COALESCE(SUM(CASE WHEN t.txn_date BETWEEN $1::date AND $2::date AND t.type='IN' THEN t.quantity END),0) AS bought, "
          " COALESCE(SUM(CASE WHEN t.txn_date BETWEEN $1::date AND $2::date AND t.type='OUT' THEN t.quantity END),0) AS issued, "
          " COALESCE(SUM(CASE WHEN t.txn_date BETWEEN $1::date AND $2::date AND t.type IN ('ADJUST_PLUS','ADJUST_MINUS') "
          "                   THEN pallet_sign(t.type)*t.quantity END),0) AS adjusted, "
          " COALESCE(SUM(CASE WHEN t.txn_date <= $2::date THEN pallet_sign(t.type)*t.quantity END),0) AS closing "
          "FROM pallet_txns t JOIN sites s ON s.id=t.site_id JOIN pallet_types p ON p.id=t.pallet_type_id "
          "WHERE ($3 = 0 OR t.site_id=$3) GROUP BY s.name, p.name ORDER BY s.name, p.name",pqxx::params{from,to,site});
        auto w=tx.exec(
          "SELECT s.name AS site, COALESCE(SUM(pp.cft_consumed),0) AS cft FROM pallet_production pp JOIN sites s ON s.id=pp.site_id "
          "WHERE pp.txn_date BETWEEN $1::date AND $2::date AND ($3 = 0 OR pp.site_id=$3) "
          "AND NOT EXISTS (SELECT 1 FROM pallet_production_reversals r WHERE r.production_id=pp.id) GROUP BY s.name ORDER BY s.name",
          pqxx::params{from,to,site});
        tx.commit();
        nlohmann::json rows=nlohmann::json::array(), wood=nlohmann::json::array();
        for(auto i:x) rows.push_back({{"site",i["site"].as<std::string>()},{"pallet_type",i["pallet_type"].as<std::string>()},
            {"opening",i["opening"].as<int>()},{"produced",i["produced"].as<int>()},{"bought",i["bought"].as<int>()},
            {"issued",i["issued"].as<int>()},{"adjusted",i["adjusted"].as<int>()},{"closing",i["closing"].as<int>()}});
        for(auto i:w) wood.push_back({{"site",i["site"].as<std::string>()},{"wood_used_cft",i["cft"].as<double>()}});
        res.json(200,{{"pallets",rows},{"wood_used",wood}});
    }));

}