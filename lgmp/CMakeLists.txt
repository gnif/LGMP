# SPDX-License-Identifier: GPL-2.0-or-later

cmake_minimum_required(VERSION 3.10)
project(lgmp LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include_directories(include)

set(SOURCES
  src/host.c
  src/client.c
  src/status.c
  src/spmc.c
  src/stream.c
)

if(CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
  foreach(option
      -Wall
      -Werror
      -Wfatal-errors
      -Wstrict-prototypes
      -ffast-math
      -fdata-sections
      -ffunction-sections)
    add_compile_options("$<$<COMPILE_LANGUAGE:C>:${option}>")
  endforeach()

  add_compile_options(
    "$<$<AND:$<COMPILE_LANGUAGE:C>,$<CONFIG:DEBUG>>:-O0>"
    "$<$<AND:$<COMPILE_LANGUAGE:C>,$<CONFIG:DEBUG>>:-g3>"
    "$<$<AND:$<COMPILE_LANGUAGE:C>,$<CONFIG:DEBUG>>:-ggdb>"
  )
elseif(MSVC)
  foreach(option
      /W4
      /WX
      /utf-8
      /wd4324)
    add_compile_options("$<$<COMPILE_LANGUAGE:C>:${option}>")
  endforeach()
endif()

add_library(lgmp STATIC ${SOURCES})
target_include_directories(lgmp
  INTERFACE
    include
  PRIVATE
    src
)

option(LGMP_BUILD_TESTS "Build LGMP tests" OFF)
if(LGMP_BUILD_TESTS)
  enable_testing()

  if(NOT WIN32)
    find_package(Threads REQUIRED)
  endif()

  add_library(lgmp-test-support STATIC
    tests/test_support.c
  )
  target_include_directories(lgmp-test-support
    PUBLIC
      tests
  )
  target_link_libraries(lgmp-test-support
    PUBLIC
      lgmp
  )
  if(NOT WIN32)
    target_link_libraries(lgmp-test-support PUBLIC Threads::Threads)
  endif()

  function(lgmp_add_test_executable target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE lgmp-test-support)
  endfunction()

  lgmp_add_test_executable(lgmp-core-test        tests/core_test.c)
  lgmp_add_test_executable(lgmp-queue-test       tests/queue_test.c)
  lgmp_add_test_executable(lgmp-stream-test      tests/stream_test.c)
  lgmp_add_test_executable(lgmp-spmc-test        tests/spmc_test.c)
  lgmp_add_test_executable(lgmp-concurrency-test tests/concurrency_test.c)

  add_test(NAME lgmp-core
    COMMAND lgmp-core-test)
  add_test(NAME lgmp-queue-broadcast
    COMMAND lgmp-queue-test broadcast)
  add_test(NAME lgmp-queue-targeted
    COMMAND lgmp-queue-test targeted)
  add_test(NAME lgmp-queue-capacity
    COMMAND lgmp-queue-test capacity)
  add_test(NAME lgmp-queue-client-data
    COMMAND lgmp-queue-test client-data)
  add_test(NAME lgmp-stream-host-to-client
    COMMAND lgmp-stream-test host-to-client)
  add_test(NAME lgmp-stream-client-to-host
    COMMAND lgmp-stream-test client-to-host)
  add_test(NAME lgmp-spmc-basic
    COMMAND lgmp-spmc-test basic)
  add_test(NAME lgmp-spmc-overrun
    COMMAND lgmp-spmc-test overrun)
  add_test(NAME lgmp-concurrency
    COMMAND lgmp-concurrency-test)

  set_tests_properties(
    lgmp-core
    lgmp-queue-broadcast
    lgmp-queue-targeted
    lgmp-queue-capacity
    lgmp-queue-client-data
    lgmp-stream-host-to-client
    lgmp-stream-client-to-host
    lgmp-spmc-basic
    lgmp-spmc-overrun
    lgmp-concurrency
    PROPERTIES TIMEOUT 15
  )
endif()
