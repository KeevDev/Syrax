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
    envport_test.cpp
    config_test.cpp
    jobs_test.cpp
    ws_test.cpp
)
target_link_libraries(syrax_tests PRIVATE syrax::syrax Catch2::Catch2WithMain)

# Solo se compila, no se ejecuta: es el guardia de que los ejemplos del
# README siguen siendo codigo valido.
add_library(syrax_readme_snippets OBJECT readme_snippets.cpp)
target_link_libraries(syrax_readme_snippets PRIVATE syrax::syrax)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(syrax_tests)
