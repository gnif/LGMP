cmake_minimum_required(VERSION 3.0.0)
project(relacy-test)

include_directories(include)
set(SOURCES main.cpp)

get_filename_component(PROJECT_TOP "${PROJECT_SOURCE_DIR}/.." ABSOLUTE)
add_subdirectory("${PROJECT_TOP}/lgmp" "${CMAKE_BINARY_DIR}/lgmp")
target_compile_definitions(lgmp PRIVATE -DLGMP_REALACY)

add_executable(relacy-test ${SOURCES})
target_link_libraries(relacy-test
	lgmp
)
target_include_directories(relacy-test
	PUBLIC
		"${PROJECT_TOP}/relacy"
)
