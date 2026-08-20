macro(print_all_variables)
    message(STATUS "print_all_variables------------------------------------------{")
    get_cmake_property(_variableNames VARIABLES)
    foreach(_variableName ${_variableNames})
        message(STATUS "${_variableName}=${${_variableName}}")
    endforeach()
    message(STATUS "print_all_variables------------------------------------------}")
endmacro()


cmake_minimum_required(VERSION 3.16)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/helpers")

set(CMAKE_COLOR_MAKEFILE ON)
set(CMAKE_VERBOSE_MAKEFILE ON)
set(CMAKE_COLOR_DIAGNOSTICS ON)


# ---------------------------------------------------------------------------
# C++ standard
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


# ---------------------------------------------------------------------------
# Project
# ---------------------------------------------------------------------------

project(magewell2ts VERSION 3.0)


# ---------------------------------------------------------------------------
# Compiler checks
# ---------------------------------------------------------------------------

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
   CMAKE_CXX_COMPILER_VERSION VERSION_LESS "13.0")

    message(STATUS "GCC < 13 detected. Falling back to external libfmt.")

    find_package(fmt REQUIRED)

    # Since the executable isn't declared yet, track the fallback status.
    set(USE_FMT_FALLBACK TRUE)

endif()


# ---------------------------------------------------------------------------
# Build configuration
# ---------------------------------------------------------------------------

if(NOT CMAKE_BUILD_TYPE)

    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Choose the type of build, options are: None Debug Release RelWithDebInfo MinSizeRel ASan."
        FORCE)

    message(STATUS
        "No CMAKE_BUILD_TYPE specified. "
        "Defaulting to optimized 'Release' mode.")

endif()


# ---------------------------------------------------------------------------
# AddressSanitizer capability check
#
# This checks that the compiler/linker can actually build an ASan program.
# The result is only checked when configuring an ASan build.
# ---------------------------------------------------------------------------

if(CMAKE_BUILD_TYPE STREQUAL "ASan")

    include(CheckCXXSourceCompiles)

    set(_ASAN_OLD_REQUIRED_FLAGS
        "${CMAKE_REQUIRED_FLAGS}")

    set(_ASAN_OLD_REQUIRED_LINK_OPTIONS
        "${CMAKE_REQUIRED_LINK_OPTIONS}")

    set(CMAKE_REQUIRED_FLAGS
        "-fsanitize=address -fno-omit-frame-pointer")

    set(CMAKE_REQUIRED_LINK_OPTIONS
        "-fsanitize=address")

    check_cxx_source_compiles(
        "
        int main()
        {
            return 0;
        }
        "
        HAS_ASAN
    )

    set(CMAKE_REQUIRED_FLAGS
        "${_ASAN_OLD_REQUIRED_FLAGS}")

    set(CMAKE_REQUIRED_LINK_OPTIONS
        "${_ASAN_OLD_REQUIRED_LINK_OPTIONS}")


    if(NOT HAS_ASAN)

        message(FATAL_ERROR
            "=============================================================\n"
            " ERROR: AddressSanitizer could not be compiled and linked.\n"
            "\n"
            " Try installing the AddressSanitizer runtime:\n"
            "     sudo dnf install libasan\n"
            "=============================================================\n"
        )

    endif()

    message(STATUS
        "AddressSanitizer compiler/linker support detected.")

endif()


# ---------------------------------------------------------------------------
# Installation directories
# ---------------------------------------------------------------------------

include(GNUInstallDirs)

set(INSTALL_BIN_DIR ${CMAKE_INSTALL_BINDIR})


# ---------------------------------------------------------------------------
# Git version
# ---------------------------------------------------------------------------

include(GetGitVersion.cmake)


configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h"
)


# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------

add_executable(magewell2ts
    AudioStream.cpp
    PCMStream.cpp
    BitStream.cpp
    VideoStream.cpp
    OutputTS.cpp
    EAC3Parser.cpp
    IEC61937Parser.cpp
    Magewell.cpp
    magewell2ts.cpp
)


# ---------------------------------------------------------------------------
# AddressSanitizer compiler/linker options
#
# These are deliberately applied directly to the target instead of relying
# on CMAKE_CXX_FLAGS_<CONFIG>, because ASan is a custom single-config
# build type.
# ---------------------------------------------------------------------------

if(CMAKE_BUILD_TYPE STREQUAL "ASan")

    message(STATUS "Configuring AddressSanitizer build.")

    target_compile_options(magewell2ts PRIVATE
        -fsanitize=address
        -fno-omit-frame-pointer
        -g
        -O1
    )

    target_link_options(magewell2ts PRIVATE
        -fsanitize=address
    )

endif()


# ---------------------------------------------------------------------------
# Threading
# ---------------------------------------------------------------------------

set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)


# ---------------------------------------------------------------------------
# fmt fallback
# ---------------------------------------------------------------------------

if(USE_FMT_FALLBACK)

    target_link_libraries(magewell2ts PRIVATE
        fmt::fmt
    )

    target_compile_definitions(magewell2ts PRIVATE
        USE_LIBFMT_FALLBACK
    )

endif()


# ---------------------------------------------------------------------------
# V4L2
# ---------------------------------------------------------------------------

find_package(V4L2)

if(NOT V4L2_FOUND)

    message(STATUS "----------------------------------------------------------------")

    message(FATAL_ERROR
        "libv4l required. "
        "Maybe: `sudo dnf install -y libv4l-devel`")

    message(STATUS "----------------------------------------------------------------")

endif()


# ---------------------------------------------------------------------------
# Magewell
# ---------------------------------------------------------------------------

find_package(Magewell REQUIRED)

if(NOT Magewell_FOUND)

    message(STATUS "----------------------------------------------------------------")

    message(FATAL_ERROR
        "Magewell SDK not found. "
        "Expecting to find it in the parent directory of this src tree.")

    message(STATUS "----------------------------------------------------------------")

endif()


# ---------------------------------------------------------------------------
# Threads
# ---------------------------------------------------------------------------

find_package(Threads REQUIRED)


# ---------------------------------------------------------------------------
# ALSA
# ---------------------------------------------------------------------------

find_package(ALSA REQUIRED)

if(NOT ALSA_FOUND)

    message(STATUS "----------------------------------------------------------------")

    message(FATAL_ERROR
        "ALSA development headers not found. "
        "Maybe try: `sudo dnf install alsa-lib-devel`")

    message(STATUS "----------------------------------------------------------------")

endif()


# ---------------------------------------------------------------------------
# udev
# ---------------------------------------------------------------------------

find_package(udev REQUIRED)

if(NOT udev_FOUND)

    message(STATUS "----------------------------------------------------------------")

    message(FATAL_ERROR
        "udev development headers not found. "
        "Maybe try: `sudo apt-get install libudev-dev`")

    message(STATUS "----------------------------------------------------------------")

endif()


# ---------------------------------------------------------------------------
# spdlog
# ---------------------------------------------------------------------------

find_package(spdlog QUIET)

if(NOT spdlog_FOUND)

    message(STATUS "----------------------------------------------------------------")

    message(WARNING
        "spdlog library not found. "
        "spdlog is required for logging functionality.")

    message(STATUS "To install spdlog, perhaps try:")
    message(STATUS "  sudo dnf install spdlog-devel")
    message(STATUS "  sudo apt install libspdlog-dev")

    message(FATAL_ERROR
        "spdlog library not found. "
        "Please install it before continuing.")

    message(STATUS "----------------------------------------------------------------")

endif()


# ---------------------------------------------------------------------------
# FFmpeg
# ---------------------------------------------------------------------------

set(ENV{PKG_CONFIG_PATH}
    "$ENV{PKG_CONFIG_PATH}:/opt/ffmpeg/lib/pkgconfig")


find_package(PkgConfig)

if(NOT PkgConfig_FOUND)

    message(STATUS "----------------------------------------------------------------")

    message(FATAL_ERROR
        "pkg_config not found. "
        "Maybe try `sudo dnf install pkgconfig` or "
        "`sudo apt-get install pkg-config`")

    message(STATUS "----------------------------------------------------------------")

endif()


pkg_check_modules(LIBAV IMPORTED_TARGET
    libavdevice>=57
    libavformat>=57
    libavcodec>=57
    libavutil>=57
    libswresample
    libswscale
    libavfilter
)


if(NOT LIBAV_FOUND)

    message(STATUS "----------------------------------------------------------------")

    message(FATAL_ERROR
        "ffmpeg development headers not found.\n"
        "Maybe try `sudo dnf install ffmpeg-devel` or "
        "`sudo apt-get install ffmpeg-dev`")

    message(STATUS "----------------------------------------------------------------")

endif()


# ---------------------------------------------------------------------------
# Intel oneVPL
# ---------------------------------------------------------------------------

pkg_check_modules(VPL IMPORTED_TARGET vpl)


if(NOT VPL_FOUND)

    message(STATUS "----------------------------------------------------------------")

    message(STATUS
        "libvpl not found, Intel oneVPL will not be enabled.")

    message(STATUS
        "If desired, maybe try: `sudo dnf install -y oneVPL-devel`.")

    message(STATUS "----------------------------------------------------------------")

else()

    target_link_libraries(magewell2ts PRIVATE
        PkgConfig::VPL
    )

    target_include_directories(magewell2ts PRIVATE
        /usr/include/vpl
    )

    add_compile_definitions(
        VPL_VERSION=${VPL_VERSION}
    )


    # -----------------------------------------------------------------------
    # Check for extra QSV structures
    # -----------------------------------------------------------------------

    find_path(INTEL_MFX_INCLUDE_DIR
        NAMES mfx/mfxvideo.h
        PATHS
            /usr/include
            /usr/local/include
    )


    if(INTEL_MFX_INCLUDE_DIR)

        message(STATUS
            "Intel MediaSDK headers detected. "
            "Enabling advanced QSV frame type management.")

        target_include_directories(magewell2ts PRIVATE
            ${INTEL_MFX_INCLUDE_DIR}
        )

        target_compile_definitions(magewell2ts PRIVATE
            HAS_MAGEWELL_QSV_SUPPORT
        )

    else()

        message(STATUS "----------------------------------------------------------------")

        message(STATUS
            "NOTICE: 'mfx/mfxvideo.h' not found. "
            "QSV framing features are disabled.")

        message(STATUS
            "If you are on an Intel system, you can optionally install:")

        message(STATUS
            "  sudo dnf install -y intel-mediasdk-devel")

        message(STATUS "----------------------------------------------------------------")

    endif()

endif()


# ---------------------------------------------------------------------------
# Include directories
# ---------------------------------------------------------------------------

target_include_directories(magewell2ts PUBLIC
    "${PROJECT_BINARY_DIR}"
    "${PROJECT_BINARY_DIR}/generated"
    "${Magewell_INCLUDE_DIR}"
)


# ---------------------------------------------------------------------------
# Link libraries
# ---------------------------------------------------------------------------

target_link_libraries(magewell2ts PRIVATE
    Magewell::Magewell
    ${V4L2_LIBRARIES}
    ${UDEV_LIBRARY}
    ALSA::ALSA
    Threads::Threads
    PkgConfig::LIBAV
    spdlog::spdlog
)


# ---------------------------------------------------------------------------
# Compiler options
# ---------------------------------------------------------------------------

target_compile_options(magewell2ts PRIVATE

    -Wall
    -Wreturn-type

    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:Release>:-march=native>
    $<$<CONFIG:Release>:-flto=auto>

)


# ---------------------------------------------------------------------------
# Compile definitions
# ---------------------------------------------------------------------------

target_compile_definitions(magewell2ts PRIVATE

    $<$<OR:$<CONFIG:Debug>,$<CONFIG:ASan>>:DEBUG_MODE>
    $<$<OR:$<CONFIG:Debug>,$<CONFIG:ASan>>:ENABLE_VERBOSE_ERRORS>
    $<$<OR:$<CONFIG:Debug>,$<CONFIG:ASan>>:LOG_ELAPSED=1>

)


# ---------------------------------------------------------------------------
# Link options
# ---------------------------------------------------------------------------

target_link_options(magewell2ts PRIVATE

    $<$<CONFIG:Release>:-flto=auto>

)


# ---------------------------------------------------------------------------
# Convenience target: Debug
# ---------------------------------------------------------------------------

add_custom_target(debug

    COMMAND ${CMAKE_COMMAND}
        -S ${CMAKE_SOURCE_DIR}
        -B ${CMAKE_BINARY_DIR}
        -DCMAKE_BUILD_TYPE=Debug

    COMMAND ${CMAKE_COMMAND}
        --build ${CMAKE_BINARY_DIR}
        --target all

    COMMENT "Switch CMAKE_BUILD_TYPE to Debug"

    VERBATIM

)


# ---------------------------------------------------------------------------
# Convenience target: Release
# ---------------------------------------------------------------------------

add_custom_target(release

    COMMAND ${CMAKE_COMMAND}
        -S ${CMAKE_SOURCE_DIR}
        -B ${CMAKE_BINARY_DIR}
        -DCMAKE_BUILD_TYPE=Release

    COMMAND ${CMAKE_COMMAND}
        --build ${CMAKE_BINARY_DIR}
        --target all

    COMMENT "Switch CMAKE_BUILD_TYPE to Release"

    VERBATIM

)


# ---------------------------------------------------------------------------
# Convenience target: AddressSanitizer
# ---------------------------------------------------------------------------

add_custom_target(asan

    COMMAND ${CMAKE_COMMAND}
        -S ${CMAKE_SOURCE_DIR}
        -B ${CMAKE_BINARY_DIR}
        -DCMAKE_BUILD_TYPE=ASan

    COMMAND ${CMAKE_COMMAND}
        --build ${CMAKE_BINARY_DIR}
        --target all

    COMMENT "Switch CMAKE_BUILD_TYPE to ASan (AddressSanitizer)"

    VERBATIM

)


# ---------------------------------------------------------------------------
# Installation
# ---------------------------------------------------------------------------

install(
    TARGETS magewell2ts
    DESTINATION ${INSTALL_BIN_DIR}
)


install(
    PROGRAMS m2tsweb
    DESTINATION ${INSTALL_BIN_DIR}
)


# ---------------------------------------------------------------------------
# Set capabilities
# ---------------------------------------------------------------------------

install(CODE "
    execute_process(
        COMMAND sudo setcap cap_sys_nice+ep
                \${CMAKE_INSTALL_PREFIX}/${INSTALL_BIN_DIR}/magewell2ts
        RESULT_VARIABLE SETCAP_RESULT
    )

    if(NOT SETCAP_RESULT EQUAL 0)
        message(WARNING
            \"Failed to apply setcap to magewell2ts. \"
            \"Real-time scheduling may fail.\")
    endif()
")
