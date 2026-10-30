#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdlib>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "http/Request.hpp"
#include "http/Response.hpp"

using Handler = std::function<void(Request&, Response&)>;

class App{
    public:
    
    void get(const std::string& path,Handler h) { add("GET",path,std::move(h)); }
    void post(const std::string& path,Handler h) { add("POST",path,std::move(h)); }
    void put(const std::string& path,Handler h) { add("PUT",path,std::move(h)); }
    void del(const std::string& path,Handler h) { add("DELETE",path,std::move(h)); }

    void listen(int port){
        ::signal(SIGPIPE, SIG_IGN);

        int s = ::socket(AF_INET, SOCK_STREAM, 0);
        if(s < 0){ std::perror("socket"); return; }
        int yes = 1;
        ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        // BIND_ADDR=127.0.0.1 -> only the reverse proxy (Caddy) on this machine can reach the API
        if(const char* b=std::getenv("BIND_ADDR"); b && ::inet_pton(AF_INET,b,&addr.sin_addr)!=1){
            std::cerr<<"BIND_ADDR is not a valid IPv4 address\n"; ::close(s);return;
        }

        addr.sin_port = htons((uint16_t)port);
        if(::bind(s, (sockaddr*)&addr, sizeof addr) < 0){ std::perror("bind"); ::close(s); return; }
        if(::listen(s, 128) < 0){ std::perror("listen"); ::close(s); return; }
        std::cout<<"listening on port "<<port<<std::endl;

        for(;;){
            sockaddr_in peer{}; socklen_t plen=sizeof peer;
            int c=::accept(s,(sockaddr*)&peer,&plen);
            if(c<0){ if(errno != EINTR) std::perror("accept"); continue; }
            char ipbuf[INET_ADDRSTRLEN] = "";
            ::inet_ntop(AF_INET, &peer.sin_addr, ipbuf, sizeof ipbuf);
            std::string peer_ip=ipbuf;

            timeval tv{10, 0};
            ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

            if(active1 >= kMaxConnections){
                Response r; r.error(503, "server busy");
                send_all(c, r.serialize());
                ::close(c);
                continue;
            }
            ++active1;
            std::thread([this, c,peer_ip]{
                handle(c,peer_ip);
                ::close(c);
                --active1;
            }).detach();
        }
    }

    private:
    struct Route {
        std::string method;
        std::vector<std::string> parts;
        Handler handler;
    };
    std::vector<Route> routes_;
    std::atomic<int> active1{0};
    static constexpr int kMaxConnections = 256;
    static constexpr size_t kMaxHeader = 16 * 1024;
    static constexpr size_t kMaxBody = 1024 * 1024;

    void add(const std::string& method, const std::string& pattern, Handler h){
        routes_.push_back({method, split(pattern), std::move(h)});
    }

    static std::vector<std::string> split(const std::string& s){
        std::vector<std::string> out;
        std::string cur;
        for(char ch:s){
            if(ch == '/') { if(!cur.empty()) out.push_back(cur); cur.clear(); }
            else cur += ch;
        }
        if(!cur.empty()) out.push_back(cur);
        return out;
    }

    static bool match(const std::vector<std::string>& pat, const std::vector<std::string>& got, std::map<std::string, std::string>& params){
        if(pat.size() != got.size()) return false;
        for(int i = 0;i<(int)pat.size();i++){
            if(!pat[i].empty() && pat[i][0] == ':') params[pat[i].substr(1)] = got[i];
            else if(pat[i] != got[i]) return false;
        }
        return true;
    }

    static bool send_all(int fd, const std::string& data){
        size_t sent = 0;
        while(sent < data.size()){
            ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, 0);
            if(n<0) { if(errno == EINTR) continue; return false; }
            sent += (size_t)n;
        }
        return true;
    }

    static bool read_request(int fd, Request& req, int& err_status) {
        err_status = 0;
        std::string buf;
        char tmp[4096];

        size_t head_end;
        while((head_end = buf.find("\r\n\r\n")) == std::string::npos) {
            if (buf.size() > kMaxHeader) { err_status = 431; return false; }
            ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
            if (n <= 0) return false;
            buf.append(tmp, (size_t)n);
        }

        if(!parse_head(buf.substr(0, head_end), req)) { err_status = 400; return false; }
        std::string body = buf.substr(head_end + 4);

        size_t len = 0;
        auto it = req.headers.find("content-length");
        if(it != req.headers.end()){
            try{ len=std::stoul(it->second); } catch (...) { err_status = 400; return false; }
            if(len>kMaxBody) { err_status = 413; return false; }
        }
        while(body.size() < len){
            ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
            if(n <= 0) return false;
            body.append(tmp, (size_t)n);
        }
        body.resize(len);
        req.body = std::move(body);
        return true;
    }

    void dispatch(Request& req, Response& res){
        auto parts = split(req.path);
        bool path_matched = false;
        for(auto& r : routes_){
            std::map<std::string, std::string> params;
            if(!match(r.parts, parts, params)) continue;
            path_matched = true;
            if(r.method != req.method) continue;
            req.params = std::move(params);
            r.handler(req, res);
            return;
        }
        if(path_matched) res.error(405, "method not allowed");
        else              res.error(404, "not found");
    }

    void handle(int fd,const std::string &peer_ip){
        Request req;
        int err=0;
        if(!read_request(fd, req, err)){
            if(err){ Response r; r.error(err, "bad request"); send_all(fd, r.serialize()); }
            return;
        }

        // Behind our own proxy (Caddy on the same machine) the real client IP is in X-Forwarded-For.
        // Anyone else connecting directly is NOT trusted to set it.
        req.ip=peer_ip;
        if(peer_ip=="127.0.0.1"){
            std::string xff=req.header("x-forwarded-for");
            if(!xff.empty()) req.ip=trim(xff.substr(0,xff.find(',')));
        }

        Response res;
        if(req.method=="OPTIONS"){
            res.status=204;
        }else{
            try{
                dispatch(req,res);
            }catch(const std::exception& e){
                std::cerr<<"handler error: "<<e.what()<<std::endl;
                res.error(500, "internal error");
            }
        }
        std::cout<<req.method<<" "<<req.path<<" -> "<<res.status<<std::endl;
        send_all(fd, res.serialize());
    }
};
