cmake_minimum_required(VERSION 3.16)
project(obs-proxy VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Qt6 (for the in-app Controls-dock toggle button). Point at the OBS Qt deps:
#   -DOBS_QT_PREFIX="C:/.../obs-studio/.deps/obs-deps-qt6-<date>-x64"
set(OBS_QT_PREFIX "" CACHE PATH "Path to the Qt6 prefix (obs-deps-qt6)")
if(OBS_QT_PREFIX)
    list(APPEND CMAKE_PREFIX_PATH "${OBS_QT_PREFIX}")
endif()
find_package(Qt6 COMPONENTS Core Widgets REQUIRED)

# Preferred: consume an installed/exported libobs package, e.g.:
#   -DCMAKE_PREFIX_PATH="C:/obs-studio/build/rundir/RelWithDebInfo"
find_package(libobs QUIET)

# Fallback: build directly against a raw OBS source + build tree (useful for
# local test builds where libobs isn't exported as a package). Pass:
#   -DOBS_SRC_DIR="C:/path/to/obs-studio"
#   -DOBS_BUILD_DIR="C:/path/to/obs-studio/build_x64"
if(NOT TARGET OBS::libobs)
    if(DEFINED OBS_SRC_DIR AND DEFINED OBS_BUILD_DIR)
        message(STATUS "libobs package not found; linking against OBS tree at ${OBS_BUILD_DIR}")
        add_library(OBS::libobs SHARED IMPORTED)
        set_target_properties(OBS::libobs PROPERTIES
            IMPORTED_IMPLIB "${OBS_BUILD_DIR}/libobs/RelWithDebInfo/obs.lib"
            IMPORTED_LOCATION "${OBS_BUILD_DIR}/libobs/RelWithDebInfo/obs.dll"
            INTERFACE_INCLUDE_DIRECTORIES "${OBS_SRC_DIR}/libobs;${OBS_BUILD_DIR}/config"
            INTERFACE_COMPILE_DEFINITIONS "HAVE_OBSCONFIG_H")
    else()
        message(FATAL_ERROR
            "libobs not found. Either provide an exported package via "
            "CMAKE_PREFIX_PATH, or set -DOBS_SRC_DIR and -DOBS_BUILD_DIR.")
    endif()
endif()

# obs-frontend-api (for the Tools-menu settings dialog).
if(NOT TARGET OBS::obs-frontend-api)
    if(DEFINED OBS_SRC_DIR AND DEFINED OBS_BUILD_DIR)
        add_library(OBS::obs-frontend-api SHARED IMPORTED)
        set_target_properties(OBS::obs-frontend-api PROPERTIES
            IMPORTED_IMPLIB "${OBS_BUILD_DIR}/frontend/api/RelWithDebInfo/obs-frontend-api.lib"
            IMPORTED_LOCATION "${OBS_BUILD_DIR}/frontend/api/RelWithDebInfo/obs-frontend-api.dll"
            INTERFACE_INCLUDE_DIRECTORIES "${OBS_SRC_DIR}/frontend/api")
    else()
        find_package(obs-frontend-api REQUIRED)
    endif()
endif()

include(FetchContent)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(minhook)

add_library(obs-proxy MODULE
    src/plugin-main.c
    src/socks5.c
    src/hooks.c
    src/proxylist.c
    src/settings-ui.c
    src/dock.cpp
    src/plugin.rc
)

target_include_directories(obs-proxy PRIVATE src)

target_link_libraries(obs-proxy
    PRIVATE OBS::libobs OBS::obs-frontend-api
            Qt6::Core Qt6::Widgets
            minhook ws2_32 winhttp user32
)

set_target_properties(obs-proxy PROPERTIES PREFIX "" SUFFIX ".dll")

# Emit the plugin DLL into a single, config-independent folder so the installer
# resource script can reference it by a stable path.
set(OBS_PROXY_OUT "${CMAKE_BINARY_DIR}/out")
# A MODULE library's DLL is a LIBRARY artifact on Windows, so set that too.
set_target_properties(obs-proxy PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY                "${OBS_PROXY_OUT}"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG          "${OBS_PROXY_OUT}"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE        "${OBS_PROXY_OUT}"
    LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO "${OBS_PROXY_OUT}"
    LIBRARY_OUTPUT_DIRECTORY_MINSIZEREL     "${OBS_PROXY_OUT}")

# ---------------------------------------------------------------------------
# Self-contained installer executable (embeds the plugin DLL + locale).
# ---------------------------------------------------------------------------
option(BUILD_INSTALLER "Build the self-contained installer .exe" ON)
if(BUILD_INSTALLER AND WIN32)
    set(PLUGIN_DLL_PATH "${OBS_PROXY_OUT}/obs-proxy.dll")
    set(LOCALE_INI_PATH "${CMAKE_SOURCE_DIR}/data/locale/en-US.ini")
    set(MANIFEST_PATH   "${CMAKE_SOURCE_DIR}/installer/installer.manifest")
    configure_file("${CMAKE_SOURCE_DIR}/installer/installer.rc.in"
                   "${CMAKE_BINARY_DIR}/installer.rc" @ONLY)

    # Re-run the resource compiler whenever the embedded payloads change,
    # so the installer never ships a stale plugin DLL. OBJECT_DEPENDS alone is
    # unreliable for generated .rc files, so also bump the .rc mtime right after
    # the plugin DLL is (re)built.
    set_source_files_properties("${CMAKE_BINARY_DIR}/installer.rc" PROPERTIES
        OBJECT_DEPENDS "${PLUGIN_DLL_PATH};${LOCALE_INI_PATH}")
    add_custom_command(TARGET obs-proxy POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E touch_nocreate "${CMAKE_BINARY_DIR}/installer.rc"
        COMMENT "Marking installer resources for re-embed")

    add_executable(obs-proxy-installer
        installer/installer.c
        "${CMAKE_BINARY_DIR}/installer.rc")
    target_include_directories(obs-proxy-installer PRIVATE "${CMAKE_SOURCE_DIR}/installer")
    target_compile_definitions(obs-proxy-installer PRIVATE UNICODE _UNICODE)
    target_link_libraries(obs-proxy-installer PRIVATE shell32 shlwapi advapi32)
    # Our own RT_MANIFEST is embedded via the .rc; disable the linker default.
    target_link_options(obs-proxy-installer PRIVATE /MANIFEST:NO)
    add_dependencies(obs-proxy-installer obs-proxy)
    set_target_properties(obs-proxy-installer PROPERTIES
        OUTPUT_NAME "obs-proxy-installer"
        RUNTIME_OUTPUT_DIRECTORY                "${OBS_PROXY_OUT}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG          "${OBS_PROXY_OUT}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE        "${OBS_PROXY_OUT}"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${OBS_PROXY_OUT}"
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL     "${OBS_PROXY_OUT}")
endif()
