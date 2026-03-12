cmake_minimum_required(VERSION 3.33)
project(stepvr_openvr_detour LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

if(NOT WIN32)
  message(FATAL_ERROR "This project targets Windows only.")
endif()


set(OPENVR_SDK_ROOT ${CMAKE_SOURCE_DIR}/deps/openvr/)
set(OPENVR_HEADERS ${OPENVR_SDK_ROOT}/headers)

file(GLOB hooking_library CONFIGURE_DEPENDS hooking_library/*.cpp hooking_library/*.h )

add_library(stepvr_detour SHARED
	${hooking_library}
)

target_include_directories(stepvr_detour PRIVATE
  ${OPENVR_HEADERS}
  ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(stepvr_detour PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
)

add_subdirectory(${CMAKE_SOURCE_DIR}/deps/minhook)

target_link_libraries(stepvr_detour PRIVATE
  user32
  minhook
)

set_target_properties(stepvr_detour PROPERTIES
  OUTPUT_NAME "stepvr_detour"
)

file(GLOB injector_src CONFIGURE_DEPENDS injector/*.cpp) 

add_executable(stepvr_injector ${injector_src})


