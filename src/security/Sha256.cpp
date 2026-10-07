// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/security/Sha256.hpp"

#include <cerrno>
#include <cstring>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <openssl/evp.h>

namespace akeno::security {

struct Sha256::State {
    EVP_MD_CTX* ctx = nullptr;
    bool finished = false;
};

Sha256::Sha256() : state_(std::make_unique<State>()) {
    state_->ctx = EVP_MD_CTX_new();
    if (state_->ctx != nullptr) {
        EVP_DigestInit_ex(state_->ctx, EVP_sha256(), nullptr);
    }
}

Sha256::~Sha256() {
    if (state_ && state_->ctx != nullptr) {
        EVP_MD_CTX_free(state_->ctx);
    }
}

Sha256::Sha256(Sha256&&) noexcept = default;
Sha256& Sha256::operator=(Sha256&& other) noexcept {
    if (this != &other) {
        if (state_ && state_->ctx != nullptr) EVP_MD_CTX_free(state_->ctx);
        state_ = std::move(other.state_);
    }
    return *this;
}

void Sha256::update(const void* data, std::size_t size) {
    if (state_->ctx != nullptr && !state_->finished && size > 0) {
        EVP_DigestUpdate(state_->ctx, data, size);
    }
}

std::string Sha256::finishHex() {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (state_->ctx == nullptr || state_->finished || EVP_DigestFinal_ex(state_->ctx, digest, &length) != 1) {
        return {};
    }
    state_->finished = true;
    static constexpr char kHex[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(length * 2);
    for (unsigned int i = 0; i < length; ++i) {
        hex.push_back(kHex[digest[i] >> 4]);
        hex.push_back(kHex[digest[i] & 0x0F]);
    }
    return hex;
}

std::string sha256Hex(std::string_view data) {
    Sha256 hasher;
    hasher.update(data);
    return hasher.finishHex();
}

Result<std::string> sha256File(const std::filesystem::path& path, const CancellationToken* cancel,
                               const std::function<void(std::uint64_t)>& progress) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return makeError(errno == ENOENT ? ErrorCode::NotFound : ErrorCode::IoError, "Could not open a file to verify.",
                         path.string() + ": " + std::strerror(errno));
    }
    Sha256 hasher;
    std::vector<char> buffer(1024 * 1024);
    std::uint64_t total = 0;
    while (true) {
        if (cancel != nullptr && cancel->cancelled()) {
            ::close(fd);
            return makeError(ErrorCode::Cancelled, "Verification was cancelled.");
        }
        ssize_t n = ::read(fd, buffer.data(), buffer.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            int err = errno;
            ::close(fd);
            return makeError(ErrorCode::IoError, "Could not read a file to verify.", path.string() + ": " + std::strerror(err));
        }
        if (n == 0) break;
        hasher.update(buffer.data(), static_cast<std::size_t>(n));
        total += static_cast<std::uint64_t>(n);
        if (progress) progress(total);
    }
    ::close(fd);
    std::string hex = hasher.finishHex();
    if (hex.empty()) {
        return makeError(ErrorCode::Internal, "SHA-256 is not available.");
    }
    return hex;
}

bool isSha256Hex(std::string_view text) noexcept {
    if (text.size() != 64) return false;
    for (char c : text) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

}  // namespace akeno::security
