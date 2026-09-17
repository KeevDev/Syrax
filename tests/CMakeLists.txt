FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.16.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Catch2)

add_executable(syrax_tests
    schema_test.cpp
    result_test.cpp
    openapi_test.cpp
    db_test.cpp
    test_server.cpp
    http_test.cpp
    auth_test.cpp
    policy_test.cpp
    middleware_test.cpp
    validation_test.cpp
    query_test.cpp
    join_test.cpp
    relations_test.cpp
    envport_test.cpp
    config_test.cpp
    config_typed_test.cpp
    errors_test.cpp
    log_test.cpp
    jobs_test.cpp
    audit_test.cpp
    jwks_crypto_test.cpp
    schedule_test.cpp
    ws_test.cpp
)
target_link_libraries(syrax_tests PRIVATE syrax::syrax Catch2::Catch2WithMain)

# El kit de tests va en su propio binario: syrax_tests ya levanta un servidor
# con test_server.cpp, y Drogon solo admite un app().run() por proceso.
add_executable(syrax_testing_kit_tests testing_kit_test.cpp health_test.cpp ratelimit_shared_test.cpp etag_test.cpp idempotency_test.cpp paginate_http_test.cpp fields_test.cpp http_client_test.cpp metrics_test.cpp uploads_test.cpp jwks_test.cpp)
target_link_libraries(syrax_testing_kit_tests PRIVATE syrax::syrax Catch2::Catch2WithMain)

# Solo se compila, no se ejecuta: es el guardia de que los ejemplos del
# README siguen siendo codigo valido.
add_library(syrax_readme_snippets OBJECT readme_snippets.cpp)
target_link_libraries(syrax_readme_snippets PRIVATE syrax::syrax)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(syrax_tests)
catch_discover_tests(syrax_testing_kit_tests)
