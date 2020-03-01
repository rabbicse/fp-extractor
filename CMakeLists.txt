cmake_minimum_required(VERSION 3.10)

# set the project name
project(FingerprintExtractor)

# add the executable
add_executable(FingerprintExtractor src/fp_extractor_example.cpp src/include/FJFX.h src/include/FRFXLL.h src/fp_ext_bak.cpp src/fp_ext_bak.h)