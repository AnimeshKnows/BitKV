cmake_minimum_required(VERSION 3.20)

project(BitKV VERSION 0.1.0 LANGUAGES CXX)

option(BITKV_BUILD_TESTS "Build BitKV unit tests" ON)
option(BITKV_BUILD_BENCH "Build BitKV benchmarks" OFF)
set(BITKV_SANITIZE "" CACHE STRING
  "Sanitizers to enable (e.g. address,undefined or thread). Empty = off.")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(bitkv_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX)
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
  endif()
endfunction()

function(bitkv_apply_sanitize target)
  if(BITKV_SANITIZE STREQUAL "")
    return()
  endif()

  # True MSVC cl.exe only — not clang-cl (which also sets MSVC=1).
  if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    message(WARNING "BITKV_SANITIZE=${BITKV_SANITIZE} is not supported with MSVC cl.exe")
    return()
  endif()

  # clang-cl: address only (/fsanitize=address); no tsan / no GNU -fsanitize= list.
  if(MSVC AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    if(BITKV_SANITIZE MATCHES "thread")
      message(WARNING "BITKV_SANITIZE=thread is not supported with clang-cl")
      return()
    endif()
    if(BITKV_SANITIZE MATCHES "address")
      target_compile_options(${target} PRIVATE /fsanitize=address)
      target_link_options(${target} PRIVATE /fsanitize=address)
    else()
      message(WARNING
        "BITKV_SANITIZE=${BITKV_SANITIZE}: clang-cl only supports address")
    endif()
    return()
  endif()

  # GCC / Clang (GNU driver): single -fsanitize= value on compile and link.
  target_compile_options(${target} PRIVATE
    "-fsanitize=${BITKV_SANITIZE}"
    -fno-omit-frame-pointer)
  target_link_options(${target} PRIVATE
    "-fsanitize=${BITKV_SANITIZE}")
endfunction()

add_library(bitkv_core STATIC
  src/clock.cpp
  src/store.cpp
)
add_library(BitKV::bitkv_core ALIAS bitkv_core)

target_include_directories(bitkv_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

bitkv_set_warnings(bitkv_core)
bitkv_apply_sanitize(bitkv_core)

add_executable(bitkv src/main.cpp)
target_link_libraries(bitkv PRIVATE bitkv_core)
target_compile_definitions(bitkv PRIVATE BITKV_VERSION="${PROJECT_VERSION}")
bitkv_set_warnings(bitkv)
bitkv_apply_sanitize(bitkv)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS bitkv_core
  EXPORT BitKVTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/bitkv
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(TARGETS bitkv
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(EXPORT BitKVTargets
  FILE BitKVTargets.cmake
  NAMESPACE BitKV::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/BitKV
)

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/BitKVConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BitKVConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/BitKVConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/BitKV
)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/BitKVConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/BitKVConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/BitKV
)

export(EXPORT BitKVTargets
  FILE "${CMAKE_CURRENT_BINARY_DIR}/BitKVTargets.cmake"
  NAMESPACE BitKV::
)

if(BITKV_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(BITKV_BUILD_BENCH)
  message(STATUS "BITKV_BUILD_BENCH is ON but no benchmarks are defined yet")
endif()
