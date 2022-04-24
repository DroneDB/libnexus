cmake_minimum_required(VERSION 3.1)
project(libnexus)

set (CMAKE_CXX_STANDARD 11)

option(BUILD_NXS_TEST "" OFF)

# VCGLib -- required
if (VCGDIR) # VCGDIR exists - using custom user vcglib path
	if(EXISTS ${VCGDIR})
		add_subdirectory(${VCGDIR} ${CMAKE_CURRENT_BINARY_DIR}/vcglib)
		message(STATUS "nexus - VCGLib - using custom VCGDIR path library")
	else()
		set(VCGDIR NOTFOUND)
	endif()
else()
	get_filename_component(VCGDIR "${CMAKE_CURRENT_LIST_DIR}/vendor/vcglib" ABSOLUTE)
	if(EXISTS ${VCGDIR})
		add_subdirectory(${VCGDIR} ${CMAKE_CURRENT_BINARY_DIR}/vcglib)
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
target_link_libraries(nxs PUBLIC nexus ${ADDITIONAL_LIBS})

install(TARGETS nxs EXPORT nxs DESTINATION lib)
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


