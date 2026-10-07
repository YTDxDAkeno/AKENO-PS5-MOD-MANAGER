// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace akeno::app {

enum class RunMode { Interactive, SelfCheck, ListGames, Help, Version };

struct CommandLine {
    RunMode mode = RunMode::Interactive;
    std::optional<std::filesystem::path> dataRoot;
    std::optional<int> shadowMountPort;
    std::optional<std::string> catalogueUrl;
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
