# - Find Magewell SDK

## HACK!!! I could not get find_path to work, so
set(Magewell_INCLUDE_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/../Include" )

find_path(Magewell_INCLUDE_DIR LibMWCapture/MWCapture.h
  DOC "The Magewell SDK include directory")
message(STATUS "Magewell header found at: ${Magewell_INCLUDE_DIR}")

## HACK!!! I could not get find_library to work, so
set(Magewell_LIB   "${CMAKE_CURRENT_SOURCE_DIR}/../Lib/x64/libMWCapture.a" )

find_library(Magewell_LIB Magewell libMWCapture
  DOC "The Magewell SDK library"
)
message(STATUS "libMagewell found at: ${Magewell_LIB}")

mark_as_advanced(Magewell_INCLUDE_DIR Magewell_LIB)

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
