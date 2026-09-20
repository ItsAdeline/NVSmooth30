cmake_minimum_required(VERSION 3.24)
project(NVSmooth30 VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC)
  message(FATAL_ERROR "NVSmooth30 currently targets 64-bit Windows with MSVC")
endif()

add_library(NVSmooth30 SHARED
  src/dllmain.cpp
  src/config.cpp
  src/log.cpp
  src/pe.cpp
  src/fatbin.cpp
  src/nvpresent.cpp
  src/dxgi_hooks.cpp
  src/d3d11_bridge.cpp
  src/osd.cpp
  src/pacer.cpp
  src/version_proxy.cpp
  src/version_proxy.def
)

target_include_directories(NVSmooth30 PRIVATE include)
target_compile_features(NVSmooth30 PRIVATE cxx_std_20)
target_compile_definitions(NVSmooth30 PRIVATE
  WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
  NVS30_VERSION="${PROJECT_VERSION}"
)
target_compile_options(NVSmooth30 PRIVATE /W4 /permissive- /EHsc /guard:cf-)
target_link_options(NVSmooth30 PRIVATE /DEF:${CMAKE_CURRENT_SOURCE_DIR}/src/version_proxy.def)
target_link_libraries(NVSmooth30 PRIVATE
  d3d11 d3d12 dxgi dxguid user32 gdi32 shlwapi
)
set_target_properties(NVSmooth30 PROPERTIES OUTPUT_NAME version)

install(TARGETS NVSmooth30 RUNTIME DESTINATION . LIBRARY DESTINATION .)

