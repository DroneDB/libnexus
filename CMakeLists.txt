cmake_minimum_required(VERSION 3.10)
project(libnexus)

# Use vcpkg manifest mode
if(DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
else()
    message(FATAL_ERROR "Please set VCPKG_ROOT environment variable to use vcpkg")
endif()

set (CMAKE_CXX_STANDARD 14)

option(BUILD_NXS_TEST "" OFF)

# VCGLib -- required. It is header only and only compiled into nxs, so it is added without its
# install rules (EXCLUDE_FROM_ALL) and without OpenMP: Nexus builds its nodes on one thread, and
# with OpenMP VCGLib would hand Eigen's matrix products to a thread team and nxs would need the
# OpenMP runtime next to it.
set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
set(VCG_USE_OPENMP OFF)
if (VCGDIR) # VCGDIR exists - using custom user vcglib path
	if(EXISTS ${VCGDIR})
		add_subdirectory(${VCGDIR} ${CMAKE_CURRENT_BINARY_DIR}/vcglib EXCLUDE_FROM_ALL)
		message(STATUS "nexus - VCGLib - using custom VCGDIR path library")
	else()
		set(VCGDIR NOTFOUND)
	endif()
else()
	get_filename_component(VCGDIR "${CMAKE_CURRENT_LIST_DIR}/vendor/vcglib" ABSOLUTE)
	if(EXISTS ${VCGDIR})
		add_subdirectory(${VCGDIR} ${CMAKE_CURRENT_BINARY_DIR}/vcglib EXCLUDE_FROM_ALL)
		message(STATUS "nexus - VCGLib - using bundled source")
	else()
		set(VCGDIR NOTFOUND)
	endif()
endif()
set(VCGDIR "${VCGDIR}")

if(NOT VCGDIR)
	message(FATAL_ERROR "VCGLib not found.")
endif()

add_subdirectory("vendor/nexus")

#add_subdirectory(vendor/nexus/corto EXCLUDE_FROM_ALL)

SET(HEADERS
	vendor/nexus/src/nxsbuild/partition.h
	vendor/nexus/src/nxsbuild/trianglesoup.h
	vendor/nexus/src/nxsbuild/vertex_cache_optimizer.h
)

SET(SOURCES
	nxs.cpp
)

include_directories("vendor/nexus/src/nxsbuild")

add_library(nxs SHARED ${SOURCES} ${HEADERS})
set(ADDITIONAL_LIBS "")

if (WIN32)
	list(APPEND ADDITIONAL_LIBS netapi32 wsock32 ws2_32 userenv version winmm)
endif()
# nexus is PRIVATE: nxs is a shared library that carries it (and the image libraries it uses),
# and a program linking nxs must not pull a second copy of them into its own image.
target_link_libraries(nxs PRIVATE nexus ${ADDITIONAL_LIBS})

# ddb-nexus: command line front end of nxs. DroneDB runs it as a separate process instead of
# loading nxs into its own, so the Nexus / VCGLib code never shares an address space with
# the host. The version it reports is set by the parent project (DroneDB's APP_VERSION).
if (NOT DEFINED DDB_NEXUS_VERSION)
	if (DEFINED APP_VERSION)
		set(DDB_NEXUS_VERSION "${APP_VERSION}")
	else()
		set(DDB_NEXUS_VERSION "unknown")
	endif()
endif()

find_package(Threads REQUIRED)
add_executable(ddb-nexus ddb_nexus_main.cpp)
target_compile_definitions(ddb-nexus PRIVATE DDB_NEXUS_VERSION="${DDB_NEXUS_VERSION}")
target_include_directories(ddb-nexus PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(ddb-nexus PRIVATE nxs Threads::Threads)
if (UNIX AND NOT APPLE)
	# libnxs.so next to the program (zip / build tree) or in ../lib (an installed prefix)
	set_target_properties(ddb-nexus PROPERTIES
		BUILD_WITH_INSTALL_RPATH ON
		INSTALL_RPATH "$ORIGIN;$ORIGIN/../lib")
endif()

install(TARGETS nxs EXPORT nxs DESTINATION lib)
install(TARGETS ddb-nexus RUNTIME DESTINATION bin)
install(FILES nxs.h DESTINATION include)
install(FILES cmake/nxs-config.cmake DESTINATION share/cmake/nxs)

if (BUILD_NXS_TEST)
    add_executable(nxs_test nxs_test.cpp)
	if (UNIX)
    	target_link_libraries(nxs_test PUBLIC nxs pthread dl)
	else()
		target_link_libraries(nxs_test PUBLIC nxs)
	endif()
    install(TARGETS nxs_test)
endif()


