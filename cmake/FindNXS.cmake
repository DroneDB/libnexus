# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

#[=======================================================================[.rst:
FindNxs
-------

Find libnxs, the libnexus library

#]=======================================================================]

find_path(NXS_NXS_INCLUDE_DIR libnxs.h PATH_SUFFIXES include)
mark_as_advanced(NXS_NXS_INCLUDE_DIR)

list(APPEND NXS_NAMES nxs libnxs)
list(APPEND NXS_NAMES_DEBUG nxsd libnxsd)

# set(_NXS_VERSION_SUFFIXES 17 16 15 14 12)
# if (NXS_FIND_VERSION MATCHES "^([0-9]+)\\.([0-9]+)(\\..*)?$")
# set(_NXS_VERSION_SUFFIX_MIN "${CMAKE_MATCH_1}${CMAKE_MATCH_2}")
# if (NXS_FIND_VERSION_EXACT)
#     set(_NXS_VERSION_SUFFIXES ${_NXS_VERSION_SUFFIX_MIN})
# else ()
#     string(REGEX REPLACE
#         "${_NXS_VERSION_SUFFIX_MIN}.*" "${_NXS_VERSION_SUFFIX_MIN}"
#         _NXS_VERSION_SUFFIXES "${_NXS_VERSION_SUFFIXES}")
# endif ()
unset(_NXS_VERSION_SUFFIX_MIN)
# endif ()
# foreach(v IN LISTS _NXS_VERSION_SUFFIXES)
# list(APPEND NXS_NAMES NXS${v} libNXS${v} libNXS${v}_static)
# list(APPEND NXS_NAMES_DEBUG NXS${v}d libNXS${v}d libNXS${v}_staticd)
# endforeach()
unset(_NXS_VERSION_SUFFIXES)

# Set by select_library_configurations(), but we want the one from
# find_package_handle_standard_args() below.
unset(NXS_FOUND)
if(NOT NXS_LIBRARY)
    find_library(NXS_LIBRARY_RELEASE NAMES ${NXS_NAMES} NAMES_PER_DIR)
    find_library(NXS_LIBRARY_DEBUG NAMES ${NXS_NAMES_DEBUG} NAMES_PER_DIR)
    include(${CMAKE_CURRENT_LIST_DIR}/SelectLibraryConfigurations.cmake)
    select_library_configurations(NXS)
    mark_as_advanced(NXS_LIBRARY_RELEASE NXS_LIBRARY_DEBUG)
endif()

if (NXS_LIBRARY AND NXS_NXS_INCLUDE_DIR)
    set(NXS_INCLUDE_DIRS ${NXS_NXS_INCLUDE_DIR} ${ZLIB_INCLUDE_DIR} )
    set(NXS_INCLUDE_DIR ${NXS_INCLUDE_DIRS} ) # for backward compatibility
    set(NXS_LIBRARIES ${NXS_LIBRARY} ${ZLIB_LIBRARY})
    if((CMAKE_SYSTEM_NAME STREQUAL "Linux") AND
        ("${NXS_LIBRARY}" MATCHES "\\${CMAKE_STATIC_LIBRARY_SUFFIX}$"))
    list(APPEND NXS_LIBRARIES m)
    endif()

    if(NOT TARGET NXS::NXS)
    add_library(NXS::NXS UNKNOWN IMPORTED)
    set_target_properties(NXS::NXS PROPERTIES
        INTERFACE_COMPILE_DEFINITIONS "${_NXS_COMPILE_DEFINITIONS}"
        INTERFACE_INCLUDE_DIRECTORIES "${NXS_INCLUDE_DIRS}"
        INTERFACE_LINK_LIBRARIES ZLIB::ZLIB)
    if((CMAKE_SYSTEM_NAME STREQUAL "Linux") AND
        ("${NXS_LIBRARY}" MATCHES "\\${CMAKE_STATIC_LIBRARY_SUFFIX}$"))
        set_property(TARGET NXS::NXS APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES m)
    endif()

    if(EXISTS "${NXS_LIBRARY}")
        set_target_properties(NXS::NXS PROPERTIES
        IMPORTED_LINK_INTERFACE_LANGUAGES "C"
        IMPORTED_LOCATION "${NXS_LIBRARY}")
    endif()
    if(EXISTS "${NXS_LIBRARY_RELEASE}")
        set_property(TARGET NXS::NXS APPEND PROPERTY
        IMPORTED_CONFIGURATIONS RELEASE)
        set_target_properties(NXS::NXS PROPERTIES
        IMPORTED_LINK_INTERFACE_LANGUAGES_RELEASE "C"
        IMPORTED_LOCATION_RELEASE "${NXS_LIBRARY_RELEASE}")
    endif()
    if(EXISTS "${NXS_LIBRARY_DEBUG}")
        set_property(TARGET NXS::NXS APPEND PROPERTY
        IMPORTED_CONFIGURATIONS DEBUG)
        set_target_properties(NXS::NXS PROPERTIES
        IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "C"
        IMPORTED_LOCATION_DEBUG "${NXS_LIBRARY_DEBUG}")
    endif()
    endif()

    unset(_NXS_COMPILE_DEFINITIONS)
endif ()

if (NXS_NXS_INCLUDE_DIR AND EXISTS "${NXS_NXS_INCLUDE_DIR}/NXS.h")
    file(STRINGS "${NXS_NXS_INCLUDE_DIR}/NXS.h" NXS_version_str REGEX "^#define[ \t]+NXS_LIBNXS_VER_STRING[ \t]+\".+\"")

    string(REGEX REPLACE "^#define[ \t]+NXS_LIBNXS_VER_STRING[ \t]+\"([^\"]+)\".*" "\\1" NXS_VERSION_STRING "${NXS_version_str}")
    unset(NXS_version_str)
endif ()

include(${CMAKE_CURRENT_LIST_DIR}/FindPackageHandleStandardArgs.cmake)
find_package_handle_standard_args(NXS
                                  REQUIRED_VARS NXS_LIBRARY NXS_NXS_INCLUDE_DIR
                                  VERSION_VAR NXS_VERSION_STRING)
