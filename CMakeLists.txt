cmake_minimum_required(VERSION 3.14)

# ---- Project ----

project(
    stream
    VERSION 0.0.1
    DESCRIPTION ""
    HOMEPAGE_URL ""
    LANGUAGES CXX C
)

# ---- Set C CXX standard ----

if (NOT CMAKE_C_STANDARD)
    set(CMAKE_C_STANDARD 11)
endif()
set(CMAKE_C_STANDARD_REQUIRED ON)

if (NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 14)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- Include guards ----

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(
        FATAL_ERROR
        "In-source builds not allowed. Please make a new directory (called a build directory) and run CMake from there."
    )
endif()

# ---- Include Project Settings ----

include(cmake/StandardSettings.cmake)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

#this will allow to use same _DEBUG macro available in both Linux as well as Windows - MSCV environment. Easy to put Debug specific code.
if (UNIX)
    add_compile_options("$<$<CONFIG:DEBUG>:-D_DEBUG>")
endif ()

# ---- Set Project Aliases ----

if (${PROJECT_NAME}_USE_ALT_NAMES)
    string(TOLOWER ${PROJECT_NAME} PROJECT_NAME_LOWER)
    string(TOUPPER ${PROJECT_NAME} PROJECT_NAME_UPPER)
else()
    set(PROJECT_NAME_LOWER ${PROJECT_NAME})
    set(PROJECT_NAME_UPPER ${PROJECT_NAME})
endif()

# ---- Add dependencies via CPM ----
# see https://github.com/cpm-cmake/CPM.cmake for more info

include(cmake/CPM.cmake)

# ---- Add source files ----
file(GLOB_RECURSE ${PROJECT_NAME}_SOURCES CONFIGURE_DEPENDS "src/*.cpp" "src/*.c")
file(GLOB_RECURSE ${PROJECT_NAME}_HEADERS CONFIGURE_DEPENDS "src/*.hpp" "src/*.h")
message(STATUS "Sources: ${${PROJECT_NAME}_SOURCES}")
message(STATUS "Headers: ${${PROJECT_NAME}_HEADERS}")

# ---- Create library ----
if(${PROJECT_NAME}_BUILD_HEADERS_ONLY)
    add_library(${PROJECT_NAME} INTERFACE)
elseif(${PROJECT_NAME}_BUILD_SHARED_LIBS)
    add_library(${PROJECT_NAME} SHARED ${${PROJECT_NAME}_SOURCES} ${${PROJECT_NAME}_HEADERS})
else()
    add_library(${PROJECT_NAME} STATIC ${${PROJECT_NAME}_SOURCES} ${${PROJECT_NAME}_HEADERS})
endif()
target_include_directories(${PROJECT_NAME} PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)

# ---- Link dependencies ----
if (WIN32)
    target_link_libraries(${PROJECT_NAME} wsock32 winmm Ws2_32)
elseif(UNIX)
    target_link_libraries(${PROJECT_NAME} m rt pthread)
endif()

# ---- Add Definitions ----
if (WIN32)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
endif()
include(cmake/CompilerWarnings.cmake)
set_project_warnings(${PROJECT_NAME})

# ---- Set Output paths ----
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    DEBUG_POSTFIX _d
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    PUBLIC_HEADER "${${PROJECT_NAME}_HEADERS}"
)

# ---- Create examples ----
if(${PROJECT_NAME}_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ---- Create tests ----
if(${PROJECT_NAME}_BUILD_TESTS)
    enable_testing() # Note: this enables must call before add_subdirectory(tests)
    add_subdirectory(tests)
endif()

# ---- Create an installable target ----
# this allows users to install and find the library via `find_package()`.
string(TOLOWER ${PROJECT_NAME}/version.h VERSION_HEADER_LOCATION)

CPMAddPackage("gh:TheLartians/PackageProject.cmake@1.11.2")

packageProject(
    NAME ${PROJECT_NAME}
    VERSION ${PROJECT_VERSION}
    NAMESPACE ${PROJECT_NAME}
    BINARY_DIR ${PROJECT_BINARY_DIR}
    INCLUDE_DIR ${PROJECT_SOURCE_DIR}/include
    INCLUDE_DESTINATION include/${PROJECT_NAME}-${PROJECT_VERSION}
    VERSION_HEADER "${VERSION_HEADER_LOCATION}"
    COMPATIBILITY SameMajorVersion
)