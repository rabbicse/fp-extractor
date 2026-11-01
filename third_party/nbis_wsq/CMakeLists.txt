# Minimal subset of NIST NBIS (public domain) needed for WSQ encode/decode.
# Source: NBIS 5.0.0 (imgtools/src/lib/wsq, imgtools/src/lib/jpegl, commonnbis).
cmake_minimum_required(VERSION 3.16)
project(nbis_wsq C)

file(GLOB NBIS_WSQ_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.c)
add_library(nbis_wsq STATIC ${NBIS_WSQ_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/nbis_globals.c)
target_include_directories(nbis_wsq PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
set_target_properties(nbis_wsq PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 99)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  # Legacy K&R-era code: silence warnings, it is not ours to fix.
  target_compile_options(nbis_wsq PRIVATE -w -fcommon)
endif()
target_compile_definitions(nbis_wsq PRIVATE __NBISLE__)
find_library(MATH_LIB m)
if(MATH_LIB)
  target_link_libraries(nbis_wsq PUBLIC ${MATH_LIB})
endif()
