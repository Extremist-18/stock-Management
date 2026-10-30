#pragma once
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <cstdlib>

inline const std::string& corsOrigin(){
    static const std::string o=[]{ const char* v = std::getenv("CORS_ORIGIN"); return std::string(v?v:"");}();
    return o;
}

struct Response{
    int status=200;
    int retryAfter=0;
    std::string body,content_type="application/json";
    std::string disposition;

    void json(int code, const nlohmann::json&j){
        status=code;
        body = j.dump();
        content_type = "application/json";
    }
    void error(int code,const std::string &msg){
        json(code, nlohmann::json{{"error", msg}});
    }
    void text(int code,const std::string &s){
        status=code;
        body=s;
        content_type ="text/plain";
    }

    static const char* reason(int s) {
        switch (s) {
            case 200: return "OK";
            case 201: return "Created";
            case 204: return "No Content";
            case 400: return "Bad Request";
            case 401: return "Unauthorized";
            case 403: return "Forbidden";
            case 404: return "Not Found";
            case 405: return "Method Not Allowed";
            case 409: return "Conflict";
            case 413: return "Payload Too Large";
            case 429: return "Too Many Requests";
            case 431: return "Request Header Fields Too Large";
            case 503: return "Service Unavailable";
            default:  return s >= 500 ? "Internal Server Error" : "OK";
        }
    }
    void csv(const std::string &s,const std::string &filename){
        status=200; body=s; content_type="text/csv"; 
        disposition="attachment; filename=\""+filename+"\"";
    }

    std::string serialize() const {
        std::ostringstream o;
        o<<"HTTP/1.1 " << status << " " << reason(status) << "\r\n"
        <<"Content-Type: " << content_type << "; charset=utf-8\r\n"
        <<"Content-Length: " << body.size() << "\r\n"
        <<"Connection: close\r\n";
        if(retryAfter>0) o<<"Retry-After: "<<retryAfter<<"\r\n";
        if(!disposition.empty()) o<<"Content-Disposition: "<<disposition<<"\r\n";

        if(!corsOrigin().empty())
            o<<"Access-Control-Allow-Origin: "<<corsOrigin()<<"\r\n"
            <<"Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
            <<"Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n"
            <<"Vary: Origin\r\n";
        o<<"\r\n"<< body;
        return o.str();
    }
};

