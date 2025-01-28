#
# CMake settings
#
option(${PROJECT_NAME}_BUILD_SHARED_LIBS "Build ${PROJECT_NAME} as a shared libraries" ON)
option(${PROJECT_NAME}_BUILD_HEADERS_ONLY "Build only the headers of ${PROJECT_NAME}" OFF)
option(${PROJECT_NAME}_BUILD_EXAMPLES "Build the examples of ${PROJECT_NAME}" ON)
option(${PROJECT_NAME}_BUILD_TESTS "Build the tests of ${PROJECT_NAME}" ON)

option(${PROJECT_NAME}_USE_ALT_NAMES "Use alternative names for the project, such as naming the include directory all lowercase." ON)

#
# Compiler settings
#
option(${PROJECT_NAME}_WARING_AS_ERRORS "Treat warnings as errors" ON)