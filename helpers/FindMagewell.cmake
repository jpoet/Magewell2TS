# - Find Magewell SDK

SET(Magewell_SEARCH_PATHS
    ../
    ../Magewell_Capture_SDK_Linux_3.3.1.1505
    ../Magewell_Capture_SDK_Linux_3.3.1.1313
)

FIND_PATH(Magewell_INCLUDE_DIR MWFOURCC.h
	HINTS
	$ENV{MagewellAPI}
	PATH_SUFFIXES Include
	PATHS ${Magewell_SEARCH_PATHS}
)

# message(STATUS "Magewell header: ${Magewell_INCLUDE_DIR}")

FIND_LIBRARY(Magewell_LIB
	NAMES libMWCapture.a
	HINTS
	$ENV{MagewellAPI}
	PATH_SUFFIXES Lib/x64
	PATHS ${Magewell_SEARCH_PATHS}
)

# make the variables advanced to not shown in CMake GUI
mark_as_advanced(Magewell_INCLUDE_DIR Magewell_LIB)

# Provide standard CMake package variables
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Magewell REQUIRED_VARS
  Magewell_INCLUDE_DIR
  Magewell_LIB
  )

if(Magewell_FOUND AND NOT TARGET Magewell::Magewell)
  add_library(Magewell::Magewell SHARED IMPORTED)
  set_target_properties(Magewell::Magewell PROPERTIES
    IMPORTED_LINK_INTERFACE_LANGUAGES "CXX"
    IMPORTED_LOCATION "${Magewell_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES
      "${Magewell_INCLUDE_DIR}"
      )
    
endif()
