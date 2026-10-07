// SPDX-License-Identifier: GPL-3.0-or-later
// Streaming SHA-256 (OpenSSL EVP). Used for download verification, cache keys and installed
// file integrity. Files are hashed in fixed-size chunks; nothing is loaded whole into memory.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "akeno/core/Result.hpp"
#include "akeno/core/Tasks.hpp"

namespace akeno::security {

class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(Sha256&&) noexcept;
    Sha256& operator=(Sha256&&) noexcept;
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    void update(const void* data, std::size_t size);
    void update(std::string_view data) { update(data.data(), data.size()); }
    // Lower-case hex digest. The object must not be updated afterwards.
    std::string finishHex();

private:
    struct State;
    std::unique_ptr<State> state_;
};

std::string sha256Hex(std::string_view data);

// Hashes a file in 1 MiB chunks. `progress` (optional) receives bytes hashed so far.
Result<std::string> sha256File(const std::filesystem::path& path, const CancellationToken* cancel = nullptr,
                               const std::function<void(std::uint64_t)>& progress = {});

// True for exactly 64 lower-case hexadecimal characters.
bool isSha256Hex(std::string_view text) noexcept;

}  // namespace akeno::security
