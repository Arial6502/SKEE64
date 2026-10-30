add_library("${PROJECT_NAME}" SHARED)

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

include(AddCXXFiles)
add_cxx_files("${PROJECT_NAME}")

# Stands in for the old project's forced include of ITypes.h.
target_precompile_headers(
	"${PROJECT_NAME}"
	PRIVATE
	src/ITypes.h
)

# Build DLL RC
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/version.rc"
    @ONLY
)
target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/version.rc")

set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)

target_compile_definitions(
	"${PROJECT_NAME}"
	PRIVATE
	_WINDOWS
	_USRDLL
	XBYAK_NO_OP_NAMES
)

target_compile_options(
    "${PROJECT_NAME}"
    PRIVATE
    /MP
    /utf-8
    /Zc:__cplusplus
    /W3
    /permissive # SKSE 2.0.20's headers predate the conformance mode C++20 turns on
)

target_include_directories(
	${PROJECT_NAME}
	PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/src
)
