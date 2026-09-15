cmake_minimum_required(VERSION 3.25)
project(syrax LANGUAGES CXX VERSION 0.2.1)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Cuando Syrax se consume via FetchContent, el consumidor no debe pagar el
# build del CLI ni del ejemplo.
option(SYRAX_BUILD_EXAMPLES "Build the example app" ${PROJECT_IS_TOP_LEVEL})
option(SYRAX_BUILD_CLI      "Build the syrax CLI"   ${PROJECT_IS_TOP_LEVEL})
option(SYRAX_BUILD_TESTS    "Build the test suite"  ${PROJECT_IS_TOP_LEVEL})

# El CLI no enlaza contra la libreria: solo genera proyectos e invoca cmake.
# Apagar SYRAX_BUILD_LIB evita descargar y compilar Drogon por completo, que
# es lo que permite que instalar el comando tarde segundos y no minutos.
option(SYRAX_BUILD_LIB "Build the syrax library (downloads Drogon + Glaze)" ON)

if(SYRAX_BUILD_EXAMPLES AND NOT SYRAX_BUILD_LIB)
    message(FATAL_ERROR "SYRAX_BUILD_EXAMPLES requiere SYRAX_BUILD_LIB=ON")
endif()

if(SYRAX_BUILD_LIB)
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
set(BUILD_ORM      ON  CACHE BOOL "" FORCE)  # DbClient y corrutinas de BD
FetchContent_Declare(drogon
    GIT_REPOSITORY https://github.com/drogonframework/drogon.git
    GIT_TAG        v1.9.13
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(glaze drogon)

# Los warnings de Drogon no son nuestros y no los podemos arreglar: son ruido
# que esconde los errores propios. Se silencian sin tocar los del codigo del
# usuario.
if(TARGET drogon)
    target_compile_options(drogon PRIVATE -w)
endif()
if(TARGET trantor)
    target_compile_options(trantor PRIVATE -w)
endif()

# --- Syrax: header-only ----------------------------------------------------
add_library(syrax INTERFACE)
add_library(syrax::syrax ALIAS syrax)

target_include_directories(syrax INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(syrax INTERFACE cxx_std_23)
find_package(OpenSSL REQUIRED)
target_link_libraries(syrax INTERFACE drogon glaze::glaze OpenSSL::Crypto)

install(DIRECTORY include/ DESTINATION include)
endif()  # SYRAX_BUILD_LIB

if(SYRAX_BUILD_CLI)
    add_subdirectory(cli)
endif()

if(SYRAX_BUILD_EXAMPLES)
    add_subdirectory(example)
endif()

if(SYRAX_BUILD_TESTS AND SYRAX_BUILD_LIB)
    enable_testing()
    add_subdirectory(tests)
endif()
