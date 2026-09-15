#include "FileExecutor.hpp"

#include "platform/Error.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
#include <vector>

#include "util/Uuid.hpp"

#ifdef _WIN32
#include <windows.h>
// Windows 下 UTF-8 路径需经宽字符打开，窄字符 fstream 会按 ANSI 代码页解释致乱码
static std::wstring u8ToWide(const std::string& s) {
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    return w;
}
static std::ifstream openIn(const std::string& p) {
    return std::ifstream(std::filesystem::path(u8ToWide(p)), std::ios::binary);
}
static std::fstream openOutTrunc(const std::string& p) {
    return std::fstream(std::filesystem::path(u8ToWide(p)),
                        std::ios::binary | std::ios::out | std::ios::trunc);
}
// 续传：打开已存在文件用于随机写（r+ 语义）
static std::fstream openOutAppend(const std::string& p) {
    return std::fstream(std::filesystem::path(u8ToWide(p)),
                        std::ios::binary | std::ios::in | std::ios::out);
}
#else
static std::ifstream openIn(const std::string& p) {
    return std::ifstream(p, std::ios::binary);
}
static std::fstream openOutTrunc(const std::string& p) {
    return std::fstream(p, std::ios::binary | std::ios::out | std::ios::trunc);
}
static std::fstream openOutAppend(const std::string& p) {
    return std::fstream(p, std::ios::binary | std::ios::in | std::ios::out);
}
#endif

namespace echonode::executor {

using common::Sha256;

namespace {

// 组装 0x01 文件块帧：taskId(16) + seq(4,大端) + type(1) + offset(8,大端) + 数据
std::string buildFileFrame(const std::string& taskIdBytes, uint32_t seq, size_t offset,
                           const char* data, size_t len) {
    std::string frame = taskIdBytes;
    frame += static_cast<char>((seq >> 24) & 0xFF);
    frame += static_cast<char>((seq >> 16) & 0xFF);
    frame += static_cast<char>((seq >> 8) & 0xFF);
    frame += static_cast<char>(seq & 0xFF);
    frame += static_cast<char>(0x01);
    for (int i = 7; i >= 0; --i)
        frame += static_cast<char>((offset >> (i * 8)) & 0xFF);
    frame.append(data, len);
    return frame;
}

// 从 29B 帧头解析 offset（[21..28] 大端）
size_t parseOffset(const uint8_t* bytes) {
    size_t off = 0;
    for (int i = 0; i < 8; ++i) off = (off << 8) | bytes[21 + i];
    return off;
}

// 读取文件 [0, n) 喂给 sha（续传时补齐前缀，使最终摘要覆盖全文件）；返回实际读到的字节数
size_t seedShaPrefix(std::ifstream& in, size_t n, Sha256& sha) {
    if (n == 0) return 0;
    in.clear();
    in.seekg(0, std::ios::beg);
    std::vector<char> buf(65536);
    size_t done = 0;
    while (done < n) {
        const size_t want = std::min(buf.size(), n - done);
        in.read(buf.data(), static_cast<std::streamsize>(want));
        const size_t got = static_cast<size_t>(in.gcount());
        if (got == 0) break;
        sha.update(buf.data(), got);
        done += got;
    }
    in.clear();
    return done;
}

} // namespace

protocol::TaskResult FileExecutor::execute(protocol::Task task) {
    try {
        if (task.action == "file_download") {
            const std::string taskId = task.taskId;
            startDownload(taskId, task);
            // 异步状态机，最终汇总经 resultSender_ 发出；空 taskId 让 Client 抑制同步应答
            return {std::string{}, false, {}, {}};
        }
        if (task.action == "file_upload") return startUpload(task);
        return {task.taskId, false, {}, "unknown file action"};
    } catch (const platform::PlatformError& e) {
        return {task.taskId, false, {}, e.what()};
    }
}

protocol::TaskResult FileExecutor::startUpload(protocol::Task& task) {
    Upload up;
    up.path = task.payload.value("path", std::string{});
    up.size = task.payload.value("size", size_t{0});
    up.expectedSha = task.payload.value("sha256", std::string{});
    size_t resumeFrom = task.payload.value("resumeFrom", size_t{0});
    // 目标为目录时自动追加本机文件名（与 scp 行为一致）
    const auto fname = task.payload.value("filename", std::string{});
    if (!fname.empty() && !up.path.empty()) {
        std::error_code ec;
#ifdef _WIN32
        if (std::filesystem::is_directory(std::filesystem::path(u8ToWide(up.path)), ec))
#else
        if (std::filesystem::is_directory(up.path, ec))
#endif
        {
            // filename 只取末段，防路径穿越（恶意构造的 filename 可带 ../）
            auto base = fname;
            const auto cut = base.find_last_of("/\\");
            if (cut != std::string::npos) base = base.substr(cut + 1);
            if (base.empty() || base == "." || base == "..") {
                return {task.taskId, false, {}, "invalid filename"};
            }
            if (up.path.back() != '\\' && up.path.back() != '/') up.path += '/';
            up.path += base;
        }
    }
    if (up.path.empty() || up.size == 0 || up.expectedSha.empty()) {
        return {task.taskId, false, {}, "payload.path/size/sha256 are required"};
    }

    // 续传：sha 前缀播种用只读探测流完成并关闭后再开写句柄（Windows 同文件双句柄会共享冲突）
    size_t effectiveResume = 0;
    if (resumeFrom > 0) {
        std::ifstream probe = openIn(up.path);
        if (probe) {
            probe.seekg(0, std::ios::end);
            const auto have = static_cast<size_t>(std::max<std::streamoff>(0, probe.tellg()));
            effectiveResume = std::min({resumeFrom, have, up.size});
            if (effectiveResume > 0) {
                // 用已有前缀播种 sha，使最终摘要覆盖全文件
                const size_t seeded = seedShaPrefix(probe, effectiveResume, up.sha);
                if (seeded != effectiveResume) effectiveResume = 0; // 前缀读不满，放弃续传
            }
        }
        // probe 在此关闭，下面才开写句柄
    }

    if (effectiveResume > 0) {
        up.file = openOutAppend(up.path);
        if (!up.file) {
            return {task.taskId, false, {}, "cannot open file for resume: " + up.path};
        }
    } else {
        up.file = openOutTrunc(up.path);
        if (!up.file) {
            return {task.taskId, false, {}, "cannot open file for writing: " + up.path};
        }
        up.sha = Sha256(); // 全新摘要（含续传回退场景，清掉可能已播种的前缀）
    }
    up.received = effectiveResume;
    up.file.seekp(static_cast<std::streamoff>(effectiveResume));

    const std::string taskId = task.taskId;
    uploads_[taskId] = std::move(up);
    // 就绪应答携带实际续传起点
    const std::string readyData =
        nlohmann::json{{"ready", true}, {"resumeFrom", effectiveResume}}.dump();
    return {taskId, true, readyData, {}};
}

void FileExecutor::startDownload(const std::string& taskId, protocol::Task& task) {
    const std::string path = task.payload.value("path", std::string{});
    if (path.empty()) {
        if (resultSender_)
            resultSender_({taskId, false, {}, "payload.path is required"});
        return;
    }
    if (!binarySender_) {
        if (resultSender_)
            resultSender_({taskId, false, {}, "binary sender not configured"});
        return;
    }
    const size_t resumeFrom = task.payload.value("resumeFrom", size_t{0});
    const size_t chunkSize =
        std::max<size_t>(4096, task.payload.value("chunkSize", kChunkSize));
    const size_t windowSize =
        std::clamp<size_t>(task.payload.value("windowSize", kWindowSize), 1, 64);

    std::ifstream in = openIn(path);
    if (!in) {
        if (resultSender_)
            resultSender_({taskId, false, {}, "cannot open file: " + path});
        return;
    }
    in.seekg(0, std::ios::end);
    const auto size = static_cast<size_t>(std::max<std::streamoff>(0, in.tellg()));

    Download dl;
    dl.size = size;
    dl.chunkSize = chunkSize;
    dl.windowSize = windowSize;
    // 续传：先把 [0, resumeFrom) 喂给 sha（不发送），保证最终摘要覆盖全文件
    size_t effectiveResume = std::min(resumeFrom, size);
    if (effectiveResume > 0) seedShaPrefix(in, effectiveResume, dl.sha);
    in.clear();
    in.seekg(static_cast<std::streamoff>(effectiveResume), std::ios::beg);

    dl.file = std::move(in);
    dl.nextOffset = effectiveResume;
    dl.ackedOffset = effectiveResume;
    dl.seq = 0;
    downloads_[taskId] = std::move(dl);

    pumpDownload(taskId);
}

void FileExecutor::pumpDownload(const std::string& taskId) {
    auto it = downloads_.find(taskId);
    if (it == downloads_.end()) return;
    Download& dl = it->second;

    const std::string taskIdBytes = common::uuidToBytes(taskId);
    std::vector<char> buf(dl.chunkSize);

    // 窗口约束：在途未确认字节 nextOffset - ackedOffset 须 < windowSize*chunkSize
    while (dl.nextOffset < dl.size &&
           (dl.nextOffset - dl.ackedOffset) < dl.windowSize * dl.chunkSize) {
        const size_t want = std::min(dl.chunkSize, dl.size - dl.nextOffset);
        dl.file.read(buf.data(), static_cast<std::streamsize>(want));
        const size_t got = static_cast<size_t>(dl.file.gcount());
        if (got == 0) break; // 读失败则停止，汇总时 sha 会暴露不一致
        dl.sha.update(buf.data(), got);
        const std::string frame =
            buildFileFrame(taskIdBytes, dl.seq, dl.nextOffset, buf.data(), got);
        binarySender_(frame.data(), frame.size());
        dl.nextOffset += got;
        ++dl.seq;
    }

    if (dl.nextOffset >= dl.size) {
        if (resultSender_) {
            const nlohmann::json summary = {{"size", dl.size},
                                            {"sha256", dl.sha.finish()}};
            resultSender_({taskId, true, summary.dump(), {}});
        }
        downloads_.erase(it);
    }
}

void FileExecutor::onFileAck(const std::string& taskId, size_t ackedOffset) {
    auto it = downloads_.find(taskId);
    if (it == downloads_.end()) return;
    Download& dl = it->second;

    if (ackedOffset > dl.ackedOffset) dl.ackedOffset = ackedOffset;
    pumpDownload(taskId);
}

bool FileExecutor::onBinary(const void* data, size_t len) {
    if (len < kHeaderSize) return false;
    const auto* bytes = static_cast<const uint8_t*>(data);
    if (bytes[20] != kStreamFile) return false;

    const std::string taskId = common::uuidFromBytes(
        std::string(reinterpret_cast<const char*>(data), 16));
    auto it = uploads_.find(taskId);
    if (it == uploads_.end()) return false;

    Upload& up = it->second;
    const size_t offset = parseOffset(bytes);
    const size_t payloadLen = len - kHeaderSize;
    const char* payload = reinterpret_cast<const char*>(data) + kHeaderSize;

    if (offset == up.received && payloadLen > 0) {
        up.file.seekp(static_cast<std::streamoff>(offset));
        up.file.write(payload, static_cast<std::streamsize>(payloadLen));
        up.sha.update(payload, payloadLen);
        up.received = offset + payloadLen;
        sendAck(taskId, up.received);
        if (up.received >= up.size) finishUpload(taskId);
    } else if (offset < up.received) {
        sendAck(taskId, up.received);
    }
    return true;
}

void FileExecutor::finishUpload(const std::string& taskId) {
    auto it = uploads_.find(taskId);
    if (it == uploads_.end()) return;
    Upload up = std::move(it->second);
    uploads_.erase(it);
    up.file.close();

    const std::string actual = up.sha.finish();
    if (up.received == up.size && actual == up.expectedSha) {
        if (resultSender_) resultSender_({taskId, true, "ok", {}});
    } else {
        if (resultSender_)
            resultSender_({taskId, false, {}, "sha256 mismatch after upload"});
    }
}

void FileExecutor::sendAck(const std::string& taskId, size_t ackedOffset) {
    if (!textSender_) return;
    textSender_(nlohmann::json{{"type", "file_ack"},
                               {"taskId", taskId},
                               {"ackedOffset", ackedOffset}}
                    .dump());
}

} // namespace echonode::executor
