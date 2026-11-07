#pragma once
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include "auth/guard.hpp"

inline std::string fmtNum(double v){
    char b[48]; std::snprintf(b, sizeof b, "%.3f", v);
    std::string s = b;
    while(!s.empty() && s.back() == '0') s.pop_back();
    if(!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

// reads items: [{material_id, qty_per_pallet}, ...]. On a bad list it sends the error and returns false.
inline bool readItems(const nlohmann::json& j, Response& res, std::vector<std::pair<int,double>>& out){
    if(!j.contains("items") || !j["items"].is_array() || j["items"].empty() || j["items"].size() > 30){
        res.error(400, "items: 1 to 30 materials are required"); return false;
    }
    std::map<int,bool> seen;
    for(const auto& it : j["items"]){
        if(!it.is_object() || !it.contains("material_id") || !it["material_id"].is_number_integer() ||
           !it.contains("qty_per_pallet") || !it["qty_per_pallet"].is_number()){
            res.error(400, "each material needs a quantity per pallet"); return false;
        }
        int m = it["material_id"].get<int>();
        double q = it["qty_per_pallet"].get<double>();
        if(m <= 0 || !(q >= 0.000001 && q <= 100000)){ res.error(400, "invalid material or quantity per pallet"); return false; }
        if(seen[m]){ res.error(400, "a material is listed twice"); return false; }
        seen[m] = true;
        out.push_back({m, q});
    }
    return true;
}

inline void registerPackingRoutes(App& app, Pool& pool){

    // ---------- recipes ----------
    app.get("/packing-specs", requireAuth(pool, [&pool](Request& req, Response& res){
        const bool all = req.role == "admin" && req.query.count("all") && req.query["all"] == "1";
        auto conn = pool.acquire(); pqxx::work tx(*conn);
        auto s = tx.exec(
          "SELECT s.id, s.name, s.is_active, s.pallet_type_id, pt.name AS pallet_type "
          "FROM packing_specs s JOIN pallet_types pt ON pt.id = s.pallet_type_id "
          "WHERE ($1 OR (s.is_active AND pt.is_active)) ORDER BY pt.name, s.name", pqxx::params{all});
        auto it = tx.exec(
          "SELECT i.spec_id, i.material_id, m.name, m.unit, i.qty_per_pallet::text AS q "
          "FROM packing_spec_items i JOIN materials m ON m.id = i.material_id ORDER BY m.name");
        tx.commit();
        std::map<int, nlohmann::json> items;
        for(auto i : it)
            items[i["spec_id"].as<int>()].push_back({{"material_id", i["material_id"].as<int>()},
                {"material", i["name"].as<std::string>()}, {"unit", i["unit"].as<std::string>()},
                {"qty_per_pallet", i["q"].as<double>()}});
        nlohmann::json arr = nlohmann::json::array();
        for(auto i : s){
            int id = i["id"].as<int>();
            arr.push_back({{"id", id}, {"name", i["name"].as<std::string>()},
                {"pallet_type_id", i["pallet_type_id"].as<int>()}, {"pallet_type", i["pallet_type"].as<std::string>()},
                {"is_active", i["is_active"].as<bool>()},
                {"items", items.count(id) ? items[id] : nlohmann::json::array()}});
        }
        res.json(200, arr);
    }));

    app.post("/packing-specs", requireAdmin(pool, [&pool](Request& req, Response& res){
        nlohmann::json j; if(!jsonBody(req, res, j)) return;
        std::string name = j.value("name", "");
        if(name.empty() || name.size() > 100){ res.error(400, "name required (max 100 chars)"); return; }
        if(!j.contains("pallet_type_id") || !j["pallet_type_id"].is_number_integer()){ res.error(400, "pallet_type_id required"); return; }
        std::vector<std::pair<int,double>> items;
        if(!readItems(j, res, items)) return;
        auto conn = pool.acquire(); pqxx::work tx(*conn);
        auto r = tx.exec("INSERT INTO packing_specs (name, pallet_type_id) VALUES ($1,$2) RETURNING id",
                         pqxx::params{name, j["pallet_type_id"].get<int>()});
        int id = r[0][0].as<int>();
        for(auto& it : items)
            tx.exec("INSERT INTO packing_spec_items (spec_id, material_id, qty_per_pallet) VALUES ($1,$2,ROUND($3::numeric,6))",
                    pqxx::params{id, it.first, it.second});
        audit(tx, "packing_specs", id, "PACKING_SPEC_CREATED", req.user_id, j);
        tx.commit();
        res.json(201, {{"id", id}});
    }));

    // change name, switch on/off, and/or replace the whole materials list
    app.put("/packing-specs/:id", requireAdmin(pool, [&pool](Request& req, Response& res){
        int id; if(!idParam(req, res, "id", id)) return;
        nlohmann::json j; if(!jsonBody(req, res, j)) return;
        std::vector<std::pair<int,double>> items;
        bool newItems = j.contains("items");
        if(newItems && !readItems(j, res, items)) return;
        auto conn = pool.acquire(); pqxx::work tx(*conn);
        auto cur = tx.exec("SELECT name, is_active FROM packing_specs WHERE id=$1 FOR UPDATE", pqxx::params{id});
        if(cur.empty()){ res.error(404, "packing recipe not found"); return; }
        std::string name = j.contains("name") ? j["name"].get<std::string>() : cur[0]["name"].as<std::string>();
        bool active = j.contains("is_active") ? j["is_active"].get<bool>() : cur[0]["is_active"].as<bool>();
        if(name.empty() || name.size() > 100){ res.error(400, "name required (max 100 chars)"); return; }
        tx.exec("UPDATE packing_specs SET name=$2, is_active=$3 WHERE id=$1", pqxx::params{id, name, active});
        if(newItems){
            tx.exec("DELETE FROM packing_spec_items WHERE spec_id=$1", pqxx::params{id});
            for(auto& it : items)
                tx.exec("INSERT INTO packing_spec_items (spec_id, material_id, qty_per_pallet) VALUES ($1,$2,ROUND($3::numeric,6))",
                        pqxx::params{id, it.first, it.second});
        }
        audit(tx, "packing_specs", id, "PACKING_SPEC_UPDATED", req.user_id, j);
        tx.commit();
        res.json(200, {{"ok", true}});
    }));

    // ---------- packing: body {site_id, entries:[{spec_id, quantity}], note?, client_ref?} ----------
    // everything saves together, or nothing does
    app.post("/packing", requireAuth(pool, [&pool](Request& req, Response& res){
        nlohmann::json j; if(!jsonBody(req, res, j)) return;
        if(!j.contains("site_id") || !j["site_id"].is_number_integer() || !j.contains("entries") ||
           !j["entries"].is_array() || j["entries"].empty() || j["entries"].size() > 50){
            res.error(400, "need site_id and entries: [{spec_id, quantity}]"); return;
        }
        int site = j["site_id"].get<int>();
        std::string note = j.value("note", ""), ref = j.value("client_ref", "");
        if(!ref.empty() && !isUuid(ref)){ res.error(400, "client_ref must be a UUID"); return; }

        std::vector<std::pair<int,int>> entries;
        std::map<int,bool> seen;
        for(const auto& e : j["entries"]){
            if(!e.is_object() || !e.contains("spec_id") || !e["spec_id"].is_number_integer() ||
               !e.contains("quantity") || !e["quantity"].is_number_integer()){
                res.error(400, "each entry needs whole numbers spec_id and quantity"); return;
            }
            int sp = e["spec_id"].get<int>(), q = e["quantity"].get<int>();
            if(sp <= 0 || q <= 0 || q > 1000000){ res.error(400, "quantity must be 1..1000000"); return; }
            if(seen[sp]){ res.error(400, "a recipe is listed twice"); return; }
            seen[sp] = true;
            entries.push_back({sp, q});
        }

        auto conn = pool.acquire(); pqxx::work tx(*conn);
        if(!canAccessSite(tx, req, site)){ res.error(403, "no access to this site"); return; }
        setUser(tx, req.user_id);

        if(!ref.empty()){   // same request sent twice: return the first result
            auto dup = tx.exec("SELECT id FROM packing_runs WHERE client_ref=$1::uuid", pqxx::params{ref});
            if(!dup.empty()){ res.json(200, {{"duplicate", true}}); return; }
        }
        auto act = tx.exec("SELECT is_active FROM sites WHERE id=$1", pqxx::params{site});
        if(act.empty() || !act[0][0].as<bool>()){ res.error(400, "unknown or inactive site"); return; }

        struct Line { int mat; std::string name, unit, q, need; };
        struct Need { std::string name, unit; double need = 0, have = 0; };
        std::map<int, Need> total;
        std::vector<std::vector<Line>> plan(entries.size());
        std::vector<std::string> specName(entries.size());
        std::vector<int> specPallet(entries.size());

        // 1) work out what each recipe uses and add it up per material
        for(size_t k = 0; k < entries.size(); k++){
            auto sp = tx.exec("SELECT s.name, s.pallet_type_id FROM packing_specs s JOIN pallet_types pt ON pt.id = s.pallet_type_id "
                              "WHERE s.id=$1 AND s.is_active AND pt.is_active", pqxx::params{entries[k].first});
            if(sp.empty()){ res.error(400, "unknown or switched-off packing recipe"); return; }
            specName[k] = sp[0]["name"].as<std::string>();
            specPallet[k] = sp[0]["pallet_type_id"].as<int>();
            auto li = tx.exec(
              "SELECT i.material_id, m.name, m.unit, i.qty_per_pallet::text AS q, "
              "       ROUND($2::numeric * i.qty_per_pallet, 3)::text AS need, COALESCE(cs.balance,0)::text AS bal "
              "FROM packing_spec_items i JOIN materials m ON m.id = i.material_id "
              "LEFT JOIN current_stock cs ON cs.site_id = $3 AND cs.material_id = i.material_id "
              "WHERE i.spec_id = $1 ORDER BY m.name", pqxx::params{entries[k].first, entries[k].second, site});
            if(li.empty()){ res.error(400, "this recipe has no materials: " + specName[k]); return; }
            for(auto r : li){
                std::string mname = r["name"].as<std::string>(), munit = r["unit"].as<std::string>();
                double need = std::stod(r["need"].as<std::string>());
                if(need <= 0){
                    res.error(400, mname + ": the amount rounds to zero. Use a smaller unit (for example kg instead of tons)."); return;
                }
                int mid = r["material_id"].as<int>();
                Need& t = total[mid];
                t.name = mname; t.unit = munit; t.need += need; t.have = std::stod(r["bal"].as<std::string>());
                plan[k].push_back({mid, mname, munit, r["q"].as<std::string>(), r["need"].as<std::string>()});
            }
        }

        // 2) stop with a clear message if anything is short
        std::string lack;
        for(auto& kv : total)
            if(kv.second.need > kv.second.have + 1e-9)
                lack += (lack.empty() ? "" : "; ") + kv.second.name + ": need " + fmtNum(kv.second.need) + " " +
                        kv.second.unit + ", in stock " + fmtNum(kv.second.have);
        if(!lack.empty()){ res.error(409, "Not enough stock. " + lack); return; }

        // 3) save the runs and take the materials out of stock
        nlohmann::json out = nlohmann::json::array();
        for(size_t k = 0; k < entries.size(); k++){
            auto run = tx.exec(
              "INSERT INTO packing_runs (site_id, spec_id, spec_name, pallet_type_id, pallets, note, client_ref, created_by) "
              "VALUES ($1,$2,$3,$4,$5,$6,NULLIF($7,'')::uuid,$8) RETURNING id",
              pqxx::params{site, entries[k].first, specName[k], specPallet[k], entries[k].second, note,
                           k == 0 ? ref : std::string(), req.user_id});
            long long rid = run[0][0].as<long long>();
            for(auto& l : plan[k]){
                tx.exec("INSERT INTO packing_run_lines (run_id, material_id, qty_per_pallet, consumed) VALUES ($1,$2,$3::numeric,$4::numeric)",
                        pqxx::params{rid, l.mat, l.q, l.need});
                tx.exec("INSERT INTO stock_txns (site_id, material_id, type, quantity, note, packing_id, created_by) "
                        "VALUES ($1,$2,'USAGE',$3::numeric,$4,$5,$6)",
                        pqxx::params{site, l.mat, l.need, "packing #" + std::to_string(rid), rid, req.user_id});
            }
            audit(tx, "packing_runs", rid, "PACKING_DONE", req.user_id,
                  {{"spec_id", entries[k].first}, {"pallets", entries[k].second}});
            out.push_back({{"id", rid}, {"spec", specName[k]}, {"pallets", entries[k].second}});
        }
        tx.commit();
        res.json(201, {{"runs", out}});
    }));

    // ---------- history of one site ----------
    app.get("/sites/:id/packing", requireAuth(pool, [&pool](Request& req, Response& res){
        int id; if(!idParam(req, res, "id", id)) return;
        auto conn = pool.acquire(); pqxx::work tx(*conn);
        if(!canAccessSite(tx, req, id)){ res.error(403, "no access to this site"); return; }
        auto x = tx.exec(
          "SELECT r.id, r.spec_name, r.pallets, r.txn_date::text AS date, u.name AS by_name, "
          "  EXISTS(SELECT 1 FROM packing_reversals v WHERE v.run_id = r.id) AS reversed, "
          "  (SELECT json_agg(json_build_object('name', m.name, 'unit', m.unit, 'consumed', l.consumed) ORDER BY m.name) "
          "   FROM packing_run_lines l JOIN materials m ON m.id = l.material_id WHERE l.run_id = r.id)::text AS lines "
          "FROM packing_runs r JOIN users u ON u.id = r.created_by WHERE r.site_id = $1 "
          "ORDER BY r.created_at DESC, r.id DESC LIMIT 30", pqxx::params{id});
        tx.commit();
        nlohmann::json arr = nlohmann::json::array();
        for(auto i : x)
            arr.push_back({{"id", i["id"].as<long long>()}, {"spec", i["spec_name"].as<std::string>()},
                {"pallets", i["pallets"].as<int>()}, {"date", i["date"].as<std::string>()}, {"by", i["by_name"].as<std::string>()},
                {"reversed", i["reversed"].as<bool>()},
                {"lines", i["lines"].is_null() ? nlohmann::json::array() : nlohmann::json::parse(i["lines"].as<std::string>())}});
        res.json(200, arr);
    }));

    // ---------- admin: undo a packing run (materials go back into stock), once ----------
    app.post("/admin/packing/:id/reverse", requireAdmin(pool, [&pool](Request& req, Response& res){
        int id; if(!idParam(req, res, "id", id)) return;
        nlohmann::json j; if(!jsonBody(req, res, j)) return;
        std::string reason = j.value("reason", "");
        if(reason.size() < 5 || reason.size() > 300){ res.error(400, "reason is required (5-300 characters)"); return; }
        auto conn = pool.acquire(); pqxx::work tx(*conn);
        setUser(tx, req.user_id);
        auto run = tx.exec("SELECT site_id FROM packing_runs WHERE id=$1 FOR SHARE", pqxx::params{id});
        if(run.empty()){ res.error(404, "packing entry not found"); return; }
        if(!tx.exec("SELECT 1 FROM packing_reversals WHERE run_id=$1", pqxx::params{id}).empty()){
            res.error(409, "this packing was already reversed"); return;
        }
        tx.exec("INSERT INTO packing_reversals (run_id, reversed_by, reason) VALUES ($1,$2,$3)", pqxx::params{id, req.user_id, reason});
        std::string note = "Reversal of packing #" + std::to_string(id) + ": " + reason;
        auto lines = tx.exec("SELECT material_id, consumed::text AS c FROM packing_run_lines WHERE run_id=$1", pqxx::params{id});
        for(auto l : lines)
            tx.exec("INSERT INTO stock_txns (site_id, material_id, type, quantity, note, packing_id, created_by) "
                    "VALUES ($1,$2,'ADMIN_PLUS',$3::numeric,$4,$5,$6)",
                    pqxx::params{run[0]["site_id"].as<int>(), l["material_id"].as<int>(), l["c"].as<std::string>(), note, id, req.user_id});
        audit(tx, "packing_runs", id, "PACKING_REVERSED", req.user_id, j);
        tx.commit();
        res.json(200, {{"ok", true}});
    }));
}