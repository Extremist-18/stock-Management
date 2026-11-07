#pragma once
#include <pqxx/pqxx>
#include <cstdlib>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

#include "auth/jwt.hpp"
#include "db/Pool.hpp"
#include "http/App.hpp"

#include <regex>

inline bool isIsoDate(const std::string& s){
    static const std::regex re("^[0-9]{4}-[0-9]{2}-[0-9]{2}$");
    return std::regex_match(s,re);
}

template <class F>
inline nlohmann::json nullableText(const F &f){
    return f.is_null() ? nlohmann::json(nullptr) : nlohmann::json(f.template as<std::string>());
}

inline bool isUuid(const std::string& s){
    static const std::regex re("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$");
    return std::regex_match(s,re);
}

inline const std::string &jwtSecret(){
    static const std::string s=[]{
        const char*v = std::getenv("JWT_SECRET");
        if(!v || std::string(v).size()<32)  throw std::runtime_error("JWT_SECRET missing or shorter than 32 characters");
        return std::string(v);
    }();
    return s;
}

inline std::string duplicateMessage(const std::string& m){
    auto has=[&](const char* k){ return m.find(k)!=std::string::npos; };
    if(has("users_username_key"))   return "That username is already taken. Choose a different username.";
    if(has("materials_name_key"))   return "A material with this name already exists.";
    if(has("pallet_types_name_key"))    return "A pallet size with this name already exists.";
    if(has("packing_specs_name_key"))   return "A packing recipe with this name already exists.";
    if(has("sites_name_key"))   return "A site with this name already exists.";
    return "This already exists (duplicate entry).";
}

// First line of a Postgres error, without the "ERROR:  " prefix.
inline std::string dbMessage(const std::exception &e){
    std::string x =e.what();
    auto y= x.find('\n');
    if(y != std::string::npos)  x=x.substr(0,y);
    if(x.rfind("ERROR:",0)==0)  x = x.substr(6);
    size_t a =x.find_first_not_of(' ');
    return a==std::string::npos?x:x.substr(a);
}

// Turns thrown errors into clean JSON replies (so handlers can stay short).
inline Handler safe(Handler next) {
    return [next](Request& req, Response& res){
        try{    next(req, res);
        }catch(const nlohmann::json::exception&){
            res.error(400, "missing or wrong-typed field in JSON body");
        }catch(const pqxx::unique_violation& e){
            res.error(409, duplicateMessage(e.what()));
        }catch(const pqxx::sql_error& e){
            res.error(400, dbMessage(e));            // trigger messages arrive here
        }
    };
}

inline Handler requireAuth(Pool &pool, Handler next){
    return safe([&pool, next](Request &req, Response&res){
        std::string x=req.header("authorization");
        if(x.rfind("Bearer ",0)!=0){ res.error(401,"login required"); return;}
        int id =0;
        if(!jwt::verify(jwtSecret(),x.substr(7),id)){
            res.error(401,"invalid or expired token");
            return;
        }
        {
            auto conn= pool.acquire();
            pqxx::work tx(*conn);
            auto x=tx.exec("SELECT role, is_active FROM users WHERE id = $1",pqxx::params{id});
            tx.commit();
            if(x.empty() || !x[0]["is_active"].as<bool>()){
                res.error(401,"Account Disabled");
                return;
            }
            req.user_id=id;
            req.role = x[0]["role"].as<std::string>();
        }
        next(req,res);
    });
}

inline Handler requireAdmin(Pool& pool, Handler next){
    return requireAuth(pool,[next](Request& req,Response& res){
        if(req.role != "admin"){ res.error(403, "admin only"); return;}
        next(req, res);
    });
}

// // nullable text column -> JSON string, or null
// inline nlohmann::json sv(const pqxx::field &f){
//     return f.is_null() ? nlohmann::json(nullptr) : nlohmann::json(f.as<std::string>());
// }

// Tells the database triggers who is acting (lasts only for this transaction).
inline void setUser(pqxx::work& tx, int id) {
    tx.exec("SELECT set_config('app.user_id', $1, true)", pqxx::params{std::to_string(id)});
}

inline bool canAccessSite(pqxx::work &tx,const Request &req, int site_id){
    if(req.role=="admin")   return true;
    auto x = tx.exec("SELECT 1 FROM user_sites WHERE user_id = $1 AND site_id = $2",pqxx::params{req.user_id,site_id});
    return !x.empty();
}

inline void audit(pqxx::work& tx, const std::string& table, long long record_id,const std::string& action, int by, const nlohmann::json& data){
    tx.exec("INSERT INTO audit_log (table_name, record_id, action, new_data, changed_by) "
            "VALUES ($1, $2, $3, $4::jsonb, $5)",
            pqxx::params{table, record_id, action, data.dump(), by});
}

inline bool jsonBody(Request &req, Response &res, nlohmann::json&j){
    j=nlohmann::json::parse(req.body,nullptr,false);
    if(j.is_discarded() || !j.is_object()){res.error(400,"body is not a json object"); return false;}
    return true;
}

template <class T>
inline bool idParam(Request &req, Response &res, const char* name, T &out){
    try{
        const std::string &s=req.params[name];
        size_t pos=0;
        long long v = std::stoll(s,&pos);
        if(pos!=s.size() || v<=0 || v>2147483647LL) throw std::invalid_argument("x");
        out = (T)v;
        return true;
    }catch(...){
        res.error(400,std::string("invalid ") + name);
        return false;
    }
}
