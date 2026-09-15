#pragma once
#include "IExecutor.hpp"

#include "util/Sha256.hpp"

#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace echonode::executor {

class FileExecutor : public IExecutor {
public:
    using BinarySender = std::function<void(const void*, size_t)>;       // 发 binary frame
    using ResultSender = std::function<void(const protocol::TaskResult&)>; // 延后应答
    using TextSender = std::function<void(const std::string&)>;          // 发 file_ack 等文本

    std::vector<std::string> actions() const override {
        return {"file_upload", "file_download"};
    }
    ~FileExecutor() override = default;

    protocol::TaskResult execute(protocol::Task task) override;

    bool onBinary(const void* data, size_t len);
    void onFileAck(const std::string& taskId, size_t ackedOffset);

    void setBinarySender(BinarySender sender) { binarySender_ = std::move(sender); }
    void setResultSender(ResultSender sender) { resultSender_ = std::move(sender); }
    void setTextSender(TextSender sender) { textSender_ = std::move(sender); }

private:
    static constexpr size_t kChunkSize = 256 * 1024;
    static constexpr size_t kWindowSize = 4;
    static constexpr uint8_t kStreamFile = 0x01;
    static constexpr size_t kHeaderSize = 29;

    struct Upload {
        std::string path;
        size_t size = 0;
        std::string expectedSha;
        std::fstream file;
        common::Sha256 sha;
        size_t received = 0;
    };

    struct Download {
        std::ifstream file;
        size_t size = 0;
        size_t nextOffset = 0;
        size_t ackedOffset = 0;
        size_t chunkSize = kChunkSize;
        size_t windowSize = kWindowSize;
        uint32_t seq = 0;
        common::Sha256 sha;
    };

    protocol::TaskResult startUpload(protocol::Task& task);
    void startDownload(const std::string& taskId, protocol::Task& task);
    void pumpDownload(const std::string& taskId);
    void finishUpload(const std::string& taskId);
    void sendAck(const std::string& taskId, size_t ackedOffset);

    BinarySender binarySender_;
    ResultSender resultSender_;
    TextSender textSender_;
    std::map<std::string, Upload> uploads_;     // taskId → 接收中
    std::map<std::string, Download> downloads_; // taskId → 发送中
};

} // namespace echonode::executor
