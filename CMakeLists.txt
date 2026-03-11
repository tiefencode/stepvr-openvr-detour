cmake_minimum_required(VERSION 3.20)
project(stepvr_openvr_detour LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
  message(FATAL_ERROR "This project targets Windows only.")
endif()

if(NOT DEFINED ENV{OPENVR_SDK_ROOT})
  message(FATAL_ERROR "Please set OPENVR_SDK_ROOT to your OpenVR SDK checkout.")
endif()

set(OPENVR_SDK_ROOT $ENV{OPENVR_SDK_ROOT})
set(OPENVR_HEADERS ${OPENVR_SDK_ROOT}/headers)

add_library(stepvr_detour SHARED
  dllmain.cpp
  detour_controller_state.cpp
  shared.cpp
)

target_include_directories(stepvr_detour PRIVATE
  ${OPENVR_HEADERS}
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(stepvr_detour PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
)

target_link_libraries(stepvr_detour PRIVATE
  user32
)

set_target_properties(stepvr_detour PROPERTIES
  OUTPUT_NAME "stepvr_detour"
)