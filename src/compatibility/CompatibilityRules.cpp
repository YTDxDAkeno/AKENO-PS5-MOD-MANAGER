// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/compatibility/CompatibilityRules.hpp"

#include <algorithm>

#include "akeno/core/Strings.hpp"

namespace akeno::compatibility {

using providers::CompatibilityStatus;

std::string_view toString(Risk risk) noexcept {
    switch (risk) {
        case Risk::Low: return "LOW";
        case Risk::Medium: return "MEDIUM";
        case Risk::High: return "HIGH";
        case Risk::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

providers::CompatibilityStatus summaryLabel(const mods::ModSummaryEntry& entry,
                                            const std::optional<InstalledGame>& game) {
    const bool versionListed =
        game && !game->version.empty() &&
        std::find(entry.gameVersions.begin(), entry.gameVersions.end(), game->version) != entry.gameVersions.end();
    const bool versionDiffers = game && !game->version.empty() && !entry.gameVersions.empty() && !versionListed;
    switch (entry.claimed) {
        case mods::ClaimedStatus::Verified:
            if (versionListed) return CompatibilityStatus::Verified;
            return CompatibilityStatus::Experimental;
        case mods::ClaimedStatus::Likely:
            return versionDiffers ? CompatibilityStatus::Experimental : CompatibilityStatus::Likely;
        case mods::ClaimedStatus::Experimental: return CompatibilityStatus::Experimental;
        case mods::ClaimedStatus::PcOnly: return CompatibilityStatus::PcOnly;
        case mods::ClaimedStatus::Incompatible: return CompatibilityStatus::Incompatible;
        case mods::ClaimedStatus::Unknown: return CompatibilityStatus::Unknown;
    }
    return CompatibilityStatus::Unknown;
}

CompatibilityResult evaluateManifest(const mods::ModManifest& m, const std::optional<InstalledGame>& game) {
    CompatibilityResult r;
    auto line = [&](std::string label, std::string value, CheckMark mark) {
        r.lines.push_back({std::move(label), std::move(value), mark});
    };

    // Platform.
    const bool ps5 = m.platform == "ps5";
    line("Platform", m.platform.empty() ? "not stated" : m.platform, ps5 ? CheckMark::Pass : CheckMark::Fail);

    // Title ID.
    bool titleMatches = false;
    if (game) {
        titleMatches = std::find(m.titleIds.begin(), m.titleIds.end(), game->titleId) != m.titleIds.end();
        line("PS5 Title ID", game->titleId, titleMatches ? CheckMark::Pass : CheckMark::Fail);
    } else {
        line("PS5 Title ID", "game not installed", CheckMark::Unknown);
    }

    // Game version.
    bool versionMatches = false;
    if (game && !game->version.empty()) {
        versionMatches = std::find(m.gameVersions.begin(), m.gameVersions.end(), game->version) != m.gameVersions.end();
        std::string value = "installed " + game->version;
        if (!m.gameVersions.empty()) value += ", checked with " + m.gameVersions.front() +
                                              (m.gameVersions.size() > 1 ? " and others" : "");
        line("Game version", value,
             versionMatches ? CheckMark::Pass : (m.gameVersions.empty() ? CheckMark::Unknown : CheckMark::Warn));
    } else {
        line("Game version", game ? "installed version unknown" : "-", CheckMark::Unknown);
    }

    // Mod type.
    line("Mod type", m.modTypeText.empty() ? "not stated" : m.modTypeText,
         mods::isInstallableType(m.modType) ? CheckMark::Pass : CheckMark::Warn);

    // Contents are only known after download (Phase 4).
    line("Native code (DLL/EXE/ELF)", "checked after download", CheckMark::Unknown);

    // Status.
    if (!ps5) {
        r.status = CompatibilityStatus::Incompatible;
        r.reasons.push_back("This is not a PS5 mod.");
    } else if (m.claimed == mods::ClaimedStatus::PcOnly) {
        r.status = CompatibilityStatus::PcOnly;
        r.reasons.push_back("The catalogue marks this mod as PC only; it needs a manual PS5 port.");
    } else if (m.claimed == mods::ClaimedStatus::Incompatible) {
        r.status = CompatibilityStatus::Incompatible;
        r.reasons.push_back("The catalogue marks this mod as incompatible with PS5.");
    } else if (game && !titleMatches) {
        r.status = CompatibilityStatus::Incompatible;
        r.reasons.push_back("This mod is for a different game or region (title ID " + game->titleId + " is not listed).");
    } else if (m.claimed == mods::ClaimedStatus::Verified) {
        if (game && versionMatches) {
            r.status = CompatibilityStatus::Verified;
        } else {
            r.status = CompatibilityStatus::Experimental;
            if (!game) {
                r.reasons.push_back("The game is not installed, so the version cannot be compared.");
            } else if (game->version.empty()) {
                r.reasons.push_back("The installed game version is unknown.");
            } else if (m.gameVersions.empty()) {
                r.reasons.push_back("The catalogue does not say which game version was verified.");
            } else {
                r.reasons.push_back(strings::concat("Verified for game version ", m.gameVersions.front(),
                                                    "; installed version is ", game->version, "."));
            }
        }
    } else if (m.claimed == mods::ClaimedStatus::Likely) {
        if (game && !game->version.empty() && !m.gameVersions.empty() && !versionMatches) {
            r.status = CompatibilityStatus::Experimental;
            r.reasons.push_back(strings::concat("Checked with game version ", m.gameVersions.front(),
                                                "; installed version is ", game->version, "."));
        } else {
            r.status = CompatibilityStatus::Likely;
            r.reasons.push_back("Expected to work, but not verified on this game version.");
        }
    } else if (m.claimed == mods::ClaimedStatus::Experimental) {
        r.status = CompatibilityStatus::Experimental;
        r.reasons.push_back("The catalogue marks this mod as experimental.");
    } else {
        r.status = CompatibilityStatus::Unknown;
        r.reasons.push_back("There is not enough information to judge compatibility.");
    }
    if (!mods::isInstallableType(m.modType) && r.status != CompatibilityStatus::Incompatible &&
        r.status != CompatibilityStatus::PcOnly) {
        r.reasons.push_back("Mods of type '" + (m.modTypeText.empty() ? std::string("unknown") : m.modTypeText) +
                            "' are not supported for installation.");
    }

    switch (r.status) {
        case CompatibilityStatus::Verified: r.risk = Risk::Low; break;
        case CompatibilityStatus::Likely: r.risk = Risk::Medium; break;
        case CompatibilityStatus::Experimental: r.risk = Risk::High; break;
        case CompatibilityStatus::Unknown: r.risk = Risk::Unknown; break;
        case CompatibilityStatus::PcOnly:
        case CompatibilityStatus::Incompatible: r.risk = Risk::High; break;
    }
    const bool statusAllows = r.status == CompatibilityStatus::Verified || r.status == CompatibilityStatus::Likely ||
                              r.status == CompatibilityStatus::Experimental;
    r.installable = statusAllows && game.has_value() && mods::isInstallableType(m.modType) &&
                    m.installMethod == "shadowmount-overlay" && m.format != mods::ArchiveFormat::Unknown &&
                    m.format != mods::ArchiveFormat::SevenZip;
    r.needsConfirmation = r.status == CompatibilityStatus::Experimental;
    return r;
}

}  // namespace akeno::compatibility
