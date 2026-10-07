# DAYFALL engine: a Vulkan 1.3 renderer and game runtime built for DAYFALL.
# Builds on Windows (MSVC, Visual Studio 2022) and Linux (GCC or Clang).
cmake_minimum_required(VERSION 3.24)
project(DayfallEngine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
# The engine always lands in <repo>/bin (bin/dayfall.exe on Windows) next to
# its shaders, whatever the generator or configuration: docs, .mcp.json and
# agents can rely on one path.
set(DF_BIN_DIR ${CMAKE_SOURCE_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${DF_BIN_DIR})
foreach(cfg Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER ${cfg} CFG)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CFG} ${DF_BIN_DIR})
endforeach()

option(DF_ENABLE_VALIDATION "Enable Vulkan validation layers by default in debug builds" ON)

include(cmake/Dependencies.cmake)
include(cmake/Shaders.cmake)

file(GLOB_RECURSE DF_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.h)
add_executable(dayfall ${DF_SOURCES} ${volk_SOURCE_DIR}/volk.c)
target_include_directories(dayfall PRIVATE src ${volk_SOURCE_DIR} ${vma_SOURCE_DIR}/include
  ${glm_SOURCE_DIR} ${cgltf_SOURCE_DIR} ${stb_SOURCE_DIR} ${json_SOURCE_DIR}/include)
target_link_libraries(dayfall PRIVATE Vulkan::Headers glfw Jolt meshoptimizer)
target_compile_definitions(dayfall PRIVATE
  VK_NO_PROTOTYPES GLFW_INCLUDE_NONE GLM_FORCE_DEPTH_ZERO_TO_ONE GLM_FORCE_RADIANS GLM_ENABLE_EXPERIMENTAL
  $<$<CONFIG:Debug>:DF_DEBUG=1>
  $<$<BOOL:${DF_ENABLE_VALIDATION}>:DF_VALIDATION_DEFAULT=1>)
if(WIN32)
  target_compile_definitions(dayfall PRIVATE VK_USE_PLATFORM_WIN32_KHR NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(dayfall PRIVATE ws2_32)   # the MCP HTTP server
endif()
if(MSVC)
  target_compile_options(dayfall PRIVATE /W3 /permissive- /Zc:__cplusplus /MP)
else()
  target_compile_options(dayfall PRIVATE -Wall -Wno-unused-parameter -Wno-missing-field-initializers)
  target_link_libraries(dayfall PRIVATE ${CMAKE_DL_LIBS} pthread)
endif()

df_compile_shaders(dayfall_shaders ${CMAKE_SOURCE_DIR}/shaders ${DF_BIN_DIR}/shaders)
add_dependencies(dayfall dayfall_shaders)
