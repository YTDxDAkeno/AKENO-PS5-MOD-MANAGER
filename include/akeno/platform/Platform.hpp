// SPDX-License-Identifier: GPL-3.0-or-later
// Target-specific services. Only src/platform/ may contain target conditionals.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace akeno::platform {

struct FirmwareInfo {
    bool known = false;
    std::uint32_t raw = 0;      // libkernel sdk_ps5_ver, e.g. 0x12200000
    std::string display;        // "12.20", or a description when unknown
};

// Decodes the 0xMMmmxxxx BCD-style firmware word used by libkernel ("12.20").
std::optional<std::string> formatFirmwareVersion(std::uint32_t raw);

// Known homebrew components detected by the presence of their data directories. Detection
// only reads the file system.
struct HomebrewEnvironment {
    bool isConsole = false;              // running on a PS5
    bool dataWritable = false;           // /data (or the host data root) is writable
    std::vector<std::string> components; // e.g. "ShadowMountPlus data (/data/shadowmount)"
    std::string summary;
};

class IPlatform {
public:
    virtual ~IPlatform() = default;

    virtual std::string_view name() const = 0;   // "PS5" or "Host (Linux)"
    virtual bool isConsole() const = 0;
    virtual FirmwareInfo firmware() const = 0;
    virtual HomebrewEnvironment probeHomebrewEnvironment(const std::filesystem::path& dataRoot) const = 0;

    // Shows a system notification (console) or prints it (host). Best effort.
    virtual void notify(std::string_view message) = 0;

    // Default application data directory for this platform.
    virtual std::filesystem::path defaultDataRoot() const = 0;

    // Called when the UI session ends. On the console this closes the application context the
    // homebrew launcher created. Must not be called in headless modes.
    virtual void exitApplicationContext() = 0;
};

std::unique_ptr<IPlatform> createPlatform();

}  // namespace akeno::platform
