#pragma once
#include <algorithm>
#include <cmath>
#include <regex>
#include <vector>
#include "auth/guard.hpp"

inline constexpr int    kDefaultLeadDays   = 3;
inline constexpr int    kDefaultSafetyDays = 2;
inline constexpr int    kForecastWindow    = 14;   // days of usage that the daily average is based on
inline constexpr double kDefaultCoverDays  = 7;    // a delivery should last this many days beyond the safety days

inline double round3(double x){ return std::round(x*1000.0)/1000.0; }
inline double round1(double x){ return std::round(x*10.0)/10.0; }

inline std::string csvText(std::string s){
    if(!s.empty() && (s[0]=='=' || s[0]=='+' || s[0]=='-' || s[0]=='@')) s = "'" + s;
    if(s.find_first_of(",\"\r\n")==std::string::npos) return s;
    std::string o="\"";
    for(char c:s){ if(c=='"') o+="\"\""; else o+=c; }
    o+="\"";
    return o;
}

inline void registerReportRoutes(App &app, Pool &pool){
    app.get("/admin/overview",requireAdmin(pool,[&pool](Request &,Response &res){
        auto conn=pool.acquire();
        pqxx::work tx(*conn);
        auto x=tx.exec("SELECT site_id, site, material_id, material, unit, balance FROM stock_overview ORDER BY site_id, material");
        tx.commit();
        nlohmann::json arr=nlohmann::json::array();
        for(auto i:x)
            arr.push_back({{"site_id",i["site_id"].as<int>()},{"site",i["site"].as<std::string>()},
                {"material_id",i["material_id"].as<int>()},{"material",i["material"].as<std::string>()},
                {"unit",i["unit"].as<std::string>()},{"balance",i["balance"].as<double>()}});
        res.json(200,arr);
    }));

    // Set lead time + safety days for one site+material (insert or update)
    app.put("/admin/reorder-settings",requireAdmin(pool,[&pool](Request &req,Response &res){
        nlohmann::json j;
        if(!jsonBody(req,res,j)) return;
        for(const char* k:{"site_id","material_id","lead_time_days","safety_days"})
            if(!j.contains(k) || !j[k].is_number_integer()){ res.error(400,"need whole numbers: site_id, material_id, lead_time_days, safety_days"); return; }
        long long site=j["site_id"].get<long long>(), mat=j["material_id"].get<long long>();
        long long lead=j["lead_time_days"].get<long long>(), safety=j["safety_days"].get<long long>();
        if(site<=0||site>2147483647LL||mat<=0||mat>2147483647LL){ res.error(400,"invalid site_id or material_id"); return; }
        if(lead<0||lead>365||safety<0||safety>365){ res.error(400,"lead_time_days and safety_days must be 0-365"); return; }
        auto conn=pool.acquire();
        pqxx::work tx(*conn);
        tx.exec("INSERT INTO reorder_settings (site_id,material_id,lead_time_days,safety_days) VALUES ($1,$2,$3,$4) "
                "ON CONFLICT (site_id,material_id) DO UPDATE SET lead_time_days=EXCLUDED.lead_time_days, "
                "safety_days=EXCLUDED.safety_days, updated_at=now()",
                pqxx::params{(int)site,(int)mat,(int)lead,(int)safety});
        audit(tx,"reorder_settings",site,"REORDER_SETTINGS_SET",req.user_id,j);
        tx.commit();
        res.json(200,{{"ok",true}});
    }));

    // "What do I need to bring?"  ?site_id=1  ?cover_days=7
    //   avg_daily  = USAGE of the last 14 days / days observed (fewer than 14 if the stock is newer than that)
    //   days_left  = balance / avg_daily
    //   status     = RED   : days_left <= lead_time       (will run out before a delivery could arrive)
    //                AMBER : days_left <= lead + safety
    //                GREEN : otherwise
    //                NO_USAGE: nothing used in the window, so no forecast
    //   order_qty  = avg_daily * (lead + safety + cover_days) - balance, never below 0
    app.get("/admin/forecast",requireAdmin(pool,[&pool](Request &req,Response &res){
        int siteFilter=0;
        double cover=kDefaultCoverDays;
        if(req.query.count("site_id") && !req.query["site_id"].empty()){
            try{ siteFilter=std::stoi(req.query["site_id"]); }catch(...){ res.error(400,"invalid site_id"); return; }
        }
        if(req.query.count("cover_days") && !req.query["cover_days"].empty()){
            try{ cover=std::stod(req.query["cover_days"]); }catch(...){ res.error(400,"invalid cover_days"); return; }
            if(!(cover>=0 && cover<=90)){ res.error(400,"cover_days must be 0-90"); return; }
        }

        auto conn=pool.acquire();
        pqxx::work tx(*conn);
        auto x=tx.exec(
            "WITH today AS (SELECT (now() AT TIME ZONE 'Asia/Kolkata')::date AS d), "
            "u AS (SELECT site_id, material_id, "
            "  COALESCE(SUM(quantity) FILTER (WHERE type='USAGE' AND txn_date > (SELECT d FROM today) - $1::int),0) AS used, "
            "  MIN(txn_date) AS first_date FROM stock_txns GROUP BY site_id, material_id) "
            "SELECT o.site_id, o.site, o.material_id, o.material, o.unit, o.balance, "
            "  rs.lead_time_days AS lead, rs.safety_days AS safety, u.used AS used, "
            " LEAST($1::int, GREATEST(1, (SELECT d FROM today) - u.first_date + 1)) AS days_observed FROM stock_overview o "
            "LEFT JOIN reorder_settings rs ON rs.site_id=o.site_id AND rs.material_id=o.material_id "
            "LEFT JOIN u ON u.site_id=o.site_id AND u.material_id=o.material_id "
            "WHERE ($2::int = 0 OR o.site_id = $2::int) ORDER BY o.site_id, o.material",
            pqxx::params{kForecastWindow,siteFilter});
        tx.commit();

        struct Row{ nlohmann::json j; int rank; double daysLeft; };
        std::vector<Row> rows;
        for(auto i:x){
            double bal=i["balance"].as<double>();
            bool configured=!i["lead"].is_null();
            int lead   = configured?i["lead"].as<int>():kDefaultLeadDays;
            int safety = configured?i["safety"].as<int>():kDefaultSafetyDays;
            double used= i["used"].is_null()?0.0:i["used"].as<double>();
            int observed=std::max(1,i["days_observed"].as<int>());
            double avg=used/observed;

            nlohmann::json daysLeft=nullptr;
            std::string status="NO_USAGE";
            int rank=3;
            double dl=1e18;
            double order=0;
            if(avg>0){
                dl=bal/avg;
                daysLeft=round1(dl);
                if(dl<=lead){ status="RED"; rank=0; }
                else if(dl<=lead+safety){ status="AMBER"; rank=1; }
                else { status="GREEN"; rank=2; }
                order=std::max(0.0, avg*(lead+safety+cover)-bal);
                order=std::ceil(order*100.0)/100.0;
            }
            rows.push_back({{
                {"site_id",i["site_id"].as<int>()},{"site",i["site"].as<std::string>()},
                {"material_id",i["material_id"].as<int>()},{"material",i["material"].as<std::string>()},
                {"unit",i["unit"].as<std::string>()},{"balance",bal},
                {"avg_daily_usage",round3(avg)},{"days_observed",observed},
                {"days_left",daysLeft},{"lead_time_days",lead},{"safety_days",safety},{"configured",configured},
                {"status",status},{"suggested_order_qty",order}},rank,dl});
        }
        // most urgent first
        std::stable_sort(rows.begin(),rows.end(),[](const Row&a,const Row&b){
            if(a.rank!=b.rank) return a.rank<b.rank;
            return a.daysLeft<b.daysLeft;
        });
        nlohmann::json arr=nlohmann::json::array();
        for(auto &r:rows) arr.push_back(r.j);
        res.json(200,{{"window_days",kForecastWindow},{"cover_days",cover},{"items",arr}});
    }));

    // Stock report for a date range, per site + material.
    //   ?from=YYYY-MM-DD&to=YYYY-MM-DD (default: last 30 days)   ?site_id=1   ?format=csv
    //   opening + stock_in - usage + transfer_in - transfer_out + adjustments + other = closing
    app.get("/admin/report",requireAdmin(pool,[&pool](Request &req,Response &res){
        std::string from=req.query.count("from")?req.query["from"]:"";
        std::string to  =req.query.count("to")?req.query["to"]:"";
        std::string format=req.query.count("format")?req.query["format"]:"";
        int siteFilter=0;
        if(!from.empty() && !isIsoDate(from)){ res.error(400,"from must be YYYY-MM-DD"); return; }
        if(!to.empty() && !isIsoDate(to)){ res.error(400,"to must be YYYY-MM-DD"); return; }
        if(!from.empty() && !to.empty() && from>to){ res.error(400,"from is after to"); return; }
        if(!format.empty() && format!="csv" && format!="json"){ res.error(400,"format must be csv or json"); return; }
        if(req.query.count("site_id") && !req.query["site_id"].empty()){
            try{ siteFilter=std::stoi(req.query["site_id"]); }catch(...){ res.error(400,"invalid site_id"); return; }
        }

        auto conn=pool.acquire();
        pqxx::work tx(*conn);
        const std::string inRange="x.txn_date >= p.f AND x.txn_date <= p.t";
        std::string q=
            "WITH p AS (SELECT COALESCE(NULLIF($1::text,'')::date,(now() AT TIME ZONE 'Asia/Kolkata')::date - 29) AS f, "
            "                  COALESCE(NULLIF($2::text,'')::date,(now() AT TIME ZONE 'Asia/Kolkata')::date) AS t) "
            "SELECT o.site_id, o.site, o.material_id, o.material, o.unit, p.f::text AS from_date, p.t::text AS to_date, "
            "COALESCE(SUM(txn_sign(x.type)*x.quantity) FILTER (WHERE x.txn_date < p.f),0) AS opening, "
            "COALESCE(SUM(x.quantity) FILTER (WHERE x.type='IN' AND "+inRange+"),0) AS stock_in, "
            "COALESCE(SUM(x.quantity) FILTER (WHERE x.type='USAGE' AND "+inRange+"),0) AS usage, "
            "COALESCE(SUM(x.quantity) FILTER (WHERE x.type='TRANSFER_IN' AND "+inRange+"),0) AS transfer_in, "
            "COALESCE(SUM(x.quantity) FILTER (WHERE x.type='TRANSFER_OUT' AND "+inRange+"),0) AS transfer_out, "
            "COALESCE(SUM(txn_sign(x.type)*x.quantity) FILTER (WHERE x.type IN ('ADMIN_PLUS','ADMIN_MINUS') AND "+inRange+"),0) AS adjustments, "
            "COALESCE(SUM(txn_sign(x.type)*x.quantity) FILTER (WHERE x.type NOT IN "
            "  ('IN','USAGE','TRANSFER_IN','TRANSFER_OUT','ADMIN_PLUS','ADMIN_MINUS') AND "+inRange+"),0) AS other, "
            "COALESCE(SUM(txn_sign(x.type)*x.quantity) FILTER (WHERE x.txn_date <= p.t),0) AS closing "
            "FROM p, stock_overview o LEFT JOIN stock_txns x ON x.site_id=o.site_id AND x.material_id=o.material_id "
            "WHERE ($3::int = 0 OR o.site_id = $3::int) "
            "GROUP BY o.site_id, o.site, o.material_id, o.material, o.unit, p.f, p.t ORDER BY o.site_id, o.material";
        auto x=tx.exec(q.c_str(),pqxx::params{from,to,siteFilter});
        tx.commit();

        std::string fromDate=x.empty()?from:x[0]["from_date"].as<std::string>();
        std::string toDate  =x.empty()?to  :x[0]["to_date"].as<std::string>();
        if(!x.empty() && fromDate>toDate){ res.error(400,"from is after to"); return; }

        const char* cols[]={"opening","stock_in","usage","transfer_in","transfer_out","adjustments","other","closing"};

        if(format=="csv"){
            std::string out="\xEF\xBB\xBF";   // UTF-8 marker so Excel shows the text correctly
            out+="Site,Material,Unit,Opening,Stock In,Usage,Transfer In,Transfer Out,Adjustments,Other,Closing\r\n";
            for(auto i:x){
                out+=csvText(i["site"].as<std::string>())+","+csvText(i["material"].as<std::string>())+","+csvText(i["unit"].as<std::string>());
                for(const char* c:cols) out+=","+i[c].as<std::string>();
                out+="\r\n";
            }
            res.csv(out,"stock-report_"+fromDate+"_to_"+toDate+".csv");
            return;
        }

        nlohmann::json arr=nlohmann::json::array();
        for(auto i:x){
            nlohmann::json r={{"site_id",i["site_id"].as<int>()},{"site",i["site"].as<std::string>()},
                {"material_id",i["material_id"].as<int>()},{"material",i["material"].as<std::string>()},
                {"unit",i["unit"].as<std::string>()}};
            for(const char* c:cols) r[c]=i[c].as<double>();
            arr.push_back(r);
        }
        res.json(200,{{"from",fromDate},{"to",toDate},{"rows",arr}});
    }));
}