cmake_minimum_required(VERSION 3.0.0)
project(dump)

include_directories(include)
set(SOURCES main.c)

get_filename_component(PROJECT_TOP "${PROJECT_SOURCE_DIR}/.." ABSOLUTE)
add_subdirectory("${PROJECT_TOP}/lgmp" "${CMAKE_BINARY_DIR}/lgmp")

add_executable(dump ${SOURCES})
target_link_libraries(dump
	lgmp
	curses
)
