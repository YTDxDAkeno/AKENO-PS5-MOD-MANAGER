# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(akeno_tests
    test_main.cpp
    support/LocalHttpServer.cpp
    core/test_strings.cpp
    core/test_json.cpp
    core/test_tasks.cpp
    core/test_operation_journal.cpp
    logging/test_redactor.cpp
    logging/test_logger.cpp
    security/test_path_guard.cpp
    security/test_safe_fs.cpp
    platform/test_platform.cpp
    network/test_url.cpp
    network/test_http_policy.cpp
    network/test_curl_client.cpp
    database/test_migrations.cpp
    database/test_settings.cpp
    games/test_games.cpp
    shadowmount/test_shadowmount.cpp
    app/test_system_check.cpp
    app/test_command_line.cpp
)
if(TARGET akeno_ui)
    target_sources(akeno_tests PRIVATE
        ui/test_input.cpp
        ui/test_widgets.cpp
        ui/test_screens.cpp
    )
    target_link_libraries(akeno_tests PRIVATE akeno_ui)
endif()

target_include_directories(akeno_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_compile_definitions(akeno_tests PRIVATE AKENO_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
target_link_libraries(akeno_tests PRIVATE akeno_core akeno_warnings akeno_sqlite3 PkgConfig::CURL)

add_test(NAME akeno_tests COMMAND akeno_tests)
