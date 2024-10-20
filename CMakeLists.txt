macro(print_all_variables)
    message(STATUS "print_all_variables------------------------------------------{")
    get_cmake_property(_variableNames VARIABLES)
    foreach (_variableName ${_variableNames})
        message(STATUS "${_variableName}=${${_variableName}}")
    endforeach()
    message(STATUS "print_all_variables------------------------------------------}")
endmacro()

cmake_minimum_required(VERSION 3.10)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/helpers")

SET(CMAKE_COLOR_MAKEFILE ON)
SET(CMAKE_VERBOSE_MAKEFILE ON)
SET(CMAKE_COLOR_DIAGNOSTICS ON)

# specify the C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED True)
add_compile_options(-Wall -Wreturn-type)

# set the project name
project(magewellpro2ts VERSION 0.1)

configure_file(version.h.in version.h)

# add the executable
add_executable(magewellpro2ts magewellpro2ts.cpp AudioIO.cpp OutputTS.cpp)

# set_target_properties(magewellpro2ts PROPERTIES
#   LINK_SEARCH_START_STATIC ON
#   LINK_SEARCH_END_STATIC ON
# )

set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)

find_package(V4L2)
if(NOT V4L2_FOUND)
  message(FATAL_ERROR "libv4l required. e.g. `sudo dnf install -y libv4l-devel`")
endif()

target_include_directories(magewellpro2ts PUBLIC
                           "${PROJECT_BINARY_DIR}"
			   "${Magewell_INCLUDE_DIR}"
                           )

find_package(Magewell REQUIRED)
if(NOT Magewell_FOUND)
  message(FATAL_ERROR "Magewell SDK not found. Expecting to find it in the parent directory of this src tree.")
endif()

find_package(Threads REQUIRED)

find_package(ALSA REQUIRED)
if(NOT ALSA_FOUND)
  message(FATAL_ERROR "ALSA development headers not found. Maybe try: `sudo dnf install alsa-lib-devel`")
endif()

find_package(udev REQUIRED)
if(NOT udev_FOUND)
  message(FATAL_ERROR "udev development headers not found. Maybe try: `sudo apt-get install libudev-dev`")
endif()

#find_package(FFmpeg COMPONENTS AVCODEC AVFORMAT AVUTIL AVDEVICE REQUIRED)

set(ENV{PKG_CONFIG_PATH} "$ENV{PKG_CONFIG_PATH}:/opt/ffmpeg/lib/pkgconfig")

find_package(PkgConfig)
if(NOT PkgConfig_FOUND)
  message(FATAL_ERROR "pkg_config not found. Maybe try `sudo dnf install pkgconfig` or `sudo apt-get install pkg-config`")
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
  message(FATAL_ERROR "ffmpeg development headers not found.\nMaybe try `sudo dnf install ffmpeg-devel` or `sudo apt-get install ffmpeg-dev`")
endif()

string(COMPARE LESS ${LIBAV_libavdevice_VERSION} "60" result)
if(result)
  message(WARNING "libavdevice version >= 60 required for EAC3/SPDIF support. Found " ${LIBAV_libavdevice_VERSION})
endif()

pkg_check_modules(VPL IMPORTED_TARGET vpl)

if(NOT VPL_FOUND)
  message("libvpl not found, Intel oneVPL will not be enabled.\nMaybe try `sudo dnf install -y oneVPL-devel`. See: https://github.com/oneapi-src/oneVPL")
else()
  message(NOTICE "Note: To use oneVPL, ffmpeg needs to be compiled with '--enable-libvpl'")
  target_link_libraries(${TARGET} PkgConfig::VPL)
  add_compile_definitions(VPL_VERSION=${VPL_VERSION})
endif()

# print_all_variables()

set(CMAKE_VERBOSE_MAKEFILE on)

target_link_libraries(magewellpro2ts PRIVATE
  Magewell::Magewell
  ${V4L2_LIBRARIES}
  ${UDEV_LIBRARY}
  ALSA::ALSA
  Threads::Threads
  PkgConfig::LIBAV
)

install(TARGETS magewellpro2ts DESTINATION /usr/local/bin)

ADD_CUSTOM_TARGET(debug
  COMMAND ${CMAKE_COMMAND} -DCMAKE_BUILD_TYPE=Debug ${CMAKE_SOURCE_DIR}
  COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target all
  COMMENT "Switch CMAKE_BUILD_TYPE to Debug"
  )

ADD_CUSTOM_TARGET(release
  COMMAND ${CMAKE_COMMAND} -DCMAKE_BUILD_TYPE=Release ${CMAKE_SOURCE_DIR}
  COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target all
  COMMENT "Switch CMAKE_BUILD_TYPE to Release"
  )
