#pragma once
#include <mutex>
#include <chrono>
#include <pqxx/pqxx>
#include <condition_variable>
#include <memory>
#include <string>
#include <iostream>
#include <queue>
#include <stdexcept>

class Pool{
    public:
    class Lease{
        public:
        Lease(Pool *p,std::unique_ptr<pqxx::connection> c){
            pool1 = p;
            conn1 = std::move(c);
        }
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease && o) noexcept:pool1(o.pool1),conn1(std::move(o.conn1)){
            o.pool1 = nullptr;
        }
        ~Lease(){
            if(conn1 && pool1)
            pool1->release(std::move(conn1));
        }
        pqxx::connection & operator*(){
            return *conn1;
        }
        private:
        Pool *pool1;
        std::unique_ptr<pqxx::connection> conn1;
    };
    
    Pool(std::string url, int mx){
        url1 = std::move(url);
        mx_limit = mx;
    } 
    // std::unique_ptr<pqxx::connection> acquire(){
    //     std::lock_guard<std::mutex> lock(mtx);
    //     if(idle1.empty()){ // make ir wait or say try again later
    //         return std::make_unique<pqxx::connection>(url1); 
    //     }
    //     auto c=std::move(idle1.front());
    //     idle1.pop();
    //     return c;
    // }

    Lease acquire(){
        std::unique_ptr<pqxx::connection> c;
        bool need = false;
        {
            std::unique_lock<std::mutex> lock(mtx);
            if(!cv1.wait_for(lock,std::chrono::seconds(5),[&]{
                return !idle1.empty() || created<mx_limit; }))
                    throw std::runtime_error("Pool timeout: all connections busy");
            if(!idle1.empty()) { c = std::move(idle1.front()); idle1.pop(); }
            else    { ++created; need= true; }
        }
        if(!need){
            try{
                pqxx::work t(*c);   t.exec("SELECT 1"); t.commit();
            }catch(const std::exception &e){
                c.reset();
                need = true;
            }
        }
        if(need){
            try{
                c = std::make_unique<pqxx::connection>(url1);
            }catch(...){
                { std::lock_guard<std::mutex> lock(mtx); created--; }
                cv1.notify_one();
                throw;
            }
        }
        return Lease(this,std::move(c));
    }

    private:
    void release(std::unique_ptr<pqxx::connection> c){
        // free all resources alloted to c
        {
            std::lock_guard<std::mutex> lock(mtx); 
            idle1.push(std::move(c));
        }
        cv1.notify_one();
    }
    std::string url1;
    int mx_limit,created=0;
    std::queue<std::unique_ptr<pqxx::connection>> idle1;
    std::mutex mtx;
    std::condition_variable cv1;
};
