cmake_minimum_required(VERSION 3.28)

project(DevZipEngine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

option(DEVZIP_BUILD_TESTS "Build the native engine tests" ON)

add_library(devzip_engine STATIC
  src/archive_format.cpp
  src/source_scanner.cpp
  src/transform_pipeline.cpp
  src/solid_packer.cpp
  src/transforms/fastcdc_chunker.cpp
  src/transforms/stream_normalizer.cpp
  src/transforms/code_dictionary.cpp
  src/backends/best_of_three_backend.cpp
  src/backends/best_of_two_backend.cpp
  src/backends/libzpaq_backend.cpp
  src/backends/lzma_sdk_backend.cpp
  src/backends/ppmd_backend.cpp
  src/backends/selective_zpaq_backend.cpp
  src/extractor.cpp
  src/temp_file_guard.cpp
  vendor/libzpaq/libzpaq.cpp
  vendor/miniz/miniz.c
  vendor/lzma-sdk/C/Alloc.c
  vendor/lzma-sdk/C/Bra.c
  vendor/lzma-sdk/C/Bra86.c
  vendor/lzma-sdk/C/CpuArch.c
  vendor/lzma-sdk/C/7zStream.c
  vendor/lzma-sdk/C/LzFind.c
  vendor/lzma-sdk/C/LzFindOpt.c
  vendor/lzma-sdk/C/LzFindMt.c
  vendor/lzma-sdk/C/LzmaDec.c
  vendor/lzma-sdk/C/LzmaEnc.c
  vendor/lzma-sdk/C/Lzma2Dec.c
  vendor/lzma-sdk/C/Lzma2Enc.c
  vendor/lzma-sdk/C/MtCoder.c
  vendor/lzma-sdk/C/MtDec.c
  vendor/lzma-sdk/C/Ppmd7.c
  vendor/lzma-sdk/C/Ppmd7Dec.c
  vendor/lzma-sdk/C/Ppmd7Enc.c
  vendor/lzma-sdk/C/Threads.c
)

target_include_directories(devzip_engine
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/libzpaq
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/lzma-sdk/C
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/miniz
)

target_compile_definitions(devzip_engine PRIVATE _CRT_SECURE_NO_WARNINGS)

include(CheckCCompilerFlag)
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-msse4.2 DEVZIP_HAS_SSE42)
check_c_compiler_flag(-mavx2 DEVZIP_HAS_AVX2)

if(DEVZIP_HAS_SSE42)
  target_compile_options(devzip_engine PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-msse4.2>)
  target_compile_options(devzip_engine PRIVATE $<$<COMPILE_LANGUAGE:C>:-msse4.2>)
endif()
if(DEVZIP_HAS_AVX2)
  target_compile_options(devzip_engine PRIVATE $<$<COMPILE_LANGUAGE:C>:-mavx2 -mbmi -mbmi2>)
endif()

add_executable(devzip_cli src/cli/main.cpp)
target_link_libraries(devzip_cli PRIVATE devzip_engine)
set_target_properties(devzip_engine devzip_cli PROPERTIES CXX_SCAN_FOR_MODULES OFF)

if(DEVZIP_BUILD_TESTS)
  enable_testing()

  add_executable(devzip_tests
    tests/test_main.cpp
    tests/corpus_ratio_tests.cpp
    tests/roundtrip_metadata_tests.cpp
    tests/roundtrip_archive_tests.cpp
    tests/transform_roundtrip_tests.cpp
    tests/transform_ratio_tests.cpp
    tests/routing_ratio_tests.cpp
    tests/extract_integrity_tests.cpp
    tests/failure_cleanup_tests.cpp
  )

  target_include_directories(devzip_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/tests
  )

  target_link_libraries(devzip_tests PRIVATE devzip_engine)
  set_target_properties(devzip_tests PROPERTIES CXX_SCAN_FOR_MODULES OFF)
  add_test(NAME devzip_tests COMMAND devzip_tests)
endif()
