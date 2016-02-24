if (DEFINED ENV{MFX_HOME})
    MESSAGE( STATUS "MFX_HOME=$ENV{MFX_HOME}")
    set(MFX_HOME $ENV{MFX_HOME})
elseif (EXISTS "/opt/intel/mediasdk")
    MESSAGE( STATUS "MediaSDK distribution found in /opt/intel/mediasdk")
    set(MFX_HOME "/opt/intel/mediasdk")
else()
    MESSAGE( FATAL_ERROR "No MediaSDK distribution is found.")
endif()

set(MFX_INCLUDES "${MFX_HOME}/include")
set(MFX_LIBRARY  "${MFX_HOME}/lib/lin_x64/libmfx.a")
MESSAGE( STATUS "MediaSDK include directory: ${MFX_INCLUDES}")
MESSAGE( STATUS "MediaSDK library directory: ${MFX_LIBRARI}")

#Add MFX library
add_library(libmfx STATIC IMPORTED)
set_property(TARGET libmfx PROPERTY IMPORTED_LOCATION ${MFX_LIBRARY})
add_definitions(-DHAVE_CONFIG_H)
include_directories( ${MFX_INCLUDES} )
