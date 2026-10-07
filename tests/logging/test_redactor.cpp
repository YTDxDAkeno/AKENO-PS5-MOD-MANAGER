// SPDX-License-Identifier: GPL-3.0-or-later
#include "Doctest.hpp"

#include "akeno/logging/Redactor.hpp"

using akeno::logging::redactSecrets;

TEST_CASE("query-string secrets are masked") {
    CHECK(redactSecrets("GET https://api.example/v1?apikey=abc123&x=1") ==
          "GET https://api.example/v1?apikey=[REDACTED]&x=1");
    CHECK(redactSecrets("url?key=SECRETKEY&expires=123") == "url?key=[REDACTED]&expires=123");
    CHECK(redactSecrets("access_token=tok.en-value") == "access_token=[REDACTED]");
}

TEST_CASE("JSON secrets are masked") {
    CHECK(redactSecrets(R"({"api_key": "s3cr3t", "name": "ok"})") == R"({"api_key": "[REDACTED]", "name": "ok"})");
    CHECK(redactSecrets(R"({"refresh_token":"a\"b","x":1})") == R"({"refresh_token":"[REDACTED]","x":1})");
    CHECK(redactSecrets(R"({"password":"hunter2"})") == R"({"password":"[REDACTED]"})");
}

TEST_CASE("HTTP authentication headers are masked to the end of the line") {
    CHECK(redactSecrets("Authorization: Bearer abc.def.ghi") == "Authorization: Bearer [REDACTED]");
    CHECK(redactSecrets("authorization: Basic dXNlcjpwYXNz\nnext line") ==
          "authorization: Basic [REDACTED]\nnext line");
    CHECK(redactSecrets("Cookie: a=1; b=2") == "Cookie: [REDACTED]");
    CHECK(redactSecrets("X-Api-Key: 12345") == "X-Api-Key: [REDACTED]");
}

TEST_CASE("ordinary text is left alone") {
    CHECK(redactSecrets("tokenizer finished; monkey business") == "tokenizer finished; monkey business");
    CHECK(redactSecrets("the token was refreshed") == "the token was refreshed");
    CHECK(redactSecrets("{\"key\":\"showPs4\"}") == "{\"key\":\"showPs4\"}");  // generic 'key' only in queries
    CHECK(redactSecrets("") == "");
    CHECK(redactSecrets("password") == "password");
    CHECK(redactSecrets("password=") == "password=");
}

TEST_CASE("multiple secrets in one line") {
    CHECK(redactSecrets("token=a secret=b") == "token=[REDACTED] secret=[REDACTED]");
}
