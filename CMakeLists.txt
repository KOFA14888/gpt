cmake_minimum_required(VERSION 3.16)
project(screenmate LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(screenmate
  src/main.cpp
  src/http.cpp
  src/screen.cpp
  src/cursor.cpp
  src/lsp.cpp
  src/tools.cpp
  src/agent.cpp
  src/tui.cpp
  src/webui.cpp
)

target_include_directories(screenmate PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
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
endif()

if(WIN32)
  target_link_libraries(screenmate PRIVATE
    winhttp user32 gdi32 psapi shell32 advapi32 ws2_32
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

# stb headers live under third_party/stb — include path is third_party, so
# #include <stb/stb_image_write.h> works.

install(TARGETS screenmate RUNTIME DESTINATION bin)
