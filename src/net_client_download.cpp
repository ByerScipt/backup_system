#include "net_internal.hpp"

namespace backup {
namespace network {
using namespace detail;

bool BackupClient::download(const std::string& backupId, const std::string& outputPath,
                            std::string& error, ProgressCallback progress,
                            std::atomic_bool* cancel) {
    try {
        checkCancelled(cancel);
        ensure(validBackupId(backupId), "invalid backup identifier");
        struct stat outputState {};
        ensure(::lstat(outputPath.c_str(), &outputState) != 0, "download output already exists");
        ensure(errno == ENOENT, "cannot inspect download output path");
        Socket socket = connectTo(host_, port_);
        uint32_t request = 0;
        ensure(authenticateClient(socket.get(), request, username_, password_, error), error);
        checkCancelled(cancel);
        std::vector<uint8_t> payload;
        putString(payload, backupId);
        sendFrame(socket.get(), MessageType::DownloadRequest, ++request, payload);
        auto start = receiveFrame(socket.get());
        ensure(start.has_value(), "server closed before download");
        if (start->type == MessageType::Error) throw NetError(errorFromFrame(*start));
        ensure(start->type == MessageType::DownloadStart && start->requestId == request,
               "unexpected download response");
        Reader reader(start->payload);
        uint64_t declaredSize = reader.u64();
        auto digestBytes = reader.bytes(32);
        reader.end();
        std::array<uint8_t, 32> declaredDigest{};
        std::copy(digestBytes.begin(), digestBytes.end(), declaredDigest.begin());
        fs::path output = fs::absolute(outputPath);
        fs::create_directories(output.parent_path());
        fs::path temp = output.string() + ".tmp-" + randomId();
        struct Cleanup {
            fs::path path;
            ~Cleanup() {
                std::error_code ec;
                fs::remove(path, ec);
            }
        } cleanup{temp};
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        ensure(out.good(), "cannot create download output");
        ::chmod(temp.c_str(), 0600);
        uint64_t received = 0;
        while (true) {
            checkCancelled(cancel);
            auto frame = receiveFrame(socket.get());
            ensure(frame.has_value(), "server disconnected during download");
            ensure(frame->requestId == request, "download request identifier changed");
            if (frame->type == MessageType::DownloadEnd) break;
            ensure(frame->type == MessageType::DownloadChunk, "unexpected download message");
            ensure(frame->payload.size() <= declaredSize - received, "download exceeds declared size");
            if (!frame->payload.empty()) {
                out.write(reinterpret_cast<const char*>(frame->payload.data()),
                          static_cast<std::streamsize>(frame->payload.size()));
            }
            ensure(out.good(), "cannot write download output");
            received += frame->payload.size();
            reportProgress(progress, "network-download", received, declaredSize);
        }
        checkCancelled(cancel);
        out.close();
        ensure(received == declaredSize, "downloaded size mismatch");
        ensure(sha256File(temp.string()) == declaredDigest, "downloaded SHA-256 mismatch");
        commitFileNoReplace(temp, output);
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

}  // namespace network
}  // namespace backup
