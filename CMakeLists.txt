cmake_minimum_required(VERSION 3.22)
project(psx_engine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(SDL2          CONFIG REQUIRED)
find_package(glm           CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(EnTT          CONFIG REQUIRED)

add_subdirectory(vendor)   # builds glad + imgui

add_library(psx_shared INTERFACE)
target_include_directories(psx_shared INTERFACE ${CMAKE_SOURCE_DIR}/shared)

add_subdirectory(engine)
add_subdirectory(editor)
