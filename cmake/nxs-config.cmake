message("${CMAKE_CURRENT_LIST_DIR}")

find_library(NXS_LIBRARY
    NAMES nxs
    PATHS "${CMAKE_CURRENT_LIST_DIR}/../../../lib"
    NO_DEFAULT_PATH
    NO_CMAKE_FIND_ROOT_PATH
)
find_path(SDL2_INCLUDE_DIR
    NAMES nxs.h
    PATHS "${CMAKE_CURRENT_LIST_DIR}/include"
    PATH_SUFFIXES NXS
    NO_DEFAULT_PATH
    NO_CMAKE_FIND_ROOT_PATH
)
set(NXS_INCLUDE_DIRS ${NXS_INCLUDE_DIR})

if (UNIX)
list(APPEND NXS_LIBRARY "pthread")
list(APPEND NXS_LIBRARY "dl")
endif()

set(NXS_LIBRARIES ${NXS_LIBRARY})

mark_as_advanced(NXS_INCLUDE_DIR NXS_LIBRARY)

if(NOT TARGET NXS AND CMAKE_VERSION VERSION_GREATER 3.0.0)
    add_library(NXS INTERFACE)
    target_link_libraries(NXS INTERFACE ${NXS_LIBRARIES})
    target_include_directories(NXS INTERFACE ${NXS_INCLUDE_DIRS})
endif()