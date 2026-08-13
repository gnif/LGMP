cmake_minimum_required(VERSION 3.10)
project(lgmp LANGUAGES C)

include_directories(include)

set(SOURCES
	src/host.c
	src/client.c
	src/status.c
)

add_compile_options(
  "-Wall"
  "-Werror"
  "-Wstrict-prototypes"
  "-Wfatal-errors"
  "-ffast-math"
  "-fdata-sections"
  "-ffunction-sections"
  "$<$<CONFIG:DEBUG>:-O0;-g3;-ggdb>"
)

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
  find_package(Threads REQUIRED)

  add_executable(lgmp-queue-capacity-test
    tests/queue_capacity_test.c
  )
  target_link_libraries(lgmp-queue-capacity-test
    PRIVATE
      lgmp
      Threads::Threads
  )

  add_test(
    NAME lgmp-queue-capacity
    COMMAND lgmp-queue-capacity-test
  )
endif()
