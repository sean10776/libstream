cmake_minimum_required(VERSION 3.14)

project(
    ${CMAKE_PROJECT_NAME}_example
    LANGUAGES CXX C
)

# include all example
file(GLOB ${CMAKE_PROJECT_NAME}_TEST_SOURCES CONFIGURE_DEPENDS "./src/*.cpp" "./src/*.c")
message(STATUS "example_files: ${${CMAKE_PROJECT_NAME}_TEST_SOURCES}")
foreach(example_file ${${CMAKE_PROJECT_NAME}_TEST_SOURCES})
    get_filename_component(example_target ${example_file} NAME_WE)
    message(STATUS "Adding example: ${example_target} (${example_file})")
    
    add_executable(${example_target} ${example_file})
    target_link_libraries(${example_target} LINK_PUBLIC ${CMAKE_PROJECT_NAME})
    set_target_properties(${example_target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/examples/bin"
    )
    
    if (${CMAKE_PROJECT_NAME}_BUILD_SHARED_LIBS)
        add_custom_command(
            TARGET ${example_target}
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:${CMAKE_PROJECT_NAME}>
                $<TARGET_FILE_DIR:${example_target}>
        )
    endif()
endforeach()
