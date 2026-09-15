// SHA-256 摘要计算
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace echonode::common {

// 流式计算：分块 update，finish 取十六进制摘要
class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t len);
    // 完成并返回 64 字符小写十六进制，调用后对象不可再用
    std::string finish();

private:
    void processBlock(const uint8_t* block);

    uint32_t state_[8];
    uint64_t totalBits_ = 0;
    uint8_t buffer_[64];
    size_t bufferLen_ = 0;
    bool finished_ = false;
};

// 一次性计算
std::string sha256Hex(const void* data, size_t len);

} // namespace echonode::common
