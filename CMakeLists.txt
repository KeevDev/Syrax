cmake_minimum_required(VERSION 3.25)
project(syrax LANGUAGES CXX VERSION 0.0.1)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Cuando Syrax se consume via FetchContent, el consumidor no debe pagar el
# build del CLI ni del ejemplo.
option(SYRAX_BUILD_EXAMPLES "Build the example app" ${PROJECT_IS_TOP_LEVEL})
option(SYRAX_BUILD_CLI      "Build the syrax CLI"   ${PROJECT_IS_TOP_LEVEL})

include(FetchContent)

# --- Glaze: reflection, JSON, JSON Schema ---------------------------------
FetchContent_Declare(glaze
    GIT_REPOSITORY https://github.com/stephenberry/glaze.git
    GIT_TAG        v8.4.0
    GIT_SHALLOW    TRUE
)

# --- Drogon: HTTP, async, corrutinas --------------------------------------
# Apagamos lo que no usamos: es la diferencia entre 3 y 15 minutos de build.
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_CTL      OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING  OFF CACHE BOOL "" FORCE)
set(BUILD_ORM      OFF CACHE BOOL "" FORCE)
FetchContent_Declare(drogon
    GIT_REPOSITORY https://github.com/drogonframework/drogon.git
    GIT_TAG        v1.9.13
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(glaze drogon)

# --- Syrax: header-only ----------------------------------------------------
add_library(syrax INTERFACE)
add_library(syrax::syrax ALIAS syrax)

target_include_directories(syrax INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(syrax INTERFACE cxx_std_23)
target_link_libraries(syrax INTERFACE drogon glaze::glaze)

if(SYRAX_BUILD_CLI)
    add_subdirectory(cli)
endif()

if(SYRAX_BUILD_EXAMPLES)
    add_subdirectory(example)
endif()

install(DIRECTORY include/ DESTINATION include)
