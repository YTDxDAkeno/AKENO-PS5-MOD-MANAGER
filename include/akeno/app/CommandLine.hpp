// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace akeno::app {

enum class RunMode { Interactive, SelfCheck, ListGames, DownloadTest, Help, Version };

struct CommandLine {
    RunMode mode = RunMode::Interactive;
    std::optional<std::filesystem::path> dataRoot;
    std::optional<int> shadowMountPort;
    std::optional<std::string> catalogueUrl;
    // --download-test with another file than the built-in one (all three together).
    std::optional<std::string> downloadTestUrl;
    std::optional<std::string> downloadTestSha256;
    std::optional<std::uint64_t> downloadTestSize;
    int windowWidth = 1280;   // host window only; the console always renders 1920x1080
    int windowHeight = 720;
    bool fullscreen = false;
    bool verbose = false;
    std::string uiScript;     // developer option: scripted input and screenshots
    std::vector<std::string> errors;
};

CommandLine parseCommandLine(int argc, const char* const* argv);
std::string usageText();

}  // namespace akeno::app
