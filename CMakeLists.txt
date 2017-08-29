cmake_minimum_required(VERSION "2.8.0")

project(gst-smile-plugins)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

set(GST_VERSION_MAJOR 1)
set(GST_VERSION_MINOR 0)
set(GST_MAJORMINOR ${GST_VERSION_MAJOR}.${GST_VERSION_MINOR})


# Check dependencies
include(FindPkgConfig)
include(PkgCheckVariable)
pkg_check_modules(GST REQUIRED gstreamer-${GST_MAJORMINOR})
pkg_check_modules(GST_VIDEO REQUIRED gstreamer-video-${GST_MAJORMINOR})
pkg_check_variable(GST_PLUGIN_PATH gstreamer-1.0 pluginsdir)


if(NOT GST_FOUND)
    message(FATAL_ERROR "GStreamer not found")
endif()

# Define paths for includes and libraries
# GStreamer 
include_directories(
        ${GST_INCLUDE_DIRS}
        ${GST_VIDEO_INCLUDE_DIRS}
)

link_directories(
        ${GST_LIBRARY_DIRS}
        ${GST_VIDEO_LIBRARY_DIRS}
)


#define plugin sources
set(CMAKE_INCLUDE_CURRENT_DIR ON)

# Source and headers
set(core_SRCS
        gstplugins/gstmmalsrc.c
        )

set(core_HDRS
        gstplugins/gstmmalsrc.h
        )

set (MMAL_LIBS mmal_core mmal_util mmal_vc_client mmal)

#Compiler flags
set(CMAKE_MODULE_LINKER_FLAGS "-Wl,--no-as-needed")

add_library(gstmmal MODULE ${core_SRCS} ${core_HDRS})

# Link and installation
target_link_libraries(gstmmal
        ${GST_LIBRARIES}
        ${GST_VIDEO_LIBRARIES}
        ${MMAL_LIBS}
        vcos
        bcm_host
        vchiq_arm
        )
        

install(TARGETS gstmmal LIBRARY DESTINATION lib/gstreamer-${GST_MAJORMINOR})

message(STATUS "CMAKE_MODULE_PATH=${CMAKE_MODULE_PATH}")
message(STATUS "GST_PLUGIN_PATH = ${GST_PLUGIN_PATH}")
message(STATUS "GST_INCLUDE_DIRS = ${GST_INCLUDE_DIRS}")
message(STATUS "GST_VIDEO_INCLUDE_DIRS = ${GST_VIDEO_INCLUDE_DIRS}")
message(STATUS "GST_LIBRARY_DIRS = ${GST_LIBRARY_DIRS}")
message(STATUS "GST_VIDEO_LIBRARY_DIRS = ${GST_VIDEO_LIBRARY_DIRS}")
message(STATUS "GST_LIBRARIES = ${GST_LIBRARIES}")
message(STATUS "GST_VIDEO_LIBRARIES = ${GST_VIDEO_LIBRARIES}")

message(STATUS "COMPILER FLAGS = ${CMAKE_MODULE_LINKER_FLAGS}")
