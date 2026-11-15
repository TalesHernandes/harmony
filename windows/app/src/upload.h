#pragma once
#include "../../native/src/net.h"
#include <atomic>
#include <filesystem>

namespace app {
using UploadProgress = std::function<void(uint64_t sent, uint64_t total)>;
// Cancellation is thread safe; each WinHTTP operation also has a 5s timeout.
// All functions are blocking and belong on Service's worker, never the UI.
std::string ChatEscape(const std::string& value);
net::Response ChatRequest(net::Client& client, const std::string& method,
    const std::string& path, const json::Value* body = nullptr);
net::Response UploadFile(net::Client& client, const std::string& channel,
    const std::filesystem::path& file, const UploadProgress& progress,
    const std::atomic<bool>& cancel);
net::Response UploadAvatar(net::Client& client, const std::filesystem::path& file);
net::Response DownloadBytes(net::Client& client, const std::string& url,
    size_t limit = 32 * 1024 * 1024);
}
