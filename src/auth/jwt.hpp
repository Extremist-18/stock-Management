#pragma once
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <array>
#include <cstdint>
#include <ctime>
#include <nlohmann/json.hpp>
#include <string>

namespace jwt{
    inline std::string encodeURL(const std::string &in){
        static const char *t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        std::string out;
        uint32_t val=0;
        int bits =-6;
        for(unsigned char c:in){
            val = (((val<<8)+c) & 0xFFFFFF);
            bits +=8;
            while(bits>=0){
                out.push_back(t[(val>>bits)&0x3F]);
                bits -=6;
            }
        }
        if(bits>-6) out.push_back(t[((val<<8)>>(bits+8))& 0x3F]);
        return out;
    }

    inline bool decodeURL(const std::string &in, std::string &out){
        static const std::array<int,256> T=[]{
            std::array<int,256> a;
            a.fill(-1);
            const char *t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
            for(int i=0;i<64;i++)   a[(unsigned char)t[i]]=i;
            return a;
        }();
        out.clear();    
        uint32_t val=0;
        int bits = -8;
        for(unsigned char c:in){
            if(T[c]==-1)    return false;
            val =((val<<6)|(uint32_t)T[c]) & 0xFFFFFF;
            bits +=6;
            if(bits>=0){
                out.push_back((char)((val>>bits)&0xFF));
                bits -= 8;
            }
        }
        return true;
    }

    inline std::string sha256(const std::string &key, const std::string &data){
        unsigned char out[EVP_MAX_MD_SIZE];
        unsigned int len = 0;
        HMAC(EVP_sha256(), key.data(), (int)key.size(), (const unsigned char*)data.data(), data.size(), out, &len);
        return std::string((const char*)out, len);
    }

    // token = base64url(header).base64url(payload).base64url(signature)
    inline std::string create(const std::string& secret, int user_id, long ttl_seconds){
        std::string h = jwt::encodeURL(R"({"alg":"HS256","typ":"JWT"})");
        nlohmann::json payload = {{"sub", user_id}, {"exp", (long long)std::time(nullptr) + ttl_seconds}};
        std::string p = jwt::encodeURL(payload.dump());
        std::string signature = jwt::encodeURL(sha256(secret, h + "." + p));
        return (h + "." + p + "." + signature);
    }

    inline bool verify(const std::string &secret, const std::string &token, int &userId){
        size_t d1 = token.find('.');
        if(d1==std::string::npos) return false;
        size_t d2 = token.find('.',d1+1);
        if(d2==std::string::npos) return false;

        std::string h_b64 = token.substr(0,d1);
        std::string p_b64 = token.substr(d1+1,d2-d1-1);
        std::string s_b64 = token.substr(d2+1);

        std::string header_text;
        if (!decodeURL(h_b64, header_text)) return false;
        auto header = nlohmann::json::parse(header_text, nullptr, false);
        if (header.is_discarded() || !header.is_object() || header.value("alg", "") != "HS256") return false; 

        std::string got_sig;
        if(!decodeURL(s_b64, got_sig)) return false;
        std::string want_sig = sha256(secret, h_b64 + "." + p_b64);
        if(got_sig.size() != want_sig.size() || CRYPTO_memcmp(got_sig.data(), want_sig.data(), want_sig.size()) != 0)   return false; 

        std::string payload_text;
        if(!decodeURL(p_b64, payload_text)) return false;
        auto payload = nlohmann::json::parse(payload_text, nullptr, false);
        if(payload.is_discarded() || !payload.is_object()) return false;
        if(!payload.contains("sub") || !payload["sub"].is_number_integer()) return false;
        if(!payload.contains("exp") || !payload["exp"].is_number_integer()) return false;
        if(payload["exp"].get<long long>() < (long long)std::time(nullptr)) return false;

        userId = payload["sub"].get<int>();
        return true;
    }

}
