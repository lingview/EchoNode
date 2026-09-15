// v4 UUID 生成与二进制互转
#pragma once
#include <string>

namespace echonode::common {

// 生成 v4 UUID 字符串
std::string generateUuid();

// UUID 字符串压成 16 字节二进制
std::string uuidToBytes(const std::string& uuid);

// 16 字节二进制还原为 UUID 字符串，长度非 16 返回空串
std::string uuidFromBytes(const std::string& bytes);

}
