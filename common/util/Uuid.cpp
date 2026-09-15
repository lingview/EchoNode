#include "Uuid.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <random>

namespace echonode::common {

namespace {

// 混入地址与时间的随机种子
uint64_t seedMix() {
    uint64_t x = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    x ^= reinterpret_cast<uint64_t>(&x);
    std::atomic<uint64_t> counter;
    x ^= counter.fetch_add(1) * 0x9E3779B97F4A7C15ULL;
    return x;
}

uint64_t hexToU64(const std::string& hex) {
    return std::stoull(hex, nullptr, 16);
}

std::string toHex(uint64_t v, int width) {
    static const char* digits = "0123456789abcdef";
    std::string out(width, '0');
    for (int i = width - 1; i >= 0; --i) {
        out[i] = digits[v & 0xF];
        v >>= 4;
    }
    return out;
}

} // namespace

// 生成 v4 UUID 字符串
std::string generateUuid() {
    static thread_local std::mt19937_64 rng(seedMix());

    uint64_t hi = rng();
    uint64_t lo = rng();

    // 按 RFC 4122 设置版本号与变体位
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    std::string out;
    out.reserve(36);
    out += toHex(hi >> 32, 8);
    out += '-';
    out += toHex((hi >> 16) & 0xFFFF, 4);
    out += '-';
    out += toHex(hi & 0xFFFF, 4);
    out += '-';
    out += toHex(lo >> 48, 4);
    out += '-';
    out += toHex(lo & 0xFFFFFFFFFFFFULL, 12);
    return out;
}

// UUID 字符串压成 16 字节二进制
std::string uuidToBytes(const std::string& uuid) {
    std::string compact;
    compact.reserve(32);
    for (char c : uuid) {
        if (c != '-') compact += c;
    }
    std::string bytes;
    bytes.reserve(16);
    for (size_t i = 0; i + 1 < compact.size(); i += 2) {
        bytes += static_cast<char>(hexToU64(compact.substr(i, 2)) & 0xFF);
    }
    return bytes;
}

// 16 字节二进制还原为 UUID 字符串
std::string uuidFromBytes(const std::string& bytes) {
    if (bytes.size() != 16) return {};
    std::string hex;
    hex.reserve(32);
    for (unsigned char b : bytes) {
        hex += toHex(b, 2);
    }
    return hex.substr(0, 8) + '-' + hex.substr(8, 4) + '-' + hex.substr(12, 4) +
           '-' + hex.substr(16, 4) + '-' + hex.substr(20, 12);
}

} // namespace echonode::common
