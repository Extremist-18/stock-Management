#pragma once
#include <set>
#include <vector>
#include "auth/guard.hpp"

// body: {site_id, supplier, vehicle?, note?, client_ref?, items:[{kind:"material"|"pallet", id, quantity}]}
inline void registerReceiptRoutes(App& app, Pool& pool){
    app.post("/receipts", requireAuth(pool, [&pool](Request& req, Response& res){
        nlohmann::json j; if(!jsonBody(req, res, j)) return;
        if(!j.contains("site_id") || !j["site_id"].is_number_integer() || !j.contains("items") || !j["items"].is_array() || j["items"].empty() || j["items"].size() > 30){
            res.error(400, "need site_id and items: 1 to 30 lines"); return;
        }
        int site = j["site_id"].get<int>();
        std::string supplier = trim(j.value("supplier", "")), vehicle = trim(j.value("vehicle", ""));
        std::string note = trim(j.value("note", "")), ref = j.value("client_ref", "");
        if(supplier.empty() || supplier.size()>100){ res.error(400, "supplier name is required (max 100 characters)"); return; }
        if(vehicle.size()>30){ res.error(400, "vehicle number is too long (max 30 characters)"); return; }
        if(note.size()>300){ res.error(400, "note is too long (max 300 characters)"); return; }
        if(!ref.empty() && !isUuid(ref)){ res.error(400, "client_ref must be a UUID"); return; }

        struct Line { bool pallet; int id; double qty;};
        std::vector<Line> lines;
        std::set<std::pair<bool,int>> seen;
        for(const auto& it:j["items"]){
            if(!it.is_object() || !it.contains("kind") || !it["kind"].is_string() || !it.contains("id") || !it["id"].is_number_integer() || !it.contains("quantity") || !it["quantity"].is_number()){
                res.error(400, "each line needs kind, id and quantity"); return;
            }
            std::string kind = it["kind"].get<std::string>();
            if(kind!="material" && kind!="pallet"){ res.error(400, "kind must be material or pallet"); return; }
            bool pallet = kind == "pallet";
            double q = it["quantity"].get<double>();
            if(!(q>0) || q>1000000){ res.error(400, "quantity must be more than 0 (max 1000000)"); return; }
            if(pallet && !it["quantity"].is_number_integer()){ res.error(400, "pallets must be a whole number"); return; }
            int id = it["id"].get<int>();
            if(!seen.insert({pallet,id}).second){ res.error(400, "the same item is listed twice; add the two amounts together"); return; }
            lines.push_back({pallet,id,q});
        }

        std::string fullNote = vehicle.empty() ? note : ("Truck " + vehicle + (note.empty() ? "" : " - " + note));
        auto conn = pool.acquire(); pqxx::work tx(*conn);
        if(!canAccessSite(tx, req, site)){ res.error(403, "no access to this site"); return; }
        setUser(tx, req.user_id);
        auto s = tx.exec("SELECT is_active FROM sites WHERE id=$1", pqxx::params{site});
        if(s.empty() || !s[0][0].as<bool>()){ res.error(400, "unknown or inactive site"); return; }

        if(!ref.empty()){   // same delivery sent twice (double tap or bad network): do not save twice
            auto d = tx.exec("SELECT created_by FROM stock_txns WHERE client_ref=$1::uuid UNION ALL SELECT created_by FROM pallet_txns WHERE client_ref=$1::uuid", pqxx::params{ref});
            if(!d.empty()){
                if(d[0][0].as<int>() != req.user_id){ res.error(409, "client_ref already used"); return; }
                res.json(200, {{"duplicate", true}}); return;
            }
        }
        long long firstId = 0; bool firstPallet = false;
        for(size_t k=0;k<lines.size(); k++){
            const Line& l = lines[k];
            std::string r = k == 0 ? ref : std::string();    // the ref sits on the first row only
            pqxx::result ins;
            if(l.pallet){
                auto c = tx.exec("SELECT is_active FROM pallet_types WHERE id=$1", pqxx::params{l.id});
                if(c.empty() || !c[0][0].as<bool>()){ res.error(400, "unknown or switched-off pallet size"); return; }
                ins = tx.exec("INSERT INTO pallet_txns (site_id, pallet_type_id, type, quantity, party, note, client_ref, created_by) VALUES ($1,$2,'IN',$3,$4,NULLIF($5,''),NULLIF($6,'')::uuid,$7) RETURNING id",pqxx::params{site, l.id, (int)l.qty, supplier, fullNote, r, req.user_id});
            }else{
                auto c = tx.exec("SELECT is_active FROM materials WHERE id=$1", pqxx::params{l.id});
                if(c.empty() || !c[0][0].as<bool>()){ res.error(400, "unknown or switched-off material"); return; }
                ins = tx.exec("INSERT INTO stock_txns (site_id, material_id, type, quantity, party, note, client_ref, created_by) "
                "VALUES ($1,$2,'IN',$3,$4,NULLIF($5,''),NULLIF($6,'')::uuid,$7) RETURNING id",pqxx::params{site, l.id, l.qty, supplier, fullNote, r, req.user_id});
            }
            if(k==0){ firstId = ins[0][0].as<long long>(); firstPallet = l.pallet; }
        }
        audit(tx, firstPallet ? "pallet_txns" : "stock_txns", firstId, "RECEIPT", req.user_id, j);
        tx.commit();
        res.json(201, {{"saved", (int)lines.size()}});
    }));

    // ONE log for the site: material entries and pallet entries together, newest first
    app.get("/sites/:id/log", requireAuth(pool, [&pool](Request& req, Response& res){
        int id; if(!idParam(req, res, "id", id)) return;
        int limit = 60;
        if(req.query.count("limit")){ try{ limit = std::stoi(req.query["limit"]); }catch(...){} }
        if(limit<1) limit = 1;
        if(limit>300) limit = 300;
        auto conn = pool.acquire(); pqxx::work tx(*conn);
        if(!canAccessSite(tx, req, id)){ res.error(403, "no access to this site"); return; }
        auto x = tx.exec(
          "SELECT * FROM ("
          " SELECT 'material' AS kind, t.id, m.name AS item, m.unit, t.type, t.quantity::float8 AS quantity, t.party, t.note, "
          "  t.txn_date::text AS date, t.created_at, u.name AS by_name, t.created_by, "
          "  (t.transfer_ref IS NULL AND t.production_id IS NULL AND t.packing_id IS NULL AND now() - t.created_at <= edit_window()) AS can_edit "
          " FROM stock_txns t JOIN materials m ON m.id = t.material_id JOIN users u ON u.id = t.created_by WHERE t.site_id = $1 UNION ALL "
          " SELECT 'pallet', t.id, p.name, 'pcs', t.type, t.quantity::float8, t.party, t.note, t.txn_date::text, t.created_at, u.name, t.created_by, "
          "  (t.production_id IS NULL AND t.type IN ('IN','OUT') AND now() - t.created_at <= edit_window()) "
          " FROM pallet_txns t JOIN pallet_types p ON p.id = t.pallet_type_id JOIN users u ON u.id = t.created_by WHERE t.site_id = $1) l ORDER BY l.created_at DESC, l.id DESC LIMIT $2", pqxx::params{id, limit});
        tx.commit();
        nlohmann::json arr = nlohmann::json::array();
        for(auto i : x)
            arr.push_back({{"kind", i["kind"].as<std::string>()}, {"id", i["id"].as<long long>()}, {"item", i["item"].as<std::string>()},{"unit", i["unit"].as<std::string>()}, {"type", i["type"].as<std::string>()}, {"quantity", i["quantity"].as<double>()},
                {"party", nullableText(i["party"])}, {"note", nullableText(i["note"])}, {"date", i["date"].as<std::string>()},{"by", i["by_name"].as<std::string>()},
                {"editable", i["created_by"].as<int>() == req.user_id && i["can_edit"].as<bool>()}});
        res.json(200, arr);
    }));
}