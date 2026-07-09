cmake_minimum_required(VERSION 3.1.0)

set(PROJECT_NAME nanovg)

project(${PROJECT_NAME} C)

find_package(OpenGL REQUIRED)
message(STATUS "OPENGL_FOUND:= ${OPENGL_FOUND}")
message(STATUS "OPENGL_INCLUDE_DIR:= ${OPENGL_INCLUDE_DIR}")

set(GLFW_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../glfw" CACHE PATH "Path to GLFW source")

# Add GLFW subdirectory with a binary directory
add_subdirectory(${GLFW_SRC_DIR} ${CMAKE_CURRENT_BINARY_DIR}/glfw)

# Fontstash support
option(NANOVG_USE_FONTSTASH "Use fontstash with FreeType" ON)
option(NANOVG_BUILD_EXAMPLES "Build NanoVG example applications" ON)
set(NANOVG_LIBS "")

if (NANOVG_USE_FONTSTASH)
    add_definitions(-DFONS_USE_FREETYPE)
    find_package(Freetype REQUIRED)
    list(APPEND NANOVG_LIBS ${FREETYPE_LIBRARIES})
    include_directories(${FREETYPE_INCLUDE_DIRS})

    find_package(PkgConfig)
    pkg_check_modules(HARFBUZZ harfbuzz)
    if (HARFBUZZ_FOUND)
        message(STATUS "HARFBUZZ_LIBRARIES (pkg-config): ${HARFBUZZ_LIBRARIES}")
        message(STATUS "HARFBUZZ_INCLUDE_DIRS (pkg-config): ${HARFBUZZ_INCLUDE_DIRS}")
        list(APPEND NANOVG_LIBS ${HARFBUZZ_LIBRARIES})
        include_directories(${HARFBUZZ_INCLUDE_DIRS})
    endif()
endif()

set(SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
file(GLOB SRCS ${SRC_DIR}/*.c)
list(FILTER SRCS EXCLUDE REGEX "imagestash_test\\.c$") # Exclude imagestash_test.c from library
file(GLOB HDRS ${SRC_DIR}/*.h)

add_library(${PROJECT_NAME} ${SRCS})
target_include_directories(nanovg PUBLIC
    ${SRC_DIR}
    ${GLFW_SRC_DIR}/include
)
if(APPLE)
    target_include_directories(nanovg PUBLIC
        ${OPENGL_INCLUDE_DIR}/Headers
    )
else()
    if(OPENGL_INCLUDE_DIR)
        target_include_directories(nanovg PUBLIC ${OPENGL_INCLUDE_DIR})
    endif()
endif()

# Link necessary macOS frameworks and fontstash libraries for nanovg
if(APPLE)
    target_link_libraries(nanovg PUBLIC
        "-framework Cocoa"
        "-framework IOKit"
        "-framework CoreFoundation"
    )
endif()
target_link_libraries(nanovg PUBLIC ${NANOVG_LIBS} OpenGL::GL)

# Suppress implicit function declaration warnings for C99
if(APPLE)
    target_compile_options(nanovg PRIVATE -Wno-implicit-function-declaration)
endif()

if(UNIX AND NOT APPLE)
    # nanovg.c uses GL extension entry points from the utils impl.
    target_link_libraries(nanovg PUBLIC m)
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(NANOVG_BUILD_EXAMPLES)
    find_package(OpenGL REQUIRED)

    # Optional GLEW — useful on some Linux core-profile setups.
    find_package(GLEW)
    if(NOT GLEW_FOUND AND PKG_CONFIG_FOUND)
        pkg_check_modules(GLEW glew)
    endif()

    add_executable(example_gl3
        example/example_gl3.c
        example/demo.c
        example/perf.c
    )
    target_include_directories(example_gl3 PRIVATE
        ${SRC_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/example
        ${GLFW_SRC_DIR}/include
    )
    # Absolute resource path so the binary works from any working directory.
    target_compile_definitions(example_gl3 PRIVATE
        "DEMO_RESOURCE_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/example\""
    )

    set(EXAMPLE_GL3_LIBS nanovg glfw OpenGL::GL)
    if(UNIX AND NOT APPLE)
        list(APPEND EXAMPLE_GL3_LIBS m)
    endif()
    if(GLEW_FOUND)
        target_compile_definitions(example_gl3 PRIVATE NANOVG_GLEW)
        if(TARGET GLEW::GLEW)
            list(APPEND EXAMPLE_GL3_LIBS GLEW::GLEW)
        else()
            list(APPEND EXAMPLE_GL3_LIBS ${GLEW_LIBRARIES})
            target_include_directories(example_gl3 PRIVATE ${GLEW_INCLUDE_DIRS})
        endif()
    elseif(GLEW_LIBRARIES)
        target_compile_definitions(example_gl3 PRIVATE NANOVG_GLEW)
        list(APPEND EXAMPLE_GL3_LIBS ${GLEW_LIBRARIES})
        target_include_directories(example_gl3 PRIVATE ${GLEW_INCLUDE_DIRS})
    endif()

    target_link_libraries(example_gl3 PRIVATE ${EXAMPLE_GL3_LIBS})

    # Convenience: place the binary at the build-tree root.
    set_target_properties(example_gl3 PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )
endif()

install(TARGETS ${PROJECT_NAME}
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        RUNTIME DESTINATION bin
        INCLUDES DESTINATION include/${PROJECT_NAME}
)

install(FILES ${HDRS} DESTINATION include/${PROJECT_NAME}/)

if(NANOVG_BUILD_EXAMPLES)
    install(TARGETS example_gl3 RUNTIME DESTINATION bin)
endif()
