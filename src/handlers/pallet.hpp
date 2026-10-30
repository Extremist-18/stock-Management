#pragma once
#include "auth/guard.hpp"
#include <regex>

inline bool isUuid(const std::string& s){
    static const std::regex re("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$");
    return std::regex_match(s, re);
}

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
        if (!jsonBody(req, res, j)) return;
        std::string name = j.value("name", "");
        if (name.empty() || name.size() > 100) {
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
}