#include "net_internal.hpp"

namespace backup {
namespace network {
using namespace detail;

bool BackupClient::upload(const std::string& archivePath, const std::string& displayName,
                          std::string& backupId, std::string& error,
                          ProgressCallback progress, std::atomic_bool* cancel) {
    try {
        checkCancelled(cancel);
        Socket socket = connectTo(host_, port_);
        uint32_t request = 0;
        ensure(authenticateClient(socket.get(), request, username_, password_, error), error);
        checkCancelled(cancel);
        uint64_t size = static_cast<uint64_t>(fs::file_size(archivePath));
        auto digest = sha256File(archivePath);
        checkCancelled(cancel);
        std::vector<uint8_t> start;
        putString(start, displayName);
        putU64(start, size);
        start.insert(start.end(), digest.begin(), digest.end());
        sendFrame(socket.get(), MessageType::UploadStart, ++request, start);
        auto ready = receiveFrame(socket.get());
        ensure(ready.has_value(), "server closed before upload");
        if (ready->type == MessageType::Error) throw NetError(errorFromFrame(*ready));
        ensure(ready->type == MessageType::UploadReady && ready->requestId == request,
               "unexpected upload response");
        Reader idReader(ready->payload);
        backupId = idReader.string();
        idReader.end();
        std::ifstream in(archivePath, std::ios::binary);
        ensure(in.good(), "cannot open archive for upload");
        std::vector<uint8_t> chunk(kChunkSize);
        uint64_t sent = 0;
        while (in) {
            checkCancelled(cancel);
            in.read(reinterpret_cast<char*>(chunk.data()),
                    static_cast<std::streamsize>(chunk.size()));
            size_t got = static_cast<size_t>(in.gcount());
            if (got == 0) continue;
            sendFrame(socket.get(), MessageType::UploadChunk, request,
                      {chunk.begin(), chunk.begin() + static_cast<ptrdiff_t>(got)});
            sent += got;
            reportProgress(progress, "network-upload", sent, size);
        }
        checkCancelled(cancel);
        sendFrame(socket.get(), MessageType::UploadEnd, request);
        auto result = receiveFrame(socket.get());
        ensure(result.has_value(), "server closed after upload");
        if (result->type == MessageType::Error) throw NetError(errorFromFrame(*result));
        ensure(result->type == MessageType::UploadResult, "unexpected upload completion response");
        Reader done(result->payload);
        backupId = done.string();
        done.end();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

}  // namespace network
}  // namespace backup
