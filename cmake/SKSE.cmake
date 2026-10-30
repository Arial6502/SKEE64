include(FetchContent)

# SKSE is built from its VS solution and has no plugin-facing target, so only its sources are fetched.
# The SOURCE_SUBDIR points nowhere to stop FetchContent running the projects' own CMakeLists.
FetchContent_Declare(
	skse64
	GIT_REPOSITORY https://github.com/ianpatt/skse64
	GIT_TAG b9bfa27de9fe927daade322c5209180176604495 # v2.2.8, the last release for 1.6.1170
	SOURCE_SUBDIR none
)
FetchContent_Declare(
	xse_common
	GIT_REPOSITORY https://github.com/ianpatt/common
	GIT_TAG 64e233c096735551f6ac9a773726a8a3960e46cd
	SOURCE_SUBDIR none
)
FetchContent_MakeAvailable(skse64 xse_common)

# Provides RUNTIME_VERSION_PACKED. The sln's sheets/Runtime.props is stale (1.6.323), this file is what SKSE tags.
include("${skse64_SOURCE_DIR}/cmake/versioning.cmake")

include("${xse_common_SOURCE_DIR}/cmake/sourcelist.cmake")
list(TRANSFORM sources PREPEND "${xse_common_SOURCE_DIR}/")
set(SKSE_SOURCES ${sources})

include("${skse64_SOURCE_DIR}/skse64_common/cmake/sourcelist.cmake")
list(TRANSFORM sources PREPEND "${skse64_SOURCE_DIR}/skse64_common/")
list(APPEND SKSE_SOURCES ${sources})

# skse64.cpp holds the runtime DLL's DllMain, which a static library would hand to the plugin.
include("${skse64_SOURCE_DIR}/skse64/cmake/sourcelist.cmake")
list(REMOVE_ITEM sources skse64.cpp)
list(TRANSFORM sources PREPEND "${skse64_SOURCE_DIR}/skse64/")
list(APPEND SKSE_SOURCES ${sources})

# Same settings as the sln's Release_Lib_VC142 configuration and its sheets/*.props.
add_library(skse64_static STATIC ${SKSE_SOURCES})
target_include_directories(skse64_static PUBLIC "${skse64_SOURCE_DIR}" "${xse_common_SOURCE_DIR}")
target_compile_definitions(
	skse64_static
	PRIVATE
	_WINDOWS
	_USRDLL
	SKSE64_EXPORTS
	RUNTIME
	RUNTIME_VERSION=${RUNTIME_VERSION_PACKED}
	XBYAK_NO_OP_NAMES
)
target_compile_options(skse64_static PRIVATE /W0 /FIcommon/IPrefix.h)
# SKSE predates C++20 and relies on its implied /permissive- being off.
set_target_properties(skse64_static PROPERTIES CXX_STANDARD 17)
# This file defines its own round(). /MD's dllimport declaration would turn it into a DLL export, and it clashes with the CRT's.
set_source_files_properties("${skse64_SOURCE_DIR}/skse64/ScaleformExtendedData.cpp" PROPERTIES COMPILE_DEFINITIONS "_ACRTIMP=;round=skse_round")
