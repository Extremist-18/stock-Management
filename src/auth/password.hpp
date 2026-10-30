#pragma once
#include <sodium.h>
#include <stdexcept>
#include <string>

namespace pw{
    inline void init(){
        if(sodium_init()<0) throw std::runtime_error("libsodium init failed");   
    }

    inline std::string hash(const std::string &password){
        char out[crypto_pwhash_STRBYTES];
        if(crypto_pwhash_str(out,password.c_str(),password.size(),crypto_pwhash_OPSLIMIT_INTERACTIVE,crypto_pwhash_MEMLIMIT_INTERACTIVE)!=0)
            throw std::runtime_error("password hashing failed(Out of Memory)");
        return std::string(out);
    }

    inline bool verify(const std::string& stored_hash, const std::string& password){
        return crypto_pwhash_str_verify(stored_hash.c_str(), password.c_str(), password.size()) == 0;
    }
}
