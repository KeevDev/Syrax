cmake_minimum_required(VERSION 3.25)
project(dkv LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)
FetchContent_Declare(syrax
    GIT_REPOSITORY https://github.com/KeevDev/Syrax.git
    GIT_TAG        main
)
FetchContent_MakeAvailable(syrax)

# CONFIGURE_DEPENDS hace que agregar archivos no requiera tocar este CMake:
# cmake reescanea las fuentes en cada build.
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
     ${CMAKE_CURRENT_SOURCE_DIR}/database/*.cpp)

add_executable(dkv ${SOURCES})
target_include_directories(dkv PRIVATE src database)
target_link_libraries(dkv PRIVATE syrax::syrax)
