macro(print_all_variables)
    message(STATUS "print_all_variables------------------------------------------{")
    get_cmake_property(_variableNames VARIABLES)
    foreach (_variableName ${_variableNames})
        message(STATUS "${_variableName}=${${_variableName}}")
    endforeach()
    message(STATUS "print_all_variables------------------------------------------}")
endmacro()

cmake_minimum_required(VERSION 3.16)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/helpers")

SET(CMAKE_COLOR_MAKEFILE ON)
SET(CMAKE_VERBOSE_MAKEFILE ON)
SET(CMAKE_COLOR_DIAGNOSTICS ON)

# specify the C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# set the project name
project(magewell2ts VERSION 3.0)

# CHECK COMPILER VARIABLES
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS "13.0")
    message(STATUS "GCC < 13 detected. Falling back to external libfmt.")

    # Locate the system libfmt installed via DNF
    find_package(fmt REQUIRED)

    # Since the executable isn't declared yet, track the fallback status in a variable
    set(USE_FMT_FALLBACK TRUE)
endif()

# If no configuration type is explicitly defined, force the cache
# state to Release
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Choose the type of build, options are: None Debug Release RelWithDebInfo MinSizeRel."
        FORCE)
    message(STATUS "No CMAKE_BUILD_TYPE specified. Defaulting to optimized 'Release' mode.")
endif()

include(GNUInstallDirs) # Results in /usr/local being the default.
set(INSTALL_BIN_DIR ${CMAKE_INSTALL_BINDIR})
include(GetGitVersion.cmake)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h"
)

# add the executable
add_executable(magewell2ts
               AudioStream.cpp PCMStream.cpp BitStream.cpp
               VideoStream.cpp OutputTS.cpp
	       EAC3Parser.cpp IEC61937Parser.cpp
               Magewell.cpp magewell2ts.cpp)

set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)

if(USE_FMT_FALLBACK)
    target_link_libraries(magewell2ts PRIVATE fmt::fmt)
    target_compile_definitions(magewell2ts PRIVATE USE_LIBFMT_FALLBACK)
endif()

find_package(V4L2)
if(NOT V4L2_FOUND)
  message(STATUS "----------------------------------------------------------------")
  message(FATAL_ERROR "libv4l required. Maybe: `sudo dnf install -y libv4l-devel`")
  message(STATUS "----------------------------------------------------------------")
endif()

target_include_directories(magewell2ts PUBLIC
  "${PROJECT_BINARY_DIR}"
  "${PROJECT_BINARY_DIR}/generated"
  "${Magewell_INCLUDE_DIR}"
)

find_package(Magewell REQUIRED)
if(NOT Magewell_FOUND)
  message(STATUS "----------------------------------------------------------------")
  message(FATAL_ERROR "Magewell SDK not found. Expecting to find it in the parent directory of this src tree.")
  message(STATUS "----------------------------------------------------------------")
endif()

find_package(Threads REQUIRED)

find_package(ALSA REQUIRED)
if(NOT ALSA_FOUND)
  message(STATUS "----------------------------------------------------------------")
  message(FATAL_ERROR "ALSA development headers not found. Maybe try: `sudo dnf install alsa-lib-devel`")
  message(STATUS "----------------------------------------------------------------")
endif()

find_package(udev REQUIRED)
if(NOT udev_FOUND)
  message(STATUS "----------------------------------------------------------------")
  message(FATAL_ERROR "udev development headers not found. Maybe try: `sudo apt-get install libudev-dev`")
  message(STATUS "----------------------------------------------------------------")
endif()

# Find spdlog
find_package(spdlog QUIET)
if(NOT spdlog_FOUND)
  message(STATUS "----------------------------------------------------------------")
  message(WARNING "spdlog library not found. spdlog is required for logging functionality.")
  message(STATUS "To install spdlog, perhaps try:")
  message(STATUS "  sudo dnf install spdlog-devel")
  message(STATUS "  sudo apt install libspdlog-dev")
  message(FATAL_ERROR "spdlog library not found. Please install it before continuing.")
  message(STATUS "----------------------------------------------------------------")
endif()

set(ENV{PKG_CONFIG_PATH} "$ENV{PKG_CONFIG_PATH}:/opt/ffmpeg/lib/pkgconfig")

find_package(PkgConfig)
if(NOT PkgConfig_FOUND)
  message(STATUS "----------------------------------------------------------------")
  message(FATAL_ERROR "pkg_config not found. Maybe try `sudo dnf install pkgconfig` or `sudo apt-get install pkg-config`")
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
  message(FATAL_ERROR "ffmpeg development headers not found.\n")
  message(STATUS "Maybe try `sudo dnf install ffmpeg-devel` or `sudo apt-get install ffmpeg-dev`")
  message(STATUS "----------------------------------------------------------------")
endif()

pkg_check_modules(VPL IMPORTED_TARGET vpl)

if(NOT VPL_FOUND)
  message(STATUS "----------------------------------------------------------------")
  message(STATUS "libvpl not found, Intel oneVPL will not be enabled.")
  message(STATUS "If desired, Maybe try `sudo dnf install -y oneVPL-devel`.")
  message(STATUS "----------------------------------------------------------------")
else()
  target_link_libraries(magewell2ts PRIVATE PkgConfig::VPL)
  target_include_directories(magewell2ts PRIVATE /usr/include/vpl)
  add_compile_definitions(VPL_VERSION=${VPL_VERSION})

  # --- CHECK FOR EXTRA QSV STRUCTURES (SEMI-OPTIONAL) ---
  find_path(INTEL_MFX_INCLUDE_DIR
      NAMES mfx/mfxvideo.h
      PATHS /usr/include /usr/local/include
  )

  if(INTEL_MFX_INCLUDE_DIR)
      message(STATUS "Intel MediaSDK headers detected. Enabling advanced QSV frame type management.")
      # Include both the standard VPL path and the legacy MediaSDK path for FFmpeg inclusions
      target_include_directories(magewell2ts PRIVATE ${INTEL_MFX_INCLUDE_DIR})

      # Inject a new global compile flag into your C++ code
      target_compile_definitions(magewell2ts PRIVATE HAS_MAGEWELL_QSV_SUPPORT)
  else()
      message(STATUS "----------------------------------------------------------------")
      message(STATUS "NOTICE: 'mfx/mfxvideo.h' not found. QSV framing features are disabled.")
      message(STATUS "If you are on an Intel system, you can optional install:")
      message(STATUS "  sudo dnf install -y intel-mediasdk-devel")
      message(STATUS "----------------------------------------------------------------")
  endif()
endif()

set(CMAKE_VERBOSE_MAKEFILE on)

target_link_libraries(magewell2ts PRIVATE
  Magewell::Magewell
  ${V4L2_LIBRARIES}
  ${UDEV_LIBRARY}
  ALSA::ALSA
  Threads::Threads
  PkgConfig::LIBAV
  spdlog::spdlog
)

install(TARGETS magewell2ts DESTINATION ${INSTALL_BIN_DIR})
install(PROGRAMS m2tsweb DESTINATION ${INSTALL_BIN_DIR})

target_compile_options(magewell2ts PRIVATE
    -Wall
    -Wreturn-type
    $<$<CONFIG:Release>:-O3>
    $<$<CONFIG:Release>:-march=native>
    $<$<CONFIG:Release>:-flto=auto>
)

target_compile_definitions(magewell2ts PRIVATE
    $<$<CONFIG:Debug>:DEBUG_MODE>
    $<$<CONFIG:Debug>:ENABLE_VERBOSE_ERRORS>
    $<$<CONFIG:Debug>:LOG_ELAPSED=1>
)

target_link_options(magewell2ts PRIVATE
    $<$<CONFIG:Release>:-flto=auto>
)

ADD_CUSTOM_TARGET(debug
  COMMAND ${CMAKE_COMMAND} -DCMAKE_BUILD_TYPE=Debug ${CMAKE_SOURCE_DIR}
  COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target all
  COMMENT "Switch CMAKE_BUILD_TYPE to Debug"
  VERBATIM
)

ADD_CUSTOM_TARGET(release
  COMMAND ${CMAKE_COMMAND} -DCMAKE_BUILD_TYPE=Release ${CMAKE_SOURCE_DIR}
  COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target all
  COMMENT "Switch CMAKE_BUILD_TYPE to Release"
  VERBATIM
)

install(CODE "
  execute_process(
    COMMAND sudo setcap cap_sys_nice+ep \${CMAKE_INSTALL_PREFIX}/${INSTALL_BIN_DIR}/magewell2ts
    RESULT_VARIABLE SETCAP_RESULT
  )
  if(NOT SETCAP_RESULT EQUAL 0)
    message(WARNING \"Failed to apply setcap to magewell2ts. Real-time scheduling may fail.\")
  endif()
")
