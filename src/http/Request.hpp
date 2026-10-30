#pragma once
#include <algorithm>
#include <cctype>
#include <map>
#include <sstream>
#include <string>

struct Request{
    std::string method,path, body;
    std::map<std::string,std::string> headers,query, params;

    int user_id=0;
    std::string role;
    std::string ip;
    std::string header(const std::string &key) const{
        auto it =headers.find(key);
        return it==headers.end()?"":it->second;
    }
};

inline std::string trim(const std::string& s){
    auto a = s.find_first_not_of(" \t\r\n");
    if(a == std::string::npos) return "";
    auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a,b-a+1);
}

inline std::string urlDecode(const std::string &s){
    std::string out;
    int n = s.size();
    for(int i=0;i<n;i++){
        if(s[i]=='%' && i+2<n && isxdigit((unsigned char)s[i+1]) && isxdigit((unsigned char)s[i+2])){
            out += (char)stoi(s.substr(i+1,2),nullptr,16);
            i+=2;
        }else if(s[i]=='+') out+=' ';
        else out +=s[i];
    }
    return out;
}

inline void parse_query(const std::string &q, std::map<std::string,std::string> &mp){
    std::stringstream ss(q);
    std::string res;
    while(std::getline(ss,res,'&')){
        if(res.empty()) continue;
        auto x = res.find('=');
        if(x==std::string::npos) mp[urlDecode(res)]="";
        else mp[urlDecode(res.substr(0,x))] = urlDecode(res.substr(x+1));
    }
}

inline bool parse_head(const std::string &head, Request &req){
    auto x=head.find("\r\n");
    std::string first = head.substr(0,x);
    auto a =first.find(' ');
    if(a==std::string::npos) return false;
    auto b=first.find(' ',a+1);
    if(b==std::string::npos) return false;

    req.method = first.substr(0,a);
    std::string target = first.substr(a+1,b-a-1);
    std::string version =  first.substr(b+1);

    if(req.method.empty() || target.empty() || version.rfind("HTTP/",0)!=0) return false;
    auto q=target.find('?');
    req.path = urlDecode(target.substr(0,q));
    if(q!=std::string::npos) parse_query(target.substr(q+1),req.query);

    auto pos = (x==std::string::npos)?head.size():x+2;
    while(pos<head.size()){
        auto y = head.find("\r\n",pos);
        if(y==std::string::npos) y=head.size();
        std::string line = head.substr(pos,y-pos);
        auto colon = line.find(':');
        if(colon!=std::string::npos){
            std::string k = trim(line.substr(0,colon));
            std::transform(k.begin(),k.end(),k.begin(),[](unsigned char c){
                return (char)std::tolower(c);
            });
            req.headers[k] =trim(line.substr(colon+1));
        }
        pos =y+2;
    }
    return true;
}