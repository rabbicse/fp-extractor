# Third-party dependencies.
#
# Everything is fetched at configure time and pinned, so a fresh clone builds
# with nothing but a compiler + CMake. For offline / air-gapped builds point
# the *_SOURCE_DIR cache variables at local checkouts, e.g.
#   cmake -B build -DFPX_FINGERJET_SOURCE_DIR=/opt/src/FingerJetFXOSE
include(FetchContent)
if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)  # allow FetchContent_Populate (we only need sources)
endif()
set(FETCHCONTENT_QUIET ON)

# --- FingerJet FX OSE (LGPL-3.0) - minutiae extraction ----------------------
set(FPX_FINGERJET_GIT_TAG 1726ba08bf7f2137d2f861ac1ae124d5cd355eee CACHE STRING "FingerJetFXOSE commit")
set(FPX_FINGERJET_SOURCE_DIR "" CACHE PATH "Use a local FingerJetFXOSE checkout instead of downloading")
if(FPX_FINGERJET_SOURCE_DIR)
  set(fingerjet_SOURCE_DIR ${FPX_FINGERJET_SOURCE_DIR})
else()
  FetchContent_Declare(fingerjet
    GIT_REPOSITORY https://github.com/FingerJetFXOSE/FingerJetFXOSE.git
    GIT_TAG        ${FPX_FINGERJET_GIT_TAG})
  FetchContent_GetProperties(fingerjet)
  if(NOT fingerjet_POPULATED)
    FetchContent_Populate(fingerjet)   # we only need the sources, not their CMake setup
  endif()
endif()
set(FJ_ROOT ${fingerjet_SOURCE_DIR}/FingerJetFXOSE)
file(GLOB FRFXLL_SOURCES ${FJ_ROOT}/libFRFXLL/src/lib/*.cpp)
if(NOT FRFXLL_SOURCES)
  message(FATAL_ERROR "FingerJet sources not found under ${FJ_ROOT}")
endif()
# Built as a shared library so the LGPL component stays replaceable by users.
add_library(fingerjet SHARED ${FRFXLL_SOURCES})
target_include_directories(fingerjet
  PUBLIC  ${FJ_ROOT}/libFRFXLL/include
  PRIVATE ${FJ_ROOT}/libFRFXLL/src/algorithm)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(fingerjet PRIVATE -O3 -w -Wno-deprecated)
  target_compile_definitions(fingerjet PRIVATE LINUX)
endif()
set_target_properties(fingerjet PROPERTIES CXX_STANDARD 11 OUTPUT_NAME FRFXLL)

# --- NIST NBIS WSQ codec (public domain, vendored subset) -------------------
add_subdirectory(${PROJECT_SOURCE_DIR}/third_party/nbis_wsq)

# --- cpp-httplib (MIT) + nlohmann/json (MIT) for the server ------------------
if(FPX_BUILD_SERVER)
  FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.15.3
    GIT_SHALLOW    TRUE)
  set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
  set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
  set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
  set(HTTPLIB_INSTALL OFF CACHE BOOL "" FORCE)
  set(JSON_Install OFF CACHE BOOL "" FORCE)
  set(JSON_BuildTests OFF CACHE INTERNAL "")
  FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE)
  FetchContent_MakeAvailable(httplib json)
endif()

# --- doctest (MIT) for tests -------------------------------------------------
if(FPX_BUILD_TESTS)
  FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.4.11
    GIT_SHALLOW    TRUE)
  FetchContent_GetProperties(doctest)
  if(NOT doctest_POPULATED)
    FetchContent_Populate(doctest)
  endif()
  add_library(doctest_header INTERFACE)
  target_include_directories(doctest_header INTERFACE ${doctest_SOURCE_DIR})
endif()
