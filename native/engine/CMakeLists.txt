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
  src/backends/best_of_n_backend.cpp
  src/backends/bsc_backend.cpp
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

# ---------------------------------------------------------------------------
# brunsli (MIT) - lossless JPEG transcoding, with its brotli (MIT) dependency.
# We compile both as plain static libraries from the vendored sources rather
# than via their own CMake projects, which avoids the upstream FetchContent
# calls (GTest/Brotli) that require network access at configure time.
# ---------------------------------------------------------------------------
option(DEVZIP_ENABLE_BRUNSLI "Enable brunsli JPEG transcoding" ON)
set(DEVZIP_BROTLI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/brotli/c)
set(DEVZIP_BRUNSLI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/brunsli/c)
if(DEVZIP_ENABLE_BRUNSLI AND EXISTS ${DEVZIP_BRUNSLI_DIR} AND EXISTS ${DEVZIP_BROTLI_DIR})
  file(GLOB DEVZIP_BROTLI_COMMON ${DEVZIP_BROTLI_DIR}/common/*.c)
  file(GLOB DEVZIP_BROTLI_DEC ${DEVZIP_BROTLI_DIR}/dec/*.c)
  file(GLOB DEVZIP_BROTLI_ENC ${DEVZIP_BROTLI_DIR}/enc/*.c)
  add_library(devzip_brotli STATIC
    ${DEVZIP_BROTLI_COMMON} ${DEVZIP_BROTLI_DEC} ${DEVZIP_BROTLI_ENC})
  target_include_directories(devzip_brotli PUBLIC ${DEVZIP_BROTLI_DIR}/include)

  file(GLOB DEVZIP_BRUNSLI_COMMON ${DEVZIP_BRUNSLI_DIR}/common/*.cc)
  add_library(devzip_brunsli STATIC
    ${DEVZIP_BRUNSLI_COMMON}
    ${DEVZIP_BRUNSLI_DIR}/dec/ans_decode.cc
    ${DEVZIP_BRUNSLI_DIR}/dec/bit_reader.cc
    ${DEVZIP_BRUNSLI_DIR}/dec/brunsli_decode.cc
    ${DEVZIP_BRUNSLI_DIR}/dec/context_map_decode.cc
    ${DEVZIP_BRUNSLI_DIR}/dec/histogram_decode.cc
    ${DEVZIP_BRUNSLI_DIR}/dec/huffman_decode.cc
    ${DEVZIP_BRUNSLI_DIR}/dec/huffman_table.cc
    ${DEVZIP_BRUNSLI_DIR}/dec/jpeg_data_writer.cc
    ${DEVZIP_BRUNSLI_DIR}/dec/state.cc
    ${DEVZIP_BRUNSLI_DIR}/dec/decode.cc
    ${DEVZIP_BRUNSLI_DIR}/enc/ans_encode.cc
    ${DEVZIP_BRUNSLI_DIR}/enc/brunsli_encode.cc
    ${DEVZIP_BRUNSLI_DIR}/enc/context_map_encode.cc
    ${DEVZIP_BRUNSLI_DIR}/enc/histogram_encode.cc
    ${DEVZIP_BRUNSLI_DIR}/enc/huffman_encode.cc
    ${DEVZIP_BRUNSLI_DIR}/enc/huffman_tree.cc
    ${DEVZIP_BRUNSLI_DIR}/enc/jpeg_data_reader.cc
    ${DEVZIP_BRUNSLI_DIR}/enc/jpeg_huffman_decode.cc
    ${DEVZIP_BRUNSLI_DIR}/enc/write_bits.cc
    ${DEVZIP_BRUNSLI_DIR}/enc/encode.cc
  )
  target_include_directories(devzip_brunsli
    PUBLIC ${DEVZIP_BRUNSLI_DIR}/include
    PRIVATE ${DEVZIP_BRUNSLI_DIR}/.. ${DEVZIP_BROTLI_DIR}/include)
  target_link_libraries(devzip_brunsli PUBLIC devzip_brotli)
  if(MSVC)
    target_compile_definitions(devzip_brotli PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(devzip_brotli PRIVATE /MP /wd4267 /wd4244)
    target_compile_options(devzip_brunsli PRIVATE /MP /wd4267 /wd4244 /EHsc)
  endif()

  target_link_libraries(devzip_engine PUBLIC devzip_brunsli)
  target_compile_definitions(devzip_engine PRIVATE DEVZIP_HAVE_BRUNSLI=1)
  set_target_properties(devzip_brunsli PROPERTIES CXX_SCAN_FOR_MODULES OFF)
endif()

# ---------------------------------------------------------------------------
# preflate (Apache-2.0) - lossless deflate-stream split/reconstruct, used to
# undo PNG/ZIP/GZIP/PDF deflate so the backend can model the raw bytes and the
# original deflate stream is rebuilt bit-exactly on extract.
# ---------------------------------------------------------------------------
option(DEVZIP_ENABLE_PREFLATE "Enable preflate deflate recompression" ON)
set(DEVZIP_PREFLATE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/preflate)
if(DEVZIP_ENABLE_PREFLATE AND EXISTS ${DEVZIP_PREFLATE_DIR})
  file(GLOB DEVZIP_PREFLATE_ROOT ${DEVZIP_PREFLATE_DIR}/preflate_*.cpp)
  file(GLOB DEVZIP_PREFLATE_SUPPORT ${DEVZIP_PREFLATE_DIR}/support/*.cpp)
  add_library(devzip_preflate STATIC ${DEVZIP_PREFLATE_ROOT} ${DEVZIP_PREFLATE_SUPPORT})
  target_include_directories(devzip_preflate PUBLIC ${DEVZIP_PREFLATE_DIR})
  set_target_properties(devzip_preflate PROPERTIES CXX_SCAN_FOR_MODULES OFF)
  if(MSVC)
    target_compile_definitions(devzip_preflate PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    target_compile_options(devzip_preflate PRIVATE /MP /EHsc /wd4267 /wd4244 /wd4018)
  endif()
  target_link_libraries(devzip_engine PUBLIC devzip_preflate)
  target_compile_definitions(devzip_engine PRIVATE DEVZIP_HAVE_PREFLATE=1)
endif()

# ---------------------------------------------------------------------------
# libbsc (Apache-2.0) - BWT/ST block-sorting compressor.  Strong on text/log
# data and an additional best-of-N candidate.  CUDA acceleration of the sort
# transform (libcubwt) is available behind DEVZIP_ENABLE_CUDA for "speed at
# equal ratio"; the CPU path is always built.
# ---------------------------------------------------------------------------
option(DEVZIP_ENABLE_BSC "Enable the libbsc backend" ON)
option(DEVZIP_ENABLE_CUDA "Build libbsc with CUDA sort-transform acceleration" OFF)
set(DEVZIP_BSC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/libbsc/libbsc)
if(DEVZIP_ENABLE_BSC AND EXISTS ${DEVZIP_BSC_DIR})
  add_library(devzip_bsc STATIC
    ${DEVZIP_BSC_DIR}/libbsc/libbsc.cpp
    ${DEVZIP_BSC_DIR}/adler32/adler32.cpp
    ${DEVZIP_BSC_DIR}/bwt/bwt.cpp
    ${DEVZIP_BSC_DIR}/bwt/libsais/libsais.c
    ${DEVZIP_BSC_DIR}/coder/coder.cpp
    ${DEVZIP_BSC_DIR}/coder/qlfc/qlfc.cpp
    ${DEVZIP_BSC_DIR}/coder/qlfc/qlfc_model.cpp
    ${DEVZIP_BSC_DIR}/filters/detectors.cpp
    ${DEVZIP_BSC_DIR}/filters/preprocessing.cpp
    ${DEVZIP_BSC_DIR}/lzp/lzp.cpp
    ${DEVZIP_BSC_DIR}/platform/platform.cpp
    ${DEVZIP_BSC_DIR}/st/st.cpp
  )
  target_include_directories(devzip_bsc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/vendor/libbsc)
  set_target_properties(devzip_bsc PROPERTIES CXX_SCAN_FOR_MODULES OFF)
  if(MSVC)
    target_compile_definitions(devzip_bsc PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
    target_compile_options(devzip_bsc PRIVATE /MP /EHsc /wd4267 /wd4244 /wd4018 /wd4146)
  endif()
  target_link_libraries(devzip_engine PUBLIC devzip_bsc)
  target_compile_definitions(devzip_engine PRIVATE DEVZIP_HAVE_BSC=1)
endif()

include(CheckCCompilerFlag)
include(CheckCXXCompilerFlag)

if(MSVC)
  # MSVC exposes SSE4.2 intrinsics (e.g. _mm_crc32_*) unconditionally; /arch:AVX2
  # enables AVX2/BMI/BMI2 codegen and autovectorization for the hot paths.
  check_cxx_compiler_flag("/arch:AVX2" DEVZIP_MSVC_HAS_AVX2)
  if(DEVZIP_MSVC_HAS_AVX2)
    target_compile_options(devzip_engine PRIVATE /arch:AVX2)
    set(DEVZIP_HAS_AVX2 ON)
    set(DEVZIP_HAS_SSE42 ON)
  endif()
  # Parallelize translation units within each project (the LZMA SDK is large).
  target_compile_options(devzip_engine PRIVATE /MP)
else()
  check_cxx_compiler_flag(-msse4.2 DEVZIP_HAS_SSE42)
  check_c_compiler_flag(-mavx2 DEVZIP_HAS_AVX2)

  if(DEVZIP_HAS_SSE42)
    target_compile_options(devzip_engine PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-msse4.2>)
    target_compile_options(devzip_engine PRIVATE $<$<COMPILE_LANGUAGE:C>:-msse4.2>)
  endif()
  if(DEVZIP_HAS_AVX2)
    target_compile_options(devzip_engine PRIVATE $<$<COMPILE_LANGUAGE:C>:-mavx2 -mbmi -mbmi2>)
  endif()
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
