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

# set the project name
project(magewellpro2ts VERSION 0.1)

configure_file(version.h.in version.h)

# add the executable
add_executable(magewellpro2ts magewellpro2ts.cpp OutputTS.cpp)

# set_target_properties(magewellpro2ts PROPERTIES
#   LINK_SEARCH_START_STATIC ON
#   LINK_SEARCH_END_STATIC ON
# )

set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Magewell REQUIRED)
find_package(Threads REQUIRED)
find_package(ALSA REQUIRED)
find_package(V4L2 REQUIRED)
find_package(udev REQUIRED)
#find_package(FFmpeg COMPONENTS AVCODEC AVFORMAT AVUTIL AVDEVICE REQUIRED)



set(ENV{PKG_CONFIG_PATH} "$ENV{PKG_CONFIG_PATH}:/opt/ffmpeg/lib/pkgconfig")
find_package(PkgConfig)


pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET
    libavdevice>=57
    libavformat>=57
    libavcodec>=57
    libavutil>=57
    libswresample
    libswscale
    libavfilter
    )

string(COMPARE LESS ${LIBAV_libavdevice_VERSION} "60" result)
if(result)
  message(WARNING "libavdevice version >= 60 required for EAC3/SPDIF support. Found " ${LIBAV_libavdevice_VERSION})
endif()


pkg_check_modules(VPL IMPORTED_TARGET
  vpl
  )

add_compile_definitions(VPL_VERSION=${VPL_VERSION})

message(STATUS "VPL: " ${VPL_VERSION})

# print_all_variables()

target_include_directories(magewellpro2ts PUBLIC
                           "${PROJECT_BINARY_DIR}"
			   "${Magewell_INCLUDE_DIR}"
                           )

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
