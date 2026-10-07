// SPDX-License-Identifier: GPL-3.0-or-later
// akeno-catalog-check: validates an Akeno Catalogue directory with the same parser the
// application uses, and checks that the documents agree with each other.
//
//   akeno-catalog-check <catalogue-dir> [--allow-loopback]
//
// --allow-loopback accepts http://127.0.0.1 addresses (the demo catalogue for the mock server);
// the published catalogue must use https only. If <catalogue-dir>/files/<archive> exists for a
// download address ending in that file name, its size and SHA-256 are compared as well.
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "akeno/mods/Catalog.hpp"
#include "akeno/network/Url.hpp"
#include "akeno/security/SafeFs.hpp"
#include "akeno/security/Sha256.hpp"

namespace fs = std::filesystem;
using namespace akeno;

namespace {

struct Checker {
    fs::path root;
    bool allowLoopback = false;
    std::vector<std::string> problems;
    int documents = 0;

    void problem(const std::string& where, const std::string& what) { problems.push_back(where + ": " + what); }

    std::optional<std::string> read(const fs::path& relative, std::size_t limit) {
        auto text = security::readFileBounded(root / relative, limit);
        if (!text) {
            problem(relative.string(), text.error().describe());
            return std::nullopt;
        }
        ++documents;
        return std::move(text).value();
    }

    void checkUrl(const std::string& where, const std::string& url, bool required) {
        if (url.empty()) {
            if (required) problem(where, "address missing");
            return;
        }
        auto parsed = network::parseUrl(url);
        if (!parsed) {
            problem(where, "invalid address " + url);
        } else if (!parsed->isHttps() && !(allowLoopback && network::isLoopbackHost(parsed->host))) {
            problem(where, "address must use https: " + url);
        }
    }

    void checkArchive(const std::string& where, const mods::ModManifest& manifest) {
        const std::string name = manifest.downloadUrl.substr(manifest.downloadUrl.find_last_of('/') + 1);
        if (name.empty()) return;
        const fs::path local = root / "files" / name;
        std::error_code ec;
        if (!fs::is_regular_file(local, ec)) return;
        const auto size = fs::file_size(local, ec);
        if (!ec && size != manifest.downloadSize) {
            problem(where, "download.size " + std::to_string(manifest.downloadSize) + " differs from files/" + name +
                               " (" + std::to_string(size) + " bytes)");
        }
        auto digest = security::sha256File(local);
        if (!digest) {
            problem(where, digest.error().describe());
        } else if (digest.value() != manifest.downloadSha256) {
            problem(where, "download.sha256 does not match files/" + name);
        }
    }

    void run() {
        auto indexText = read("index.json", mods::kMaxIndexBytes);
        if (!indexText) return;
        auto index = mods::parseCatalogIndex(*indexText);
        if (!index) {
            problem("index.json", index.error().describe());
            return;
        }
        std::set<std::string> titleIds;
        for (const auto& ref : index->games) {
            for (const auto& id : ref.titleIds) {
                if (!titleIds.insert(id).second) problem("index.json", "title ID " + id + " is listed by two games");
            }
            const fs::path gamePath = fs::path("games") / (ref.id + ".json");
            auto gameText = read(gamePath, mods::kMaxGameFileBytes);
            if (!gameText) continue;
            auto game = mods::parseCatalogGame(*gameText, ref.id);
            if (!game) {
                problem(gamePath.string(), game.error().describe());
                continue;
            }
            if (game->titleIds != ref.titleIds) problem(gamePath.string(), "titleIds differ from index.json");
            if (static_cast<int>(game->mods.size()) != ref.modCount) {
                problem("index.json", "modCount of " + ref.id + " is " + std::to_string(ref.modCount) + " but " +
                                          gamePath.string() + " lists " + std::to_string(game->mods.size()));
            }
            for (const auto& entry : game->mods) {
                const fs::path modPath = fs::path("mods") / ref.id / (entry.id + ".json");
                const std::string where = modPath.string();
                checkUrl(gamePath.string() + " " + entry.id + ".thumbnail", entry.thumbnailUrl, false);
                auto modText = read(modPath, mods::kMaxManifestBytes);
                if (!modText) continue;
                auto manifest = mods::parseModManifest(*modText, entry.id);
                if (!manifest) {
                    problem(where, manifest.error().describe());
                    continue;
                }
                if (manifest->name != entry.name) problem(where, "name differs from the game file");
                if (manifest->version != entry.version) problem(where, "version differs from the game file");
                if (manifest->claimed != entry.claimed) problem(where, "compatibility status differs from the game file");
                if (manifest->titleIds != game->titleIds) problem(where, "game.titleIds differ from the game file");
                if (!entry.gameVersions.empty() && entry.gameVersions != manifest->gameVersions) {
                    problem(where, "game.versions differ from the game file");
                }
                if (entry.downloadSize && *entry.downloadSize != manifest->downloadSize) {
                    problem(where, "download.size differs from the game file");
                }
                if (manifest->claimed == mods::ClaimedStatus::Verified && manifest->gameVersions.empty()) {
                    problem(where, "verified mods must list the game versions they were tested with");
                }
                checkUrl(where + " download.url", manifest->downloadUrl, true);
                checkUrl(where + " media.thumbnail", manifest->thumbnailUrl, false);
                for (const auto& shot : manifest->screenshots) checkUrl(where + " media.screenshots", shot.url, true);
                checkArchive(where, *manifest);
            }
        }
    }
};

}  // namespace

int main(int argc, char** argv) {
    Checker checker;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--allow-loopback") {
            checker.allowLoopback = true;
        } else if (checker.root.empty() && !arg.empty() && arg[0] != '-') {
            checker.root = arg;
        } else {
            std::fprintf(stderr, "usage: akeno-catalog-check <catalogue-dir> [--allow-loopback]\n");
            return 2;
        }
    }
    if (checker.root.empty()) {
        std::fprintf(stderr, "usage: akeno-catalog-check <catalogue-dir> [--allow-loopback]\n");
        return 2;
    }
    checker.run();
    for (const auto& problem : checker.problems) std::printf("ERROR %s\n", problem.c_str());
    std::printf("%d documents checked, %zu problems\n", checker.documents, checker.problems.size());
    return checker.problems.empty() ? 0 : 1;
}
