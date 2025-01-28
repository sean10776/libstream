cmake_minimum_required(VERSION 3.14)

project(
    ${CMAKE_PROJECT_NAME}_test
    LANGUAGES CXX C
)

set(CMAKE_CXX_STANDARD 11)  # doctest requires C++11 or later
set(CMAKE_CXX_STANDARD_REQUIRED ON)


# ---- Include Dependencies ----
include(../cmake/CPM.cmake)

message(STATUS "Using doctest for unit testing")
CPMAddPackage("gh:doctest/doctest@2.4.11")

# ---- Create binaries ----
file(GLOB ${CMAKE_PROJECT_NAME}_TEST_SOURCES CONFIGURE_DEPENDS "./src/*.cpp")

foreach(test_file ${${CMAKE_PROJECT_NAME}_TEST_SOURCES})
    get_filename_component(test_target ${test_file} NAME_WE)
    message(STATUS "Adding test: ${test_target} (${test_file})")

    add_executable(${test_target} ${test_file})
    target_link_libraries(${test_target} LINK_PUBLIC ${CMAKE_PROJECT_NAME} doctest::doctest)
    set_target_properties(${test_target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/bin"
    )

    if (${CMAKE_PROJECT_NAME}_BUILD_SHARED_LIBS)
        add_custom_command(
            TARGET ${test_target}
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:${CMAKE_PROJECT_NAME}>
                $<TARGET_FILE_DIR:${test_target}>
        )
    endif()

    add_test(NAME ${test_target} COMMAND ${test_target})
endforeach()
