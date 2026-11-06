#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include <mbedtls/gcm.h>

namespace okswitch::net {

// VMess AEAD over a byte stream. Each instance owns exactly one TCP connection.
class VmessAead {
public:
    explicit VmessAead(const std::string& uuid);
    ~VmessAead();
    bool valid() const { return valid_; }
    bool begin(const unsigned char* destination, size_t destinationLength,
               std::vector<unsigned char>& output);
    bool encode(const unsigned char* data, size_t length,
                std::vector<unsigned char>& output);
    bool decode(const unsigned char* data, size_t length,
                std::vector<unsigned char>& output);
private:
    bool valid_=false, started_=false;
    enum class ReadStage { HeaderLength, HeaderPayload, ChunkLength, ChunkPayload } stage_=ReadStage::HeaderLength;
    std::array<unsigned char,16> uuid_{}, requestKey_{},requestIv_{},responseKey_{},responseIv_{};
    unsigned char responseAuth_=0;
    std::vector<unsigned char> pending_;
    size_t expected_=0;
    unsigned sendCount_=0,receiveCount_=0;
    mbedtls_gcm_context sender_{},receiver_{};
};
}
