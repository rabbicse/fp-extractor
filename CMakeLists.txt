cmake_minimum_required(VERSION 3.16)
project(fp_extractor VERSION 2.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(FPX_BUILD_TESTS   "Build unit / integration tests"            ON)
option(FPX_BUILD_SERVER  "Build the HTTP identification server"      ON)
option(FPX_BUILD_EXAMPLES "Build C++ examples"                       ON)
option(FPX_NATIVE_ARCH   "Optimise for the build machine (-march=native)" OFF)

include(cmake/Dependencies.cmake)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  set(FPX_WARNINGS -Wall -Wextra -Wpedantic -Wno-unused-parameter)
  if(FPX_NATIVE_ARCH)
    add_compile_options(-march=native)
  endif()
endif()

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Core library: image IO, WSQ, FingerJet extraction, template codec, matcher,
# gallery (1:N), persistent template store, C API.
# ---------------------------------------------------------------------------
add_library(fpx_core STATIC
  src/core/image.cpp
  src/core/wsq.cpp
  src/core/extractor.cpp
  src/core/template.cpp
  src/core/matcher.cpp
  src/core/gallery.cpp
  src/core/store.cpp
  src/core/synthetic.cpp
)
add_library(fpx::core ALIAS fpx_core)
target_include_directories(fpx_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(fpx_core PRIVATE fingerjet nbis_wsq PUBLIC Threads::Threads)
target_compile_options(fpx_core PRIVATE ${FPX_WARNINGS})

# Shared C library for FFI (Python ctypes, C#, Java/JNA, Go cgo, ...)
add_library(fpx SHARED src/core/c_api.cpp)
target_link_libraries(fpx PRIVATE fpx_core)
target_include_directories(fpx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---------------------------------------------------------------------------
# Command line tool
# ---------------------------------------------------------------------------
add_executable(fpx_cli src/cli/main.cpp)
set_target_properties(fpx_cli PROPERTIES OUTPUT_NAME fpx)
target_link_libraries(fpx_cli PRIVATE fpx_core)
target_compile_options(fpx_cli PRIVATE ${FPX_WARNINGS})

# ---------------------------------------------------------------------------
# HTTP server
# ---------------------------------------------------------------------------
if(FPX_BUILD_SERVER)
  add_executable(fpx_server src/server/main.cpp)
  set_target_properties(fpx_server PROPERTIES OUTPUT_NAME fpx-server)
  target_link_libraries(fpx_server PRIVATE fpx_core httplib::httplib nlohmann_json::nlohmann_json)
  target_compile_options(fpx_server PRIVATE ${FPX_WARNINGS})
endif()

if(FPX_BUILD_EXAMPLES)
  add_executable(example_pipeline examples/cpp/pipeline.cpp)
  target_link_libraries(example_pipeline PRIVATE fpx_core)
endif()

if(FPX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

include(GNUInstallDirs)
# Installed binaries find libFRFXLL / libfpx next to them in ../lib.
set_target_properties(fpx_cli fpx PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
if(FPX_BUILD_SERVER)
  set_target_properties(fpx_server PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()
install(TARGETS fpx_cli fpx fingerjet RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
                                      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
if(FPX_BUILD_SERVER)
  install(TARGETS fpx_server RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(DIRECTORY include/fpx DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
