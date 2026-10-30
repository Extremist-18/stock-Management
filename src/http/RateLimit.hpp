#pragma once
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

#include "http/Response.hpp"
using Clock=std::chrono::steady_clock;
class RateLimiter{
    int max1;
    std::mutex mtx;
    std::chrono::seconds window1;
    std::unordered_map<std::string, std::deque<Clock::time_point>> hits1;

    void prune(std::deque<Clock::time_point> &q,Clock::time_point now){
        while(!q.empty() && (now-q.front())>=window1)   q.pop_front();
    }
    void sweep(Clock::time_point now){
        if(hits1.size()<3000)   return;
        for(auto it=hits1.begin();it!=hits1.end();){
            prune(it->second,now);
            it=(it->second.empty()?hits1.erase(it):std::next(it));
        }
    }
    public:
    RateLimiter(int mx_hits,std::chrono::seconds window):max1(mx_hits),window1(window){}
    // Counts this attempt. Returns false (and does NOT count it) when over the limit.
    bool allow(const std::string &key){
        std::lock_guard<std::mutex> lock(mtx);
        auto now=Clock::now();
        auto &q=hits1[key];
        prune(q,now);
        if((int)q.size()>=max1) return false;
        q.push_back(now);
        sweep(now);
        return true;
    }
    // True when the key is already at the limit. Does not count anything.
    bool blocked(const std::string &key){
        std::lock_guard<std::mutex> lock(mtx);
        auto it=hits1.find(key);
        if(it==hits1.end()) return false;
        prune(it->second,Clock::now());
        return (int)(it->second.size()>=max1);
    }
    // Always count
    void hit(const std::string &key){
        std::lock_guard<std::mutex> lock(mtx);
        auto now=Clock::now();
        auto &q=hits1[key];
        prune(q,now);
        q.push_back(now);
        sweep(now);
    }
    // Forget the key (use after a SUCCESSFUL login).
    void clear(const std::string &key){
        std::lock_guard<std::mutex> lock(mtx);
        hits1.erase(key);
    }
    int retryAfter(const std::string &key){
        std::lock_guard<std::mutex> lock(mtx);
        auto it=hits1.find(key);
        if(it==hits1.end()) return 0;
        auto now=Clock::now();
        prune(it->second,now);
        if((int)(it->second.size()<max1))   return 0;
        auto left=std::chrono::duration_cast<std::chrono::milliseconds>(it->second.front()+window1-now).count();
        return (int)((left+999)/1000)>0?((left+999)/1000):1;
    }
};

inline RateLimiter& loginIpLimit()   { static RateLimiter r(30, std::chrono::minutes(10)); return r; } // any login try, per IP
inline RateLimiter& loginFailLimit() { static RateLimiter r(5,  std::chrono::minutes(15)); return r; } // FAILED tries, per IP+username
inline RateLimiter& signupLimit()    { static RateLimiter r(5,  std::chrono::hours(1));    return r; } // signup requests, per IP

class HashSlot{
    static std::atomic<int>&busy() {static std::atomic<int>n{0}; return n;}
    public:
    static constexpr int mx=4;
    bool ok;
    HashSlot() {ok=busy().fetch_add(1)<mx; if(!ok)  busy().fetch_sub(1);}
    ~HashSlot() {if(ok) busy().fetch_sub(1);}
    HashSlot(const HashSlot&)=delete;
    HashSlot& operator=(const HashSlot&)=delete; 
};

inline void tooMany(Response &res,int time1){
    res.error(429,"Too Many Attempts! Try Again after sometime " + std::to_string(time1) + " seconds.");
    res.retryAfter = time1;
}
