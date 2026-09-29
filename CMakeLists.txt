cmake_minimum_required(VERSION 3.24)
project(khonsu LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(KHONSU_BUILD_TESTS "Build unit/property tests" ON)
option(KHONSU_BUILD_BENCH "Build benchmarks" ON)
option(KHONSU_BUILD_WEB "Build the WebAssembly module for the web app (Emscripten only)" OFF)

# Determinism boundary: no fast-math, no FP contraction (see docs/DESIGN.md §7.2).
set(KHONSU_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
set(KHONSU_DETERMINISM -ffp-contract=off -fno-fast-math)

if(EMSCRIPTEN)
  # SIMD128 everywhere; native wasm exceptions so the JS bridge can report errors.
  set(KHONSU_TARGET_FLAGS -msimd128 -fwasm-exceptions)
  add_link_options(-fwasm-exceptions)
endif()

add_library(khonsu_kernel STATIC
  kernel/src/common.cpp
  kernel/src/store.cpp
  kernel/src/log.cpp
  kernel/src/json.cpp
  kernel/src/jsonv.cpp
  kernel/src/recording.cpp
  kernel/src/snapshot.cpp
  kernel/src/rundiff.cpp
  kernel/src/model.cpp
  kernel/src/kernel.cpp
  kernel/src/bench.cpp
)
target_include_directories(khonsu_kernel PUBLIC kernel/include PRIVATE third_party/xxhash)
target_compile_options(khonsu_kernel PRIVATE ${KHONSU_WARNINGS} ${KHONSU_DETERMINISM} ${KHONSU_TARGET_FLAGS})

if(KHONSU_BUILD_TESTS)
  enable_testing()
  add_executable(khonsu_tests tests/main.cpp tests/test_store.cpp tests/test_log.cpp tests/test_kernel.cpp
    tests/test_jsonv.cpp tests/test_recording.cpp tests/test_snapshot.cpp)
  target_link_libraries(khonsu_tests PRIVATE khonsu_kernel)
  target_compile_options(khonsu_tests PRIVATE ${KHONSU_TARGET_FLAGS})
  if(EMSCRIPTEN)
    target_link_options(khonsu_tests PRIVATE -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=1048576 -sEXIT_RUNTIME=1)
    add_test(NAME khonsu_tests COMMAND node ${CMAKE_CURRENT_BINARY_DIR}/khonsu_tests.js)
  else()
    add_test(NAME khonsu_tests COMMAND khonsu_tests)
  endif()
endif()

if(KHONSU_BUILD_BENCH)
  add_executable(khonsu_bench bench/bench_fork.cpp)
  target_link_libraries(khonsu_bench PRIVATE khonsu_kernel)
  target_compile_options(khonsu_bench PRIVATE ${KHONSU_TARGET_FLAGS})
  if(EMSCRIPTEN)
    target_link_options(khonsu_bench PRIVATE -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB -sSTACK_SIZE=1048576
                        -sEXIT_RUNTIME=1)
  endif()
endif()

# Native CLI: `khonsu record | replay | serve | show | verify` (POSIX; Linux today).
if(NOT EMSCRIPTEN AND UNIX)
  find_package(Threads REQUIRED)
  find_package(OpenSSL)
  add_executable(khonsu
    platform/native/main.cpp
    platform/native/proxy.cpp
    platform/native/http.cpp
    platform/native/net.cpp
    platform/native/file_log.cpp
    platform/native/process.cpp
    platform/native/workspace.cpp)
  target_link_libraries(khonsu PRIVATE khonsu_kernel Threads::Threads)
  target_compile_options(khonsu PRIVATE ${KHONSU_WARNINGS})
  if(OpenSSL_FOUND)
    target_link_libraries(khonsu PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(khonsu PRIVATE KHONSU_WITH_TLS=1)
  else()
    message(WARNING "OpenSSL not found: khonsu will only reach http:// upstreams")
    target_compile_definitions(khonsu PRIVATE KHONSU_WITH_TLS=0)
  endif()
endif()

if(KHONSU_BUILD_WEB)
  if(NOT EMSCRIPTEN)
    message(FATAL_ERROR "KHONSU_BUILD_WEB requires the Emscripten toolchain (emcmake cmake ...)")
  endif()
  add_executable(khonsu_web platform/web/bindings.cpp)
  target_link_libraries(khonsu_web PRIVATE khonsu_kernel)
  target_compile_options(khonsu_web PRIVATE ${KHONSU_WARNINGS} ${KHONSU_TARGET_FLAGS})
  set_target_properties(khonsu_web PROPERTIES OUTPUT_NAME khonsu SUFFIX ".mjs")
  target_link_options(khonsu_web PRIVATE
    -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createKhonsu -sENVIRONMENT=web,worker,node
    -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=4GB -sSTACK_SIZE=1048576
    -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToUTF8,lengthBytesUTF8,HEAPU8
    -sEXPORTED_FUNCTIONS=_malloc,_free
    -sFILESYSTEM=0 -sASSERTIONS=0)
  # Drop the module into the web app (served from web/public/wasm).
  add_custom_command(TARGET khonsu_web POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/web/public/wasm
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE_DIR:khonsu_web>/khonsu.mjs $<TARGET_FILE_DIR:khonsu_web>/khonsu.wasm
            ${CMAKE_SOURCE_DIR}/web/public/wasm/
    COMMENT "Copying khonsu.mjs/.wasm into web/public/wasm")
endif()
