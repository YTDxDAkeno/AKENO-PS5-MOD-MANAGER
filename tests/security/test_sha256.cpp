// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"

#include "TestSupport.hpp"
#include "akeno/security/Sha256.hpp"

using namespace akeno;
using namespace akeno::security;

TEST_CASE("SHA-256 matches the FIPS 180-2 test vectors") {
    CHECK(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    Sha256 million;
    std::string chunk(1000, 'a');
    for (int i = 0; i < 1000; ++i) million.update(chunk);
    CHECK(million.finishHex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("files are hashed in chunks") {
    test::TempDir dir;
    std::string content(3 * 1024 * 1024 + 17, 'x');
    test::writeText(dir.path() / "f.bin", content);
    std::uint64_t lastProgress = 0;
    auto hash = sha256File(dir.path() / "f.bin", nullptr, [&](std::uint64_t done) { lastProgress = done; });
    REQUIRE(hash.ok());
    CHECK(hash.value() == sha256Hex(content));
    CHECK(lastProgress == content.size());
    CHECK(sha256File(dir.path() / "missing").error().code == ErrorCode::NotFound);

    CancellationToken token;
    token.cancel();
    CHECK(sha256File(dir.path() / "f.bin", &token).error().code == ErrorCode::Cancelled);
}

TEST_CASE("hex digest validation") {
    CHECK(isSha256Hex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_FALSE(isSha256Hex("E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855"));
    CHECK_FALSE(isSha256Hex("e3b0"));
    CHECK_FALSE(isSha256Hex("g3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
}
