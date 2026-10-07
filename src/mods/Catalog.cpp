// SPDX-License-Identifier: GPL-3.0-or-later
#include "akeno/mods/Catalog.hpp"

#include <algorithm>

#include "akeno/core/Json.hpp"
#include "akeno/core/Strings.hpp"
#include "akeno/games/GameInfo.hpp"
#include "akeno/network/Url.hpp"
#include "akeno/security/Sha256.hpp"

namespace akeno::mods {

namespace {

constexpr std::size_t kMaxDescriptionBytes = 16 * 1024;
constexpr std::size_t kMaxListItems = 64;
constexpr std::size_t kMaxGamesInIndex = 2000;
constexpr std::size_t kMaxModsPerGame = 5000;
constexpr std::size_t kMaxScreenshots = 12;

Error fieldError(std::string_view document, std::string_view field, std::string_view problem) {
    return makeError(ErrorCode::SchemaError, "The mod catalogue contains an invalid entry.",
                     strings::concat(document, ": ", field, ": ", problem));
}

Result<json::Json> parseDocument(std::string_view text, std::size_t maxBytes, std::string_view document) {
    auto parsed = json::parseBounded(text, maxBytes);
    if (!parsed) {
        Error error = std::move(parsed).error();
        error.message = "The mod catalogue could not be read.";
        error.detail = strings::concat(document, ": ", error.detail);
        return error;
    }
    if (!parsed->is_object()) {
        return fieldError(document, "(root)", "must be an object");
    }
    auto version = json::getInt(parsed.value(), "schemaVersion");
    if (!version) {
        return fieldError(document, "schemaVersion", "missing");
    }
    if (*version != kCatalogSchemaVersion) {
        return makeError(ErrorCode::Unsupported,
                         "The mod catalogue uses a newer format. Please update Akeno Mod Manager.",
                         strings::concat(document, ": schemaVersion ", *version));
    }
    return parsed;
}

// String list with element validation; non-string elements are an error.
Result<std::vector<std::string>> stringList(const json::Json& object, std::string_view key, std::string_view document,
                                            const std::function<bool(const std::string&)>& valid,
                                            std::size_t maxItems = kMaxListItems) {
    std::vector<std::string> values;
    const json::Json* array = json::getArray(object, key);
    if (array == nullptr) {
        if (object.contains(key)) return fieldError(document, key, "must be an array");
        return values;
    }
    if (array->size() > maxItems) {
        return fieldError(document, key, strings::concat("more than ", maxItems, " entries"));
    }
    for (const auto& item : *array) {
        if (!item.is_string()) return fieldError(document, key, "entries must be strings");
        const auto& text = item.get_ref<const std::string&>();
        if (!valid(text)) return fieldError(document, key, "invalid entry '" + strings::sanitizeForDisplay(text, 64) + "'");
        values.push_back(text);
    }
    return values;
}

bool anyText(const std::string& text) { return !text.empty() && text.size() <= 64; }

Result<std::string> requiredId(const json::Json& object, std::string_view document) {
    auto id = json::getString(object, "id");
    if (!id || !isValidCatalogId(*id)) {
        return fieldError(document, "id", "missing or not a valid identifier");
    }
    return *id;
}

std::string optionalUrl(const json::Json& object, std::string_view key) {
    auto url = json::getString(object, key);
    return (url && isAllowedRemoteUrl(*url)) ? *url : std::string();
}

}  // namespace

bool isValidCatalogId(std::string_view id) noexcept {
    if (id.empty() || id.size() > 63) return false;
    auto alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    if (!alnum(id.front())) return false;
    for (char c : id) {
        if (!alnum(c) && c != '-') return false;
    }
    return true;
}

bool isValidModVersion(std::string_view version) noexcept {
    if (version.empty() || version.size() > 32) return false;
    for (char c : version) {
        bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '+' ||
                  c == '-';
        if (!ok) return false;
    }
    return true;
}

bool isAllowedRemoteUrl(std::string_view url) noexcept {
    auto parsed = network::parseUrl(url);
    if (!parsed) return false;
    return parsed->isHttps() || network::isLoopbackHost(parsed->host);
}

bool isSafeRelativePath(std::string_view path) noexcept {
    if (path.empty()) return true;
    if (path.size() > 512 || path.front() == '/' || path.back() == '/') return false;
    if (path.find('\\') != std::string_view::npos || path.find('\0') != std::string_view::npos) return false;
    for (const std::string& component : strings::split(path, '/')) {
        if (component.empty() || component == "." || component == "..") return false;
        for (char c : component) {
            if (static_cast<unsigned char>(c) < 0x20 || c == ':') return false;
        }
    }
    return true;
}

ClaimedStatus parseClaimedStatus(std::string_view text) noexcept {
    if (text == "verified") return ClaimedStatus::Verified;
    if (text == "likely") return ClaimedStatus::Likely;
    if (text == "experimental") return ClaimedStatus::Experimental;
    if (text == "pc-only") return ClaimedStatus::PcOnly;
    if (text == "incompatible") return ClaimedStatus::Incompatible;
    return ClaimedStatus::Unknown;
}

std::string_view toString(ClaimedStatus status) noexcept {
    switch (status) {
        case ClaimedStatus::Verified: return "verified";
        case ClaimedStatus::Likely: return "likely";
        case ClaimedStatus::Experimental: return "experimental";
        case ClaimedStatus::Unknown: return "unknown";
        case ClaimedStatus::PcOnly: return "pc-only";
        case ClaimedStatus::Incompatible: return "incompatible";
    }
    return "unknown";
}

ModType parseModType(std::string_view text) noexcept {
    if (text == "asset-replacement") return ModType::AssetReplacement;
    if (text == "asset-addition") return ModType::AssetAddition;
    if (text == "config") return ModType::Config;
    if (text == "test-harmless") return ModType::TestHarmless;
    return ModType::Other;
}

std::string_view toString(ModType type) noexcept {
    switch (type) {
        case ModType::AssetReplacement: return "asset-replacement";
        case ModType::AssetAddition: return "asset-addition";
        case ModType::Config: return "config";
        case ModType::TestHarmless: return "test-harmless";
        case ModType::Other: return "other";
    }
    return "other";
}

bool isInstallableType(ModType type) noexcept { return type != ModType::Other; }

ArchiveFormat parseArchiveFormat(std::string_view text) noexcept {
    if (text == "zip") return ArchiveFormat::Zip;
    if (text == "tar") return ArchiveFormat::Tar;
    if (text == "tar.gz") return ArchiveFormat::TarGz;
    if (text == "7z") return ArchiveFormat::SevenZip;
    return ArchiveFormat::Unknown;
}

std::string_view toString(ArchiveFormat format) noexcept {
    switch (format) {
        case ArchiveFormat::Zip: return "zip";
        case ArchiveFormat::Tar: return "tar";
        case ArchiveFormat::TarGz: return "tar.gz";
        case ArchiveFormat::SevenZip: return "7z";
        case ArchiveFormat::Unknown: return "unknown";
    }
    return "unknown";
}

const CatalogGameRef* CatalogIndex::findByTitleId(std::string_view titleId) const {
    for (const auto& game : games) {
        for (const auto& id : game.titleIds) {
            if (id == titleId) return &game;
        }
    }
    return nullptr;
}

const CatalogGameRef* CatalogIndex::findById(std::string_view id) const {
    for (const auto& game : games) {
        if (game.id == id) return &game;
    }
    return nullptr;
}

Result<CatalogIndex> parseCatalogIndex(std::string_view text) {
    constexpr std::string_view kDoc = "index.json";
    auto parsed = parseDocument(text, kMaxIndexBytes, kDoc);
    if (!parsed) return std::move(parsed).error();
    const json::Json& doc = parsed.value();
    CatalogIndex index;
    index.name = json::displayString(doc, "name", "Akeno Catalogue", 128);
    index.updatedAt = json::displayString(doc, "updatedAt", "", 40);
    const json::Json* games = json::getArray(doc, "games");
    if (games == nullptr) return fieldError(kDoc, "games", "missing array");
    if (games->size() > kMaxGamesInIndex) return fieldError(kDoc, "games", "too many entries");
    for (const auto& item : *games) {
        if (!item.is_object()) return fieldError(kDoc, "games[]", "entries must be objects");
        auto id = requiredId(item, kDoc);
        if (!id) return std::move(id).error();
        CatalogGameRef ref;
        ref.id = id.value();
        ref.name = json::displayString(item, "name", ref.id, 128);
        auto titleIds = stringList(item, "titleIds", kDoc, [](const std::string& t) { return games::isValidTitleId(t); }, 16);
        if (!titleIds) return std::move(titleIds).error();
        ref.titleIds = std::move(titleIds).value();
        if (ref.titleIds.empty()) return fieldError(kDoc, "games[" + ref.id + "].titleIds", "must not be empty");
        ref.modCount = static_cast<int>(std::max<std::int64_t>(0, json::getInt(item, "modCount").value_or(0)));
        if (index.findById(ref.id) != nullptr) return fieldError(kDoc, "games", "duplicate id '" + ref.id + "'");
        index.games.push_back(std::move(ref));
    }
    return index;
}

Result<CatalogGame> parseCatalogGame(std::string_view text, std::string_view expectedId) {
    const std::string document = "games/" + std::string(expectedId) + ".json";
    auto parsed = parseDocument(text, kMaxGameFileBytes, document);
    if (!parsed) return std::move(parsed).error();
    const json::Json& doc = parsed.value();
    auto id = requiredId(doc, document);
    if (!id) return std::move(id).error();
    if (id.value() != expectedId) return fieldError(document, "id", "does not match the file name");
    CatalogGame game;
    game.id = id.value();
    game.name = json::displayString(doc, "name", game.id, 128);
    game.engine = json::displayString(doc, "engine", "", 32);
    auto titleIds = stringList(doc, "titleIds", document, [](const std::string& t) { return games::isValidTitleId(t); }, 16);
    if (!titleIds) return std::move(titleIds).error();
    game.titleIds = std::move(titleIds).value();
    const json::Json* mods = json::getArray(doc, "mods");
    if (mods == nullptr) return fieldError(document, "mods", "missing array");
    if (mods->size() > kMaxModsPerGame) return fieldError(document, "mods", "too many entries");
    for (const auto& item : *mods) {
        if (!item.is_object()) return fieldError(document, "mods[]", "entries must be objects");
        auto modId = requiredId(item, document);
        if (!modId) return std::move(modId).error();
        ModSummaryEntry entry;
        entry.id = modId.value();
        entry.name = json::displayString(item, "name", entry.id, 128);
        entry.author = json::displayString(item, "author", "unknown author", 64);
        auto version = json::getString(item, "version");
        entry.version = (version && isValidModVersion(*version)) ? *version : std::string("?");
        entry.summary = json::displayString(item, "summary", "", 300);
        auto categories = stringList(item, "categories", document, anyText, 8);
        if (categories) entry.categories = std::move(categories).value();
        entry.claimed = parseClaimedStatus(json::getString(item, "compatibility").value_or(""));
        auto gameVersions = stringList(item, "gameVersions", document,
                                       [](const std::string& v) { return !v.empty() && v.size() <= 32; });
        if (!gameVersions) return std::move(gameVersions).error();
        entry.gameVersions = std::move(gameVersions).value();
        entry.thumbnailUrl = optionalUrl(item, "thumbnail");
        if (auto size = json::getInt(item, "downloadSize"); size && *size > 0) {
            entry.downloadSize = static_cast<std::uint64_t>(*size);
        }
        entry.updatedAt = json::displayString(item, "updatedAt", "", 40);
        for (const auto& existing : game.mods) {
            if (existing.id == entry.id) return fieldError(document, "mods", "duplicate id '" + entry.id + "'");
        }
        game.mods.push_back(std::move(entry));
    }
    return game;
}

Result<ModManifest> parseModManifest(std::string_view text, std::string_view expectedId) {
    const std::string document = std::string(expectedId) + ".json";
    auto parsed = parseDocument(text, kMaxManifestBytes, document);
    if (!parsed) return std::move(parsed).error();
    const json::Json& doc = parsed.value();
    ModManifest m;
    auto id = requiredId(doc, document);
    if (!id) return std::move(id).error();
    if (id.value() != expectedId) return fieldError(document, "id", "does not match the file name");
    m.id = id.value();
    m.name = json::displayString(doc, "name", m.id, 128);
    m.author = json::displayString(doc, "author", "unknown author", 64);
    auto version = json::getString(doc, "version");
    if (!version || !isValidModVersion(*version)) return fieldError(document, "version", "missing or invalid");
    m.version = *version;
    m.summary = json::displayString(doc, "summary", "", 300);
    m.description = json::displayString(doc, "description", "", kMaxDescriptionBytes);
    auto categories = stringList(doc, "categories", document, anyText, 8);
    if (!categories) return std::move(categories).error();
    m.categories = std::move(categories).value();
    m.license = json::displayString(doc, "license", "", 200);
    m.homepage = optionalUrl(doc, "homepage");
    m.updatedAt = json::displayString(doc, "updatedAt", "", 40);

    const json::Json* game = json::getObject(doc, "game");
    if (game == nullptr) return fieldError(document, "game", "missing object");
    auto titleIds = stringList(*game, "titleIds", document, [](const std::string& t) { return games::isValidTitleId(t); }, 16);
    if (!titleIds) return std::move(titleIds).error();
    m.titleIds = std::move(titleIds).value();
    if (m.titleIds.empty()) return fieldError(document, "game.titleIds", "must not be empty");
    auto versions = stringList(*game, "versions", document, [](const std::string& v) { return !v.empty() && v.size() <= 32; });
    if (!versions) return std::move(versions).error();
    m.gameVersions = std::move(versions).value();

    m.platform = json::displayString(doc, "platform", "", 16);

    if (const json::Json* compat = json::getObject(doc, "compatibility")) {
        m.claimed = parseClaimedStatus(json::getString(*compat, "status").value_or(""));
        m.engine = json::displayString(*compat, "engine", "", 32);
        m.modTypeText = json::displayString(*compat, "modType", "", 32);
        m.modType = parseModType(m.modTypeText);
        m.compatibilityNotes = json::displayString(*compat, "notes", "", 1000);
    }

    const json::Json* download = json::getObject(doc, "download");
    if (download == nullptr) return fieldError(document, "download", "missing object");
    auto url = json::getString(*download, "url");
    if (!url || !isAllowedRemoteUrl(*url)) return fieldError(document, "download.url", "missing or not an https URL");
    m.downloadUrl = *url;
    auto size = json::getInt(*download, "size");
    if (!size || *size <= 0 || static_cast<std::uint64_t>(*size) > kMaxDownloadSize) {
        return fieldError(document, "download.size", "missing or out of range");
    }
    m.downloadSize = static_cast<std::uint64_t>(*size);
    auto sha = json::getString(*download, "sha256");
    if (!sha || !security::isSha256Hex(*sha)) {
        return fieldError(document, "download.sha256", "must be 64 lower-case hex characters");
    }
    m.downloadSha256 = *sha;
    m.format = parseArchiveFormat(json::getString(*download, "format").value_or(""));

    if (const json::Json* install = json::getObject(doc, "installation")) {
        m.installMethod = json::getString(*install, "method").value_or("");
        m.archiveRoot = json::getString(*install, "archiveRoot").value_or("");
        m.targetPrefix = json::getString(*install, "targetPrefix").value_or("");
        if (!isSafeRelativePath(m.archiveRoot)) return fieldError(document, "installation.archiveRoot", "unsafe path");
        if (!isSafeRelativePath(m.targetPrefix)) return fieldError(document, "installation.targetPrefix", "unsafe path");
    }

    if (const json::Json* media = json::getObject(doc, "media")) {
        m.thumbnailUrl = optionalUrl(*media, "thumbnail");
        if (const json::Json* shots = json::getArray(*media, "screenshots")) {
            for (const auto& shot : *shots) {
                if (m.screenshots.size() >= kMaxScreenshots) break;
                if (!shot.is_object()) continue;
                std::string shotUrl = optionalUrl(shot, "url");
                if (shotUrl.empty()) continue;
                m.screenshots.push_back({shotUrl, json::displayString(shot, "caption", "", 120)});
            }
        }
    }

    if (const json::Json* deps = json::getObject(doc, "dependencies")) {
        auto validId = [](const std::string& s) { return isValidCatalogId(s); };
        auto readList = [&](std::string_view key, std::vector<std::string>& out) -> Status {
            auto list = stringList(*deps, key, document, validId);
            if (!list) return std::move(list).error();
            out = std::move(list).value();
            return {};
        };
        AKENO_TRY(readList("requires", m.requires_));
        AKENO_TRY(readList("recommends", m.recommends));
        AKENO_TRY(readList("conflictsWith", m.conflictsWith));
        AKENO_TRY(readList("loadAfter", m.loadAfter));
        AKENO_TRY(readList("loadBefore", m.loadBefore));
    }
    return m;
}

}  // namespace akeno::mods
