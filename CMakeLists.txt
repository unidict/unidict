cmake_minimum_required(VERSION 3.14)
project(unidict VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ============================================================
# Options
# ============================================================
option(UNIDICT_BUILD_TESTS "Build unidict unit tests" ON)
option(BUILD_SHARED_LIBS "Build shared library" OFF)

# ============================================================
# System dependencies
# ============================================================
find_package(ZLIB REQUIRED)
find_package(LibXml2 REQUIRED)
find_package(Iconv REQUIRED)

# ============================================================
# Sub-dependencies (each is a git submodule under deps/)
#
# Disable every sub-dependency's own test build so only the
# unidict test suite runs under ctest. Each dep pulls in its own
# nested sub-dependencies (e.g. lsd/stardict pull dictzip, czim
# pulls uobject) via source, so we do not add those here.
# ============================================================
set(UOBJECT_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(CZIM_BUILD_TESTS      OFF CACHE BOOL "" FORCE)
set(DICTZIP_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
set(UDX_BUILD_TESTS       OFF CACHE BOOL "" FORCE)
set(BGL_BUILD_TESTS       OFF CACHE BOOL "" FORCE)
set(CMDX_BUILD_TESTS      OFF CACHE BOOL "" FORCE)
set(EB_BUILD_TESTS        OFF CACHE BOOL "" FORCE)
set(LDX_BUILD_TESTS       OFF CACHE BOOL "" FORCE)
set(LSD_BUILD_TESTS       OFF CACHE BOOL "" FORCE)
set(STARDICT_BUILD_TESTS  OFF CACHE BOOL "" FORCE)
set(UDX_BUILD_EXAMPLES    OFF CACHE BOOL "" FORCE)

add_subdirectory(deps/uobject)
add_subdirectory(deps/dictzip)
add_subdirectory(deps/udx)
add_subdirectory(deps/czim)
add_subdirectory(deps/bgl)
add_subdirectory(deps/cmdx)
add_subdirectory(deps/ebcore)
add_subdirectory(deps/ldx)
add_subdirectory(deps/lsd)
add_subdirectory(deps/stardict)

# ============================================================
# Main library
# ============================================================
file(GLOB UNIDICT_CORE_SOURCES   ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
file(GLOB UNIDICT_FORMAT_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/formats/*.c)
file(GLOB UNIDICT_UTIL_SOURCES   ${CMAKE_CURRENT_SOURCE_DIR}/src/utils/*.c)

add_library(unidict
    ${UNIDICT_CORE_SOURCES}
    ${UNIDICT_FORMAT_SOURCES}
    ${UNIDICT_UTIL_SOURCES}
)

target_include_directories(unidict
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/formats
        ${CMAKE_CURRENT_SOURCE_DIR}/src/utils
)

# Link every sub-dependency. Their transitive (nested) deps and
# system libs come along automatically via the dependency graph.
target_link_libraries(unidict
    PUBLIC
        bgl
        cmdx
        czim
        eb
        ldx
        lsd
        stardict
        udx
        uobject
        ZLIB::ZLIB
        LibXml2::LibXml2
        Iconv::Iconv
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(unidict PUBLIC m)
endif()

if(MSVC)
    # unidict sources pull in <stdatomic.h> via uobject.h (_Atomic refcount),
    # and so does every format backend. MSVC's C11 atomics are still
    # experimental and must be explicitly enabled — same as uobject/czim/cmdx.
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /std:c11 /experimental:c11atomics")
    target_compile_definitions(unidict PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
    target_compile_options(unidict PRIVATE /W4)
else()
    target_compile_options(unidict PRIVATE -Wall -Wextra -pedantic)
endif()

# ============================================================
# Tests
# ============================================================
if(UNIDICT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ============================================================
# Install
# ============================================================
include(GNUInstallDirs)

install(TARGETS unidict
    EXPORT unidictTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/unidict.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/unidict)

install(EXPORT unidictTargets
    FILE unidictTargets.cmake
    NAMESPACE unidict::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/unidict
)
