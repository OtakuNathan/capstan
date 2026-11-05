#pragma once
#include <capstan/migration/transfer.hpp>
#include <glib.h>
#include <exception>
#ifdef CAPSTAN_OPENSSL_SHA256
#include <openssl/evp.h>
#endif
namespace capstan { namespace migration {
// Both providers implement exactly the existing SHA-256 byte commitment.
class sha256_stream final : public transfer_digest {
#ifdef CAPSTAN_OPENSSL_SHA256
    EVP_MD_CTX* hash_=EVP_MD_CTX_new();
#else
    GChecksum* hash_=g_checksum_new(G_CHECKSUM_SHA256);
#endif
    bool finalized_=false;
    std::array<std::uint8_t,32> result_{};
public:
    sha256_stream() noexcept {
        if(!hash_) std::terminate();
#ifdef CAPSTAN_OPENSSL_SHA256
        if(EVP_DigestInit_ex(hash_,EVP_sha256(),nullptr)!=1) std::terminate();
#endif
    }
    ~sha256_stream() override {
#ifdef CAPSTAN_OPENSSL_SHA256
        EVP_MD_CTX_free(hash_);
#else
        g_checksum_free(hash_);
#endif
    }
    sha256_stream(const sha256_stream&)=delete;
    sha256_stream& operator=(const sha256_stream&)=delete;
    void update(const std::uint8_t* data,std::size_t size) noexcept override {
        if(finalized_) std::terminate();
#ifdef CAPSTAN_OPENSSL_SHA256
        if(EVP_DigestUpdate(hash_,data,size)!=1) std::terminate();
#else
        if(size>static_cast<std::size_t>(G_MAXSSIZE)) std::terminate();
        g_checksum_update(hash_,data,static_cast<gssize>(size));
#endif
    }
    std::array<std::uint8_t,32> value() noexcept override {
        if(!finalized_) {
#ifdef CAPSTAN_OPENSSL_SHA256
            unsigned size=0;
            if(EVP_DigestFinal_ex(hash_,result_.data(),&size)!=1 || size!=result_.size()) std::terminate();
#else
            gsize size=result_.size();g_checksum_get_digest(hash_,result_.data(),&size);
#endif
            finalized_=true;
        }
        return result_;
    }
};
inline std::unique_ptr<transfer_digest> make_sha256_stream() {return std::unique_ptr<transfer_digest>(new sha256_stream);}
inline std::array<std::uint8_t,32> sha256(const buffer& data) noexcept {
    sha256_stream hash;hash.update(data.data(),data.size());return hash.value();
}
}}
