cmake_minimum_required(VERSION 3.21)
cmake_policy(VERSION 3.21...3.31)

# #######################################################################################################################
# # Project Definition
# #######################################################################################################################

project(
	skee64
	VERSION 0.4.19.17
	DESCRIPTION "Skyrim 64 Engine Extender"
	LANGUAGES CXX
)

# Not Exposed By CMake, Manually Set Here for use by version.rc.in
set(PROJECT_COPYRIGHT "Copyright (C) 2024")

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
find_package(jsoncpp CONFIG REQUIRED)
find_package(directxtex CONFIG REQUIRED)
find_package(tinyxml2 CONFIG REQUIRED)
find_path(HALF_INCLUDE_DIR half.hpp REQUIRED)

target_include_directories(${PROJECT_NAME} PRIVATE ${HALF_INCLUDE_DIR})

target_link_libraries(
	${PROJECT_NAME}
	PRIVATE
	skse64_static
	spdlog::spdlog
	JsonCpp::JsonCpp
	Microsoft::DirectXTex
	tinyxml2::tinyxml2
	shlwapi
)

# #######################################################################################################################
# # Packagging
# #######################################################################################################################
include(Package)
