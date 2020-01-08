cmake_minimum_required(VERSION 3.0.0)
project(lgmp LANGUAGES C)

include_directories(include)

set(SOURCES
	src/host.c
	src/client.c
)

add_library(lgmp STATIC ${SOURCES})
target_include_directories(lgmp
	INTERFACE
		include
	PRIVATE
		src
)
