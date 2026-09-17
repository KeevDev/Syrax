cmake_minimum_required(VERSION 3.25)
project(syrax LANGUAGES CXX VERSION 0.6.0)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Lo mismo que hace el CMakeLists que genera `syrax new`, y que a este le
# faltaba: sin esto, cada reconfiguracion recompila Drogon entero (~120
# unidades) aunque el codigo sea identico al de hace un minuto.
find_program(CCACHE ccache)
if(CCACHE)
    set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE})
    message(STATUS "ccache: ${CCACHE}")
endif()

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

# Las versiones fijadas, en un solo sitio: las usan tanto FetchContent como
# el find_package de SYRAX_USE_SYSTEM_DEPS, y desincronizarlas seria compilar
# contra una y decir que es otra.
set(SYRAX_GLAZE_VERSION  8.4.0)
set(SYRAX_DROGON_VERSION 1.9.13)

# --- Glaze: reflection, JSON, JSON Schema ---------------------------------
FetchContent_Declare(glaze
    GIT_REPOSITORY https://github.com/stephenberry/glaze.git
    GIT_TAG        v${SYRAX_GLAZE_VERSION}
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
    GIT_TAG        v${SYRAX_DROGON_VERSION}
    GIT_SHALLOW    TRUE
)

# Drogon v1.9.13 trae un trantor que declara cmake_minimum_required(3.5).
# CMake 4 ya elimino la compatibilidad con <3.5 y avisa de que <3.10 cae
# despues: cuando eso pase, trantor deja de configurar y con el se cae todo
# proyecto que use Syrax. No es un warning cosmetico, es una cuenta atras.
#
# CMAKE_POLICY_VERSION_MINIMUM existe justo para esto: aplica un minimo de
# politicas a una dependencia sin tocar su codigo. En CMake < 4 la variable
# no existe y se ignora, asi que es seguro ponerla siempre.
#
# Se acota a las dependencias y se restaura despues: el proyecto del usuario
# debe seguir viendo sus propios avisos de politicas.
set(_syrax_policy_min_backup "${CMAKE_POLICY_VERSION_MINIMUM}")
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)

# Por defecto las dependencias se bajan y se fijan a una version concreta, que
# es lo unico que hace reproducible el build de todo el que use Syrax.
#
# SYRAX_USE_SYSTEM_DEPS invierte eso para un caso concreto: una imagen de Docker
# o un CI donde Drogon YA esta compilado, donde bajarlo y compilarlo otra vez
# son minutos por build sin ninguna ganancia. Va apagado por defecto a
# proposito: tener Drogon instalado en la maquina no debe cambiar contra que
# compila tu proyecto sin que lo hayas pedido.
#
# Cuando se enciende, la version se exige EXACTA. Una Drogon distinta de la que
# se probo es peor que compilarla: los fallos aparecen en ejecucion.
option(SYRAX_USE_SYSTEM_DEPS
       "Usar Drogon y Glaze ya instalados en vez de bajarlos (para imagenes y CI)" OFF)

set(_syrax_fetch glaze drogon)

if(SYRAX_USE_SYSTEM_DEPS)
    find_package(Drogon ${SYRAX_DROGON_VERSION} EXACT QUIET)
    find_package(glaze ${SYRAX_GLAZE_VERSION} EXACT QUIET)

    if(Drogon_FOUND)
        list(REMOVE_ITEM _syrax_fetch drogon)
        message(STATUS "syrax: Drogon ${SYRAX_DROGON_VERSION} del sistema (${Drogon_DIR})")
    else()
        message(STATUS "syrax: Drogon ${SYRAX_DROGON_VERSION} no esta instalado, se baja")
    endif()
    if(glaze_FOUND)
        list(REMOVE_ITEM _syrax_fetch glaze)
        message(STATUS "syrax: glaze ${SYRAX_GLAZE_VERSION} del sistema (${glaze_DIR})")
    endif()
endif()

if(_syrax_fetch)
    FetchContent_MakeAvailable(${_syrax_fetch})
endif()

# El nombre del target cambia segun de donde venga: find_package instala
# Drogon::Drogon, FetchContent deja `drogon`.
if(TARGET Drogon::Drogon)
    set(SYRAX_DROGON_TARGET Drogon::Drogon)
else()
    set(SYRAX_DROGON_TARGET drogon)
endif()

set(CMAKE_POLICY_VERSION_MINIMUM "${_syrax_policy_min_backup}")
unset(_syrax_policy_min_backup)
unset(_syrax_fetch)

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
target_link_libraries(syrax INTERFACE ${SYRAX_DROGON_TARGET} glaze::glaze OpenSSL::Crypto)

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
