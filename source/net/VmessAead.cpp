#include "okswitch/net/VmessAead.hpp"
#include <switch.h>
#include <mbedtls/aes.h>
#include <mbedtls/md.h>
#include <mbedtls/md5.h>
#include <mbedtls/sha256.h>
#include <algorithm>
#include <ctime>
#include <cstring>

namespace okswitch::net {
namespace {
using Bytes=std::vector<unsigned char>;
using Paths=std::vector<std::string>;

bool digest(const unsigned char* data,size_t length,unsigned char output[32]){
    return mbedtls_sha256_ret(data,length,output,0)==0;
}
bool nestedHash(const Paths& paths,size_t level,const Bytes& input,unsigned char output[32]){
    if(level==0){
        static constexpr unsigned char root[]="VMess AEAD KDF";
        const auto* sha=mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
        return sha&&mbedtls_md_hmac(sha,root,sizeof(root)-1,input.data(),input.size(),output)==0;
    }
    const auto& path=paths[level-1];
    Bytes key(path.begin(),path.end());
    if(key.size()>64){
        unsigned char reduced[32];if(!nestedHash(paths,level-1,key,reduced))return false;
        key.assign(reduced,reduced+32);
    }
    Bytes inner(64,0x36),outer(64,0x5c);
    for(size_t i=0;i<key.size();++i){inner[i]^=key[i];outer[i]^=key[i];}
    inner.insert(inner.end(),input.begin(),input.end());
    unsigned char middle[32];if(!nestedHash(paths,level-1,inner,middle))return false;
    outer.insert(outer.end(),middle,middle+32);
    return nestedHash(paths,level-1,outer,output);
}
bool kdf(const unsigned char* key,size_t length,const Paths& paths,unsigned char output[32]){
    return nestedHash(paths,paths.size(),Bytes(key,key+length),output);
}
bool gcmSeal(const unsigned char* key,const unsigned char* nonce,const unsigned char* aad,
             size_t aadLength,const unsigned char* plain,size_t length,Bytes& out){
    mbedtls_gcm_context ctx;mbedtls_gcm_init(&ctx);
    const size_t begin=out.size();out.resize(begin+length+16);
    const bool ok=mbedtls_gcm_setkey(&ctx,MBEDTLS_CIPHER_ID_AES,key,128)==0
        &&mbedtls_gcm_crypt_and_tag(&ctx,MBEDTLS_GCM_ENCRYPT,length,nonce,12,
            aad,aadLength,plain,out.data()+begin,16,out.data()+begin+length)==0;
    mbedtls_gcm_free(&ctx);return ok;
}
bool gcmOpen(const unsigned char* key,const unsigned char* nonce,const unsigned char* aad,
             size_t aadLength,const unsigned char* cipher,size_t length,Bytes& out){
    mbedtls_gcm_context ctx;mbedtls_gcm_init(&ctx);
    const size_t begin=out.size();out.resize(begin+length);
    const bool ok=mbedtls_gcm_setkey(&ctx,MBEDTLS_CIPHER_ID_AES,key,128)==0
        &&mbedtls_gcm_auth_decrypt(&ctx,length,nonce,12,aad,aadLength,
            cipher+length,16,cipher,out.data()+begin)==0;
    mbedtls_gcm_free(&ctx);return ok;
}
uint32_t crc32(const unsigned char* data,size_t length){
    uint32_t crc=0xffffffffu;
    for(size_t i=0;i<length;++i){
        crc^=data[i];for(int j=0;j<8;++j)crc=(crc>>1)^(0xedb88320u&-(crc&1u));
    }
    return ~crc;
}
void append32(Bytes& bytes,uint32_t value){
    bytes.push_back((unsigned char)(value>>24));bytes.push_back((unsigned char)(value>>16));
    bytes.push_back((unsigned char)(value>>8));bytes.push_back((unsigned char)value);
}
std::array<unsigned char,12> chunkNonce(const std::array<unsigned char,16>& iv,unsigned count){
    std::array<unsigned char,12> nonce{};
    std::copy(iv.begin(),iv.begin()+12,nonce.begin());
    nonce[0]=(unsigned char)(count>>8);nonce[1]=(unsigned char)count;
    return nonce;
}
}

VmessAead::VmessAead(const std::string& uuid){
    mbedtls_gcm_init(&sender_);mbedtls_gcm_init(&receiver_);
    if(uuid.size()!=36||uuid[8]!='-'||uuid[13]!='-'||uuid[18]!='-'||uuid[23]!='-')return;
    size_t n=0;int high=-1;
    for(char c:uuid){
        if(c=='-')continue;
        const int digit=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10
            :c>='A'&&c<='F'?c-'A'+10:-1;
        if(digit<0)return;
        if(high<0)high=digit;
        else{uuid_[n++]=(unsigned char)((high<<4)|digit);high=-1;}
    }
    valid_=n==16;
}
VmessAead::~VmessAead(){mbedtls_gcm_free(&sender_);mbedtls_gcm_free(&receiver_);}

bool VmessAead::begin(const unsigned char* destination,size_t destinationLength,Bytes& output){
    output.clear();if(!valid_||started_||destinationLength<5||destinationLength>260)return false;
    randomGet(requestIv_.data(),16);randomGet(requestKey_.data(),16);
    randomGet(&responseAuth_,1);
    unsigned char hash[32];
    if(!digest(requestKey_.data(),16,hash))return false;
    std::copy(hash,hash+16,responseKey_.begin());
    if(!digest(requestIv_.data(),16,hash))return false;
    std::copy(hash,hash+16,responseIv_.begin());
    if(mbedtls_gcm_setkey(&sender_,MBEDTLS_CIPHER_ID_AES,requestKey_.data(),128)!=0
        ||mbedtls_gcm_setkey(&receiver_,MBEDTLS_CIPHER_ID_AES,responseKey_.data(),128)!=0)return false;

    Bytes instructions;instructions.reserve(64+destinationLength);
    instructions.push_back(1);
    instructions.insert(instructions.end(),requestIv_.begin(),requestIv_.end());
    instructions.insert(instructions.end(),requestKey_.begin(),requestKey_.end());
    instructions.push_back(responseAuth_);
    instructions.push_back(1); // Standard chunk stream without length masking.
    instructions.push_back(3); // AES-128-GCM body, no random padding.
    instructions.push_back(0);instructions.push_back(1); // Reserved, TCP.
    instructions.insert(instructions.end(),destination,destination+destinationLength);
    uint32_t fnv=2166136261u;
    for(unsigned char byte:instructions)fnv=(fnv^byte)*16777619u;
    append32(instructions,fnv);

    static constexpr unsigned char suffix[]="c48619fe-8f02-49e0-b9e9-edf763e17e21";
    Bytes cmd(uuid_.begin(),uuid_.end());cmd.insert(cmd.end(),suffix,suffix+sizeof(suffix)-1);
    unsigned char cmdKey[16];
    if(mbedtls_md5_ret(cmd.data(),cmd.size(),cmdKey)!=0)return false;
    unsigned char authPlain[16]{};
    const auto now=(uint64_t)std::time(nullptr);
    for(int i=0;i<8;++i)authPlain[i]=(unsigned char)(now>>(56-8*i));
    randomGet(authPlain+8,4);
    const uint32_t check=crc32(authPlain,12);
    authPlain[12]=(unsigned char)(check>>24);authPlain[13]=(unsigned char)(check>>16);
    authPlain[14]=(unsigned char)(check>>8);authPlain[15]=(unsigned char)check;
    if(!kdf(cmdKey,16,{"AES Auth ID Encryption"},hash))return false;
    mbedtls_aes_context aes;mbedtls_aes_init(&aes);
    unsigned char authId[16];
    const bool authOk=mbedtls_aes_setkey_enc(&aes,hash,128)==0
        &&mbedtls_aes_crypt_ecb(&aes,MBEDTLS_AES_ENCRYPT,authPlain,authId)==0;
    mbedtls_aes_free(&aes);if(!authOk)return false;
    output.insert(output.end(),authId,authId+16);
    const unsigned char lengthPlain[2]={(unsigned char)(instructions.size()>>8),
        (unsigned char)instructions.size()};
    unsigned char nonceSeed[8];randomGet(nonceSeed,8);
    const std::string authPart((char*)authId,16),noncePart((char*)nonceSeed,8);
    if(!kdf(cmdKey,16,{"VMess Header AEAD Key_Length",authPart,noncePart},hash))return false;
    unsigned char lengthKey[16];std::memcpy(lengthKey,hash,16);
    if(!kdf(cmdKey,16,{"VMess Header AEAD Nonce_Length",authPart,noncePart},hash))return false;
    if(!gcmSeal(lengthKey,hash,authId,16,lengthPlain,2,output))return false;
    output.insert(output.end(),nonceSeed,nonceSeed+8);
    if(!kdf(cmdKey,16,{"VMess Header AEAD Key",authPart,noncePart},hash))return false;
    unsigned char headerKey[16];std::memcpy(headerKey,hash,16);
    if(!kdf(cmdKey,16,{"VMess Header AEAD Nonce",authPart,noncePart},hash))return false;
    if(!gcmSeal(headerKey,hash,authId,16,instructions.data(),instructions.size(),output))return false;
    started_=true;return true;
}

bool VmessAead::encode(const unsigned char* data,size_t length,Bytes& output){
    output.clear();if(!started_)return false;
    for(size_t offset=0;offset<length;){
        if(sendCount_>=65536)return false;
        const size_t count=std::min<size_t>(0x3fff-16,length-offset);
        const size_t start=output.size();output.resize(start+2+count+16);
        const unsigned packetLength=(unsigned)(count+16);
        output[start]=(unsigned char)(packetLength>>8);
        output[start+1]=(unsigned char)packetLength;
        const auto nonce=chunkNonce(requestIv_,sendCount_++);
        if(mbedtls_gcm_crypt_and_tag(&sender_,MBEDTLS_GCM_ENCRYPT,count,
            nonce.data(),nonce.size(),nullptr,0,data+offset,output.data()+start+2,
            16,output.data()+start+2+count)!=0)return false;
        offset+=count;
    }
    return true;
}

bool VmessAead::decode(const unsigned char* data,size_t length,Bytes& output){
    output.clear();if(!started_||pending_.size()+length>128*1024)return false;
    pending_.insert(pending_.end(),data,data+length);
    for(;;){
        if(stage_==ReadStage::HeaderLength){
            if(pending_.size()<18)break;
            unsigned char hash[32];
            if(!kdf(responseKey_.data(),16,{"AEAD Resp Header Len Key"},hash))return false;
            unsigned char key[16];std::memcpy(key,hash,16);
            if(!kdf(responseIv_.data(),16,{"AEAD Resp Header Len IV"},hash))return false;
            Bytes decrypted;
            if(!gcmOpen(key,hash,nullptr,0,pending_.data(),2,decrypted))return false;
            expected_=((size_t)decrypted[0]<<8)|decrypted[1];
            if(expected_<4||expected_>4096)return false;
            pending_.erase(pending_.begin(),pending_.begin()+18);
            stage_=ReadStage::HeaderPayload;
        }
        if(stage_==ReadStage::HeaderPayload){
            if(pending_.size()<expected_+16)break;
            unsigned char hash[32];
            if(!kdf(responseKey_.data(),16,{"AEAD Resp Header Key"},hash))return false;
            unsigned char key[16];std::memcpy(key,hash,16);
            if(!kdf(responseIv_.data(),16,{"AEAD Resp Header IV"},hash))return false;
            Bytes header;
            if(!gcmOpen(key,hash,nullptr,0,pending_.data(),expected_,header)
                ||header.size()<4||header[0]!=responseAuth_
                ||header.size()<4+(size_t)header[3])return false;
            pending_.erase(pending_.begin(),pending_.begin()+expected_+16);
            expected_=0;stage_=ReadStage::ChunkLength;
        }
        if(stage_==ReadStage::ChunkLength){
            if(pending_.size()<2)break;
            expected_=((size_t)pending_[0]<<8)|pending_[1];
            if(expected_<16||expected_>0x3fff)return false;
            pending_.erase(pending_.begin(),pending_.begin()+2);
            stage_=ReadStage::ChunkPayload;
        }
        if(stage_==ReadStage::ChunkPayload){
            if(pending_.size()<expected_)break;
            if(receiveCount_>=65536)return false;
            const size_t plainLength=expected_-16,start=output.size();
            output.resize(start+plainLength);
            const auto nonce=chunkNonce(responseIv_,receiveCount_++);
            if(mbedtls_gcm_auth_decrypt(&receiver_,plainLength,nonce.data(),nonce.size(),
                nullptr,0,pending_.data()+plainLength,16,pending_.data(),output.data()+start)!=0)return false;
            pending_.erase(pending_.begin(),pending_.begin()+expected_);
            expected_=0;stage_=ReadStage::ChunkLength;
        }
    }
    return true;
}
}
