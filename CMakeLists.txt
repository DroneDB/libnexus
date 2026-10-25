cmake_minimum_required(VERSION 3.5)
project(libnexus)

# Use vcpkg manifest mode
if(DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
else()
    message(FATAL_ERROR "Please set VCPKG_ROOT environment variable to use vcpkg")
endif()

find_package(Qt5 REQUIRED COMPONENTS Core Gui)

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
# VCGLib's headers need C++17 (if constexpr); Nexus itself asks only for C++11. PUBLIC, so nxs,
# which includes the same headers, is built as C++17 too.
target_compile_features(nexus PUBLIC cxx_std_17)

#add_subdirectory(vendor/nexus/corto EXCLUDE_FROM_ALL)

SET(HEADERS
	vendor/nexus/src/nxsbuild/partition.h
	vendor/nexus/src/nxsbuild/trianglesoup.h
	vendor/nexus/src/nxsbuild/vertex_cache_optimizer.h
)

SET(SOURCES
	nxs.cpp
)

get_property(initialize_static_libjpeg GLOBAL PROPERTY USE_NEXUS_STATIC_LIBJPEG)
if (initialize_static_libjpeg STREQUAL "YES")
    add_compile_definitions(INITIALIZE_STATIC_LIBJPEG)
    message(STATUS "Libjpeg statically initialized")
else()
    message(STATUS "Libjpeg initialized as plugin")
endif()

include_directories("vendor/nexus/src/nxsbuild")

add_library(nxs SHARED ${SOURCES} ${HEADERS})
set(ADDITIONAL_LIBS "")

if (WIN32)
	list(APPEND ADDITIONAL_LIBS netapi32 wsock32 ws2_32 userenv version winmm)
endif()
# Qt and nexus are PRIVATE: nxs is a shared library that carries them (Qt is static on Linux),
# and a program linking nxs must not pull a second copy of them into its own image.
target_link_libraries(nxs PRIVATE Qt5::Core Qt5::Gui nexus ${ADDITIONAL_LIBS})

# ddb-nexus: command line front end of nxs. DroneDB runs it as a separate process instead of
# loading nxs into its own, so the Nexus / VCGLib / Qt code never shares an address space with
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

# Copy Qt plugins (imageformats, platforms, styles) to the output directory
# This is required for WebP texture support on Windows. nxs.dll and ddb-nexus.exe share the
# runtime output directory, and nxs looks for "plugins" next to the DLL.
if (WIN32)
    get_target_property(QT5_CORE_LOCATION Qt5::Core LOCATION)
    get_filename_component(QT5_BIN_DIR "${QT5_CORE_LOCATION}" DIRECTORY)
    # Qt plugins are typically in ../plugins relative to bin
    get_filename_component(QT5_PLUGINS_DIR "${QT5_BIN_DIR}/../plugins" ABSOLUTE)

    add_custom_command(TARGET nxs POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "Copying Qt plugins for libnexus..."
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:nxs>/plugins/imageformats"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:nxs>/plugins/platforms"
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:nxs>/plugins/styles"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${QT5_PLUGINS_DIR}/imageformats" "$<TARGET_FILE_DIR:nxs>/plugins/imageformats"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${QT5_PLUGINS_DIR}/platforms" "$<TARGET_FILE_DIR:nxs>/plugins/platforms"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${QT5_PLUGINS_DIR}/styles" "$<TARGET_FILE_DIR:nxs>/plugins/styles"
        COMMENT "Copying Qt plugins for WebP and other image format support"
    )
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


