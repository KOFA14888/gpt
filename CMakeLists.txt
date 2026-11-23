cmake_minimum_required(VERSION 3.16)
project(screenmate LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(SM_SOURCES
  src/main.cpp
  src/http.cpp
  src/lsp.cpp
  src/tools.cpp
  src/agent.cpp
  src/tui.cpp
  src/webui.cpp
  src/desktop.cpp
)

if(WIN32)
  list(APPEND SM_SOURCES
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
    third_party/imgui/imgui_demo.cpp
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp
    third_party/imgui/misc/cpp/imgui_stdlib.cpp
  )
endif()

add_executable(screenmate ${SM_SOURCES})

target_include_directories(screenmate PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/backends
)

if(MSVC)
  target_compile_options(screenmate PRIVATE /utf-8 /W3 /EHsc)
  target_compile_definitions(screenmate PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
  )
else()
  target_compile_options(screenmate PRIVATE -Wall -Wextra -Wno-unused-parameter)
  # Baseline ISA: ubuntu-latest (Ubuntu 26.04, since 2026-10-30) ships an
  # amd64v3 GCC whose default -march is x86-64-v3 (AVX2/FMA), which crashes
  # old CPUs with "CPU ISA level is lower than required" (glibc ld.so).
  # Force plain x86-64 so release binaries run on any x86-64 processor.
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    target_compile_options(screenmate PRIVATE -march=x86-64 -mtune=generic)
  endif()
endif()

if(WIN32)
  target_link_libraries(screenmate PRIVATE
    winhttp user32 gdi32 psapi shell32 advapi32 ws2_32
    d3d11 dxgi
  )
else()
  find_package(CURL QUIET)
  if(CURL_FOUND)
    target_link_libraries(screenmate PRIVATE CURL::libcurl pthread)
  else()
    # smoke-build without HTTPS (tools/UI still compile; network calls error at runtime)
    target_compile_definitions(screenmate PRIVATE SM_NO_HTTP=1)
    find_package(Threads REQUIRED)
    target_link_libraries(screenmate PRIVATE Threads::Threads)
  endif()
endif()

install(TARGETS screenmate RUNTIME DESTINATION bin)
