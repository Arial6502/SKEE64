cmake_minimum_required(VERSION 3.21)
cmake_policy(VERSION 3.21...3.31)

# #######################################################################################################################
# # Project Definition
# #######################################################################################################################

project(
	skee64
	VERSION 0.4.20.1
	DESCRIPTION "Skyrim 64 Engine Extender"
	LANGUAGES CXX
)

# Not Exposed By CMake, Manually Set Here for use by version.rc.in
set(PROJECT_COPYRIGHT "Copyright (C) 2026 Brendan Borthwick (expired6978)")

# Options
option(PL_DEPLOY_TO_FOLDER "Copy The Built DLL and PDB to the EnvVar Defined Target location" ON)
option(PL_BUILD_DISTRIBUTION "Construct a distribution folder after each build" ON)

# #######################################################################################################################
# # Flags
# #######################################################################################################################

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
#Disables Module Support, we don't use any anyways and it just slows compilation down.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
add_compile_definitions(_DISABLE_EXTENDED_ALIGNED_STORAGE)

# Check if MSVC is the compiler
if(MSVC)
    # Extract the version number of the MSVC compiler
    string(REGEX REPLACE "^([0-9]+)\\.([0-9]+).*" "\\1.\\2" MSVC_VERSION_STRING "${MSVC_VERSION}")
    message(STATUS "Detected MSVC version: ${MSVC_VERSION_STRING}")

    # Check if the MSVC version is at least 14.39
    if(MSVC_VERSION LESS 1939)
        message(FATAL_ERROR "MSVC version 14.39 or higher is required. Detected version: ${MSVC_VERSION_STRING}")
    endif()
else()
    message(FATAL_ERROR "This project currently only supports MSVC as the compiler.")
endif()

# #######################################################################################################################
# # Project modules
# #######################################################################################################################
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(SKSE)
include(XSEPlugin)

# #######################################################################################################################
# # Find dependencies - VCPKG
# #######################################################################################################################
find_package(spdlog CONFIG REQUIRED)
find_package(glaze CONFIG REQUIRED)
find_package(absl CONFIG REQUIRED)
find_package(directxtex CONFIG REQUIRED)
find_package(tinyxml2 CONFIG REQUIRED)
find_path(HALF_INCLUDE_DIR half.hpp REQUIRED)

target_include_directories(${PROJECT_NAME} PRIVATE ${HALF_INCLUDE_DIR})

# Glaze forces C++23 and /Zc:preprocessor on whatever links it, neither of which SKSE's headers survive,
# and it can't sit next to the min/max macros ITypes.h brings in. It stays in this one object library.
add_library(skee64_json OBJECT json/Json.cpp)
target_include_directories(skee64_json PRIVATE src)
target_compile_definitions(skee64_json PRIVATE NOMINMAX)
target_link_libraries(skee64_json PRIVATE glaze::glaze)
target_sources(${PROJECT_NAME} PRIVATE $<TARGET_OBJECTS:skee64_json>)

add_executable(skee64_json_test EXCLUDE_FROM_ALL json/JsonTest.cpp $<TARGET_OBJECTS:skee64_json>)
target_include_directories(skee64_json_test PRIVATE src)

target_link_libraries(
	${PROJECT_NAME}
	PRIVATE
	skse64_static
	spdlog::spdlog
	Microsoft::DirectXTex
	tinyxml2::tinyxml2
	absl::btree
	absl::flat_hash_map
	absl::flat_hash_set
	absl::node_hash_map
	shlwapi
)

# #######################################################################################################################
# # Packagging
# #######################################################################################################################
include(Package)
