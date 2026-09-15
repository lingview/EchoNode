#include "Sha256.hpp"

#include <cstring>

namespace echonode::common {

namespace {

// SHA-256 轮常数
constexpr uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

} // namespace

Sha256::Sha256() {
    // 初始哈希值
    state_[0] = 0x6a09e667; state_[1] = 0xbb67ae85;
    state_[2] = 0x3c6ef372; state_[3] = 0xa54ff53a;
    state_[4] = 0x510e527f; state_[5] = 0x9b05688c;
    state_[6] = 0x1f83d9ab; state_[7] = 0x5be0cd19;
}

void Sha256::update(const void* data, size_t len) {
    if (finished_) return;
    const auto* bytes = static_cast<const uint8_t*>(data);
    totalBits_ += static_cast<uint64_t>(len) * 8;

    // 先填满残余缓冲，再整块处理
    if (bufferLen_ > 0) {
        const size_t need = 64 - bufferLen_;
        const size_t take = len < need ? len : need;
        std::memcpy(buffer_ + bufferLen_, bytes, take);
        bufferLen_ += take;
        bytes += take;
        len -= take;
        if (bufferLen_ == 64) {
            processBlock(buffer_);
            bufferLen_ = 0;
        }
    }
    while (len >= 64) {
        processBlock(bytes);
        bytes += 64;
        len -= 64;
    }
    if (len > 0) {
        std::memcpy(buffer_, bytes, len);
        bufferLen_ = len;
    }
}

std::string Sha256::finish() {
    if (!finished_) {
        // 填充 0x80 并补齐到 56 字节
        uint64_t bits = totalBits_;
        uint8_t pad = 0x80;
        update(&pad, 1);
        pad = 0;
        while (bufferLen_ != 56) {
            update(&pad, 1);
        }
        // 直接写长度，不经 update 以免重复计数
        uint8_t lenBytes[8];
        for (int i = 0; i < 8; ++i) lenBytes[i] = static_cast<uint8_t>(bits >> (56 - i * 8));
        std::memcpy(buffer_ + 56, lenBytes, 8);
        processBlock(buffer_);
        finished_ = true;
    }

    std::string out(64, '0');
    static const char* hex = "0123456789abcdef";
    for (int i = 0; i < 8; ++i) {
        for (int b = 0; b < 4; ++b) {
            out[i * 8 + b * 2] = hex[(state_[i] >> (28 - b * 8)) & 0xF];
            out[i * 8 + b * 2 + 1] = hex[(state_[i] >> (24 - b * 8)) & 0xF];
        }
    }
    return out;
}

void Sha256::processBlock(const uint8_t* block) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) |
               (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];

    for (int i = 0; i < 64; ++i) {
        const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = h + S1 + ch + K[i] + w[i];
        const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
    state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
}

std::string sha256Hex(const void* data, size_t len) {
    Sha256 sha;
    sha.update(data, len);
    return sha.finish();
}

} // namespace echonode::common
