// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"

#include "TestSupport.hpp"
#include "akeno/core/OperationJournal.hpp"

using namespace akeno;
namespace fs = std::filesystem;

namespace {

struct JournalFixture {
    test::TempDir dir;
    security::SafeFs safeFs{security::WriteGuard::create({dir.path()}).value()};
    fs::path file = dir.path() / "operation_state.json";
    fs::path staging = dir.path() / "staging";
};

}  // namespace

TEST_CASE("journal round-trips an in-progress operation") {
    JournalFixture f;
    OperationJournal journal(f.file, f.safeFs);
    auto empty = journal.load();
    REQUIRE(empty.ok());
    CHECK_FALSE(empty.value().has_value());

    OperationState state;
    state.operationId = "install-PPSA01234-1";
    state.kind = "install";
    state.titleId = "PPSA01234";
    state.description = "Installing Example";
    REQUIRE(journal.begin(state).ok());
    REQUIRE(journal.recordStep("extract").ok());
    REQUIRE(journal.addStagingPath(f.staging / "op1").ok());
    REQUIRE(journal.markStepCompleted("extract").ok());

    OperationJournal reader(f.file, f.safeFs);
    auto loaded = reader.load();
    REQUIRE(loaded.ok());
    REQUIRE(loaded.value().has_value());
    const OperationState& got = *loaded.value();
    CHECK(got.operationId == "install-PPSA01234-1");
    CHECK(got.step == "extract");
    REQUIRE(got.completedSteps.size() == 1);
    REQUIRE(got.stagingPaths.size() == 1);
    CHECK(got.stagingPaths[0] == f.staging / "op1");
    CHECK_FALSE(got.activeOverlayTouched);

    auto advice = adviseRecovery(got);
    CHECK(advice.headline == "Interrupted installation detected.");
    CHECK(advice.explanation == "No active overlay was changed.");
    CHECK(advice.canCleanStaging);
    CHECK_FALSE(advice.needsOverlayRestore);

    REQUIRE(journal.complete().ok());
    CHECK_FALSE(fs::exists(f.file));
}

TEST_CASE("a second operation cannot start while one is in progress") {
    JournalFixture f;
    OperationJournal journal(f.file, f.safeFs);
    OperationState state;
    state.operationId = "a";
    state.kind = "install";
    REQUIRE(journal.begin(state).ok());
    state.operationId = "b";
    auto second = journal.begin(state);
    REQUIRE_FALSE(second.ok());
    CHECK(second.error().code == ErrorCode::Busy);
}

TEST_CASE("cleanStaging deletes listed staging paths and clears the journal") {
    JournalFixture f;
    fs::create_directories(f.staging / "op1" / "nested");
    test::writeText(f.staging / "op1" / "nested" / "file.bin", "data");
    OperationJournal journal(f.file, f.safeFs);
    OperationState state;
    state.operationId = "op1";
    state.kind = "install";
    state.stagingPaths = {f.staging / "op1"};
    REQUIRE(journal.begin(state).ok());

    REQUIRE(journal.cleanStaging(state, f.staging).ok());
    CHECK_FALSE(fs::exists(f.staging / "op1"));
    CHECK_FALSE(fs::exists(f.file));
    CHECK(fs::exists(f.staging));
}

TEST_CASE("cleanStaging refuses paths outside staging and deletes nothing") {
    JournalFixture f;
    fs::create_directories(f.staging / "ok");
    fs::create_directories(f.dir.path() / "mods" / "keep");
    OperationJournal journal(f.file, f.safeFs);
    OperationState state;
    state.operationId = "evil";
    state.kind = "install";
    state.stagingPaths = {f.staging / "ok", f.dir.path() / "mods" / "keep"};
    REQUIRE(journal.begin(state).ok());

    auto result = journal.cleanStaging(state, f.staging);
    REQUIRE_FALSE(result.ok());
    CHECK(result.error().code == ErrorCode::SafetyViolation);
    CHECK(fs::exists(f.staging / "ok"));
    CHECK(fs::exists(f.dir.path() / "mods" / "keep"));

    state.stagingPaths = {f.staging};  // the staging root itself is not deletable either
    CHECK_FALSE(journal.cleanStaging(state, f.staging).ok());
    CHECK(fs::exists(f.staging));
}

TEST_CASE("a damaged journal is reported, not silently ignored") {
    JournalFixture f;
    test::writeText(f.file, "{ this is not json");
    OperationJournal journal(f.file, f.safeFs);
    auto loaded = journal.load();
    CHECK_FALSE(loaded.ok());

    test::writeText(f.file, R"({"schemaVersion": 99, "operationId": "x", "kind": "install"})");
    CHECK_FALSE(journal.load().ok());
}

TEST_CASE("overlay-touching operations need a restore") {
    OperationState state;
    state.operationId = "x";
    state.kind = "activate";
    state.activeOverlayTouched = true;
    auto advice = adviseRecovery(state);
    CHECK(advice.needsOverlayRestore);
    CHECK(advice.headline == "Interrupted overlay activation detected.");
}

TEST_CASE("leftover staging entries of Akeno are removed, anything else stays") {
    test::TempDir dir;
    security::SafeFs safeFs{security::WriteGuard::create({dir.path()}).value()};
    const auto staging = dir.path() / "staging";
    std::filesystem::create_directories(staging / "check-a387e572f9c8e51f-20261007-150415" / "akeno-download-test");
    test::writeText(staging / "check-a387e572f9c8e51f-20261007-150415" / "akeno-download-test" / "README.txt", "x");
    test::writeText(staging / "link-probe-a", "probe");
    test::writeText(staging / "notes.txt", "not Akeno's");
    auto removed = removeLeftoverStaging(safeFs, staging);
    CHECK(removed.size() == 2);
    CHECK_FALSE(std::filesystem::exists(staging / "check-a387e572f9c8e51f-20261007-150415"));
    CHECK_FALSE(std::filesystem::exists(staging / "link-probe-a"));
    CHECK(std::filesystem::exists(staging / "notes.txt"));
    CHECK(removeLeftoverStaging(safeFs, dir.path() / "missing").empty());
}
