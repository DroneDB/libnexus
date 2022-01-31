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
	${VCGDIR}/wrap/system/qgetopt.h
	vendor/nexus/src/nxsbuild/partition.h
	vendor/nexus/src/nxsbuild/trianglesoup.h
	vendor/nexus/src/nxsbuild/vertex_cache_optimizer.h
)

SET(SOURCES
	${VCGDIR}/wrap/system/qgetopt.cpp
	libnxs.cpp
)

include_directories("vendor/nexus/src/nxsbuild")

add_library(nxs SHARED ${SOURCES} ${HEADERS})
target_link_libraries(nxs PUBLIC nexus)

set_target_properties(nxs PROPERTIES PUBLIC_HEADER libnxs.h)

install(TARGETS nxs)

file(GLOB QT_LIBS "${Qt5_DIR}/../../*.so*")
foreach ( qt_lib ${QT_LIBS} )
    install( FILES ${qt_lib} DESTINATION lib)
endforeach()

if (BUILD_NXS_TEST)
    add_executable(nxs_test nxs_test.cpp)
    target_link_libraries(nxs_test PUBLIC nxs)
    install(TARGETS nxs_test)
endif()


