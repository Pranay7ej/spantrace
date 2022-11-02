cmake_minimum_required(VERSION 3.16)
project(spantrace C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(SPANTRACE_BUILD_PASS "Build the LLVM pass plugin (needs LLVM 18 dev files)" ON)
option(SPANTRACE_BUILD_TESTS "Build tests" ON)
set(SPANTRACE_SANITIZE "" CACHE STRING "Sanitizer for the runtime and its tests (address, ...)")

find_package(Threads REQUIRED)
find_package(Python3 COMPONENTS Interpreter)

if(SPANTRACE_SANITIZE)
  add_compile_options(-fsanitize=${SPANTRACE_SANITIZE} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${SPANTRACE_SANITIZE})
endif()

# --- runtime -----------------------------------------------------------------
add_library(spantrace STATIC runtime/spantrace.cc)
target_include_directories(spantrace PUBLIC include)
target_compile_options(spantrace PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(spantrace PUBLIC Threads::Threads)
set_target_properties(spantrace PROPERTIES ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/runtime)

# --- pass --------------------------------------------------------------------
if(SPANTRACE_BUILD_PASS)
  find_package(LLVM 18 CONFIG)
  if(LLVM_FOUND)
    message(STATUS "LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")
    add_library(SpanTracePass MODULE pass/SpanTracePass.cpp)
    target_include_directories(SpanTracePass SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
    separate_arguments(LLVM_DEFS NATIVE_COMMAND "${LLVM_DEFINITIONS}")
    target_compile_definitions(SpanTracePass PRIVATE ${LLVM_DEFS})
    if(NOT LLVM_ENABLE_RTTI)
      target_compile_options(SpanTracePass PRIVATE -fno-rtti)
    endif()
    # Symbols come from the host (clang / opt), not from linking libLLVM.
    set_target_properties(SpanTracePass PROPERTIES
      PREFIX ""
      LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/pass)
  else()
    message(WARNING "LLVM 18 not found; building the runtime only")
  endif()
endif()

# --- tests -------------------------------------------------------------------
if(SPANTRACE_BUILD_TESTS)
  enable_testing()
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
      URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()

  add_executable(runtime_tests tests/runtime_test.cc)
  target_link_libraries(runtime_tests PRIVATE spantrace GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(runtime_tests)

  add_executable(json_smoke tests/json_smoke.cc)
  target_link_libraries(json_smoke PRIVATE spantrace)
  set(SMOKE_JSON ${CMAKE_BINARY_DIR}/smoke.json)
  add_test(NAME json_smoke_write COMMAND json_smoke ${SMOKE_JSON})
  set_tests_properties(json_smoke_write PROPERTIES FIXTURES_SETUP smoke)
  if(Python3_FOUND)
    add_test(NAME json_smoke_valid COMMAND ${Python3_EXECUTABLE} -m json.tool ${SMOKE_JSON})
    add_test(NAME json_smoke_top
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/top.py ${SMOKE_JSON})
    set_tests_properties(json_smoke_valid json_smoke_top PROPERTIES FIXTURES_REQUIRED smoke)
    set_tests_properties(json_smoke_top PROPERTIES
      PASS_REGULAR_EXPRESSION "player::Decoder::decode\\(int\\) +40 ")
  endif()

  # The runtime ABI, checked with IR shaped like the pass output. Needs clang.
  find_program(CLANG NAMES clang-18 clang)
  if(CLANG AND NOT SPANTRACE_SANITIZE)
    add_test(NAME abi_build
      COMMAND ${CLANG} ${CMAKE_SOURCE_DIR}/tests/abi/pass_shaped.ll $<TARGET_FILE:spantrace>
              -lstdc++ -lpthread -o ${CMAKE_BINARY_DIR}/pass_shaped)
    add_test(NAME abi_run COMMAND ${CMAKE_BINARY_DIR}/pass_shaped)
    set_tests_properties(abi_build PROPERTIES FIXTURES_SETUP abi)
    set_tests_properties(abi_run PROPERTIES FIXTURES_REQUIRED abi
      ENVIRONMENT "SPANTRACE_OUT=${CMAKE_BINARY_DIR}/abi.json")
  endif()

  # FileCheck tests for the pass itself.
  if(TARGET SpanTracePass)
    find_program(OPT opt HINTS ${LLVM_TOOLS_BINARY_DIR} NO_DEFAULT_PATH)
    find_program(FILECHECK FileCheck HINTS ${LLVM_TOOLS_BINARY_DIR} NO_DEFAULT_PATH)
    if(OPT AND FILECHECK)
      file(GLOB PASS_TESTS ${CMAKE_SOURCE_DIR}/tests/pass/*.ll)
      foreach(t ${PASS_TESTS})
        get_filename_component(name ${t} NAME_WE)
        add_test(NAME pass_${name}
          COMMAND sh ${CMAKE_SOURCE_DIR}/tests/pass/run.sh ${OPT} ${FILECHECK}
                  $<TARGET_FILE:SpanTracePass> ${t})
      endforeach()
    else()
      message(WARNING "opt/FileCheck not found; skipping pass tests")
    endif()
  endif()
endif()
