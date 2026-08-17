cmake_minimum_required (VERSION 3.13..3.18)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE INTERNAL "")
project(nanogui
  DESCRIPTION
    "NanoGUI"
  LANGUAGES
    CXX C
)

if (NOT IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/ext/glfw/src")
  message(FATAL_ERROR "The NanoGUI dependency repositories (GLFW, etc.) are missing! "
    "You probably did not clone the project with --recursive. It is possible to recover "
    "by calling \"git submodule update --init --recursive\"")
endif()

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "Setting build type to 'Release' as none was specified.")
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release"
    "MinSizeRel" "RelWithDebInfo")
endif()
string(TOUPPER "${CMAKE_BUILD_TYPE}" U_CMAKE_BUILD_TYPE)

set(NANOGUI_MASTER_PROJECT OFF)
if (${CMAKE_CURRENT_SOURCE_DIR} STREQUAL ${CMAKE_SOURCE_DIR})
  set(NANOGUI_MASTER_PROJECT ON)
endif()

if (WIN32)
  set(NANOGUI_BUILD_GLAD_DEFAULT ON)
else()
  set(NANOGUI_BUILD_GLAD_DEFAULT OFF)
endif()

set(NANOGUI_BUILD_SHARED_DEFAULT ON)
set(NANOGUI_BUILD_PYTHON_DEFAULT OFF)
set(NANOGUI_BUILD_GLFW_DEFAULT ON)

if (CMAKE_CXX_COMPILER MATCHES "/em\\+\\+(-[a-zA-Z0-9.])?$")
  set(CMAKE_CXX_COMPILER_ID "Emscripten")
  set(NANOGUI_BUILD_SHARED_DEFAULT OFF)
  set(NANOGUI_BUILD_PYTHON_DEFAULT OFF)
  set(NANOGUI_BUILD_GLAD_DEFAULT   OFF)
  set(NANOGUI_BUILD_GLFW_DEFAULT   OFF)

  set(CMAKE_STATIC_LIBRARY_SUFFIX ".bc")
  set(CMAKE_EXECUTABLE_SUFFIX ".bc")
  set(CMAKE_CXX_CREATE_STATIC_LIBRARY "<CMAKE_CXX_COMPILER> -o <TARGET> <LINK_FLAGS> <OBJECTS>")
  if (U_CMAKE_BUILD_TYPE MATCHES REL)
    add_compile_options(-O3 -DNDEBUG)
  endif()
endif()

option(NANOGUI_BUILD_EXAMPLES            "Build NanoGUI example application?" ON)
option(NANOGUI_BUILD_SHARED              "Build NanoGUI as a shared library?" ${NANOGUI_BUILD_SHARED_DEFAULT})
option(NANOGUI_BUILD_PYTHON              "Build a Python plugin for NanoGUI?" ${NANOGUI_BUILD_PYTHON_DEFAULT})
option(NANOGUI_BUILD_GLAD                "Build GLAD OpenGL loader library? (needed on Windows)" ${NANOGUI_BUILD_GLAD_DEFAULT})
option(NANOGUI_BUILD_GLFW                "Build GLFW?" ${NANOGUI_BUILD_GLFW_DEFAULT})
option(NANOGUI_INSTALL                   "Install NanoGUI on `make install`?" ON)
option(NANOGUI_USE_FREETYPE				 "Build with Freetype2" ON)
option(NANOGUI_USE_QUICKJS               "Build with QuickJS-NG scripting engine" OFF)
# Native Wayland windowing (Linux). Enables real touchpad pinch via
# zwp_pointer_gestures_v1. When OFF, GLFW uses X11/XWayland and pinch falls
# back to Ctrl+wheel (libinput often maps pinch that way under X11).
option(NANOGUI_USE_WAYLAND               "Use native Wayland + pinch gestures (Linux)" OFF)

if (NOT NANOGUI_BACKEND)
  if (CMAKE_SYSTEM_PROCESSOR MATCHES "armv7" OR
      CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64" OR
      CMAKE_CXX_COMPILER MATCHES "/em\\+\\+(-[a-zA-Z0-9.])?$")
    set(NANOGUI_BACKEND_DEFAULT "GLES 2")
  elseif (APPLE)
    set(NANOGUI_BACKEND_DEFAULT "Metal")
  else()
    set(NANOGUI_BACKEND_DEFAULT "OpenGL")
  endif()

  set(NANOGUI_BACKEND ${NANOGUI_BACKEND_DEFAULT} CACHE STRING "Choose the backend used for rendering (OpenGL/GLES 2/GLES 3/Metal)" FORCE)
endif()

set_property(CACHE NANOGUI_BACKEND PROPERTY STRINGS "OpenGL" "GLES 2" "GLES 3" "Metal")

# Required libraries, flags, and include files for compiling and linking against nanogui (all targets)
set(NANOGUI_EXTRA_INCS "")

include(CheckCXXCompilerFlag)
include(CheckCXXSourceRuns)

macro(CHECK_CXX_COMPILER_AND_LINKER_FLAGS _RESULT _CXX_FLAGS _LINKER_FLAGS)
  set(CMAKE_REQUIRED_FLAGS ${_CXX_FLAGS})
  set(CMAKE_REQUIRED_LIBRARIES ${_LINKER_FLAGS})
  set(CMAKE_REQUIRED_QUIET TRUE)
  check_cxx_source_runs("#include <iostream>\nint main(int argc, char **argv) { std::cout << \"test\"; return 0; }" ${_RESULT})
  set(CMAKE_REQUIRED_FLAGS "")
  set(CMAKE_REQUIRED_LIBRARIES "")
endmacro()

if (NANOGUI_BACKEND MATCHES "(OpenGL|GLES 2|GLES 3)")
  list(APPEND NANOGUI_EXTRA
    src/texture_gl.cpp src/shader_gl.cpp
    src/renderpass_gl.cpp src/opengl.cpp
    src/opengl_check.h
  )
endif()

if (NANOGUI_BACKEND STREQUAL "OpenGL")
  set(NANOGUI_BACKEND_DEFS -DNANOGUI_USE_OPENGL)
  set(NANOGUI_GLOB "resources/*.gl")
  message(STATUS "NanoGUI: using OpenGL backend.")
elseif (NANOGUI_BACKEND STREQUAL "GLES 2")
  set(NANOGUI_BACKEND_DEFS -DNANOGUI_USE_GLES -DNANOGUI_GLES_VERSION=2)
  set(NANOGUI_GLOB "resources/*.gles")
  message(STATUS "NanoGUI: using GLES 2 backend.")
elseif (NANOGUI_BACKEND STREQUAL "GLES 3")
  set(NANOGUI_BACKEND_DEFS -DNANOGUI_USE_GLES -DNANOGUI_GLES_VERSION=3)
  set(NANOGUI_GLOB "resources/*.gles")
  message(STATUS "NanoGUI: using GLES 3 backend.")
elseif (NANOGUI_BACKEND STREQUAL "Metal")
  set(NANOGUI_BACKEND_DEFS -DNANOGUI_USE_METAL)
  list(APPEND NANOGUI_EXTRA
    ext/nanovg_metal/src/nanovg_mtl.m ext/nanovg_metal/src/nanovg_mtl.h
    src/texture_metal.mm src/shader_metal.mm src/renderpass_metal.mm
  )
  set(NANOGUI_GLOB "resources/*.metal")
  include_directories(ext/nanovg_metal/src)
  message(STATUS "NanoGUI: using Metal backend.")
endif()

# Shared library mode: add dllimport/dllexport flags to all symbols
if (NANOGUI_BUILD_SHARED)
  message(STATUS "NanoGUI: building shared library.")
else()
  message(STATUS "NanoGUI: building static library.")
endif()

if (MSVC)
  # Disable annoying MSVC warnings (all targets)
  add_definitions(/D "_CRT_SECURE_NO_WARNINGS")

  # Parallel build on MSVC (all targets)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /MP")
endif()

# Compile with compiler warnings turned on
if (MSVC)
  if (CMAKE_CXX_FLAGS MATCHES "/W[0-4]")
    string(REGEX REPLACE "/W[0-4]" "/W4" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /W4")
  endif()
elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wno-unused-parameter -Wno-unused-function")
endif()

# Use C++17, visibility=hidden by default, interprocedural optimization
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

# Prefer libc++ in conjunction with Clang
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND NOT CMAKE_CXX_FLAGS MATCHES "-stdlib=libc\\+\\+")
  CHECK_CXX_COMPILER_AND_LINKER_FLAGS(HAS_LIBCPP "-stdlib=libc++" "-stdlib=libc++")
  if (HAS_LIBCPP)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++ -D_LIBCPP_VERSION")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -stdlib=libc++")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -stdlib=libc++")
    message(STATUS "NanoGUI: using libc++.")
  else()
    CHECK_CXX_COMPILER_AND_LINKER_FLAGS(HAS_LIBCPP_AND_CPPABI "-stdlib=libc++" "-stdlib=libc++ -lc++abi")
    if (HAS_LIBCPP_AND_CPPABI)
      set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++ -D_LIBCPP_VERSION")
      set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -stdlib=libc++ -lc++abi")
      set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -stdlib=libc++ -lc++abi")
      message(STATUS "NanoGUI: using libc++ and libc++abi.")
    else()
      message(FATAL_ERROR "When Clang is used to compile NanoGUI, libc++ must be available -- GCC's libstdc++ is not supported! (please insteall the libc++ development headers, provided e.g. by the packages 'libc++-dev' and 'libc++abi-dev' on Debian/Ubuntu).")
    endif()
  endif()
endif()

# Compile GLFW (official 3.4+: single `glfw` target, multi-backend).
if (NANOGUI_BUILD_GLFW)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL " " FORCE)
  set(GLFW_BUILD_TESTS OFF CACHE BOOL " " FORCE)
  set(GLFW_BUILD_DOCS OFF CACHE BOOL " " FORCE)
  set(GLFW_INSTALL OFF CACHE BOOL " " FORCE)
  # Embed as static PIC so we can link into shared libnanogui without a
  # separate libglfw.so. Official GLFW 3.4 drops the old glfw_objects target.
  set(GLFW_LIBRARY_TYPE "STATIC" CACHE STRING " " FORCE)

  if (UNIX AND NOT APPLE)
    # Prefer building both backends when Wayland is enabled so runtime can
    # select via GLFW_PLATFORM / WAYLAND_DISPLAY (libdecor used on GNOME).
    set(GLFW_BUILD_X11 ON CACHE BOOL "Build GLFW X11 support" FORCE)
    if (NANOGUI_USE_WAYLAND)
      set(GLFW_BUILD_WAYLAND ON CACHE BOOL "Build GLFW Wayland support" FORCE)
      message(STATUS "NanoGUI: enabling GLFW Wayland+X11 (libdecor decorations).")
    else()
      set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "Build GLFW Wayland support" FORCE)
      message(STATUS "NanoGUI: enabling GLFW X11 backend only.")
    endif()
  endif()

  if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Quench annoying deprecation warnings when compiling GLFW on OSX
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-deprecated-declarations")
  endif()

  add_subdirectory(ext/glfw)
  set_property(TARGET glfw PROPERTY POSITION_INDEPENDENT_CODE ON)
  set_target_properties(glfw PROPERTIES EXCLUDE_FROM_ALL 1 EXCLUDE_FROM_DEFAULT_BUILD 1)
  # Link static glfw into nanogui (via NANOGUI_LIBS below).
  list(APPEND NANOGUI_LIBS glfw)

  mark_as_advanced(
    GLFW_BUILD_DOCS GLFW_BUILD_EXAMPLES GLFW_BUILD_TESTS GLFW_INSTALL
    GLFW_LIBRARY_TYPE GLFW_BUILD_WAYLAND GLFW_BUILD_X11 GLFW_BUILD_WIN32
    GLFW_BUILD_COCOA GLFW_USE_HYBRID_HPG USE_MSVC_RUNTIME_LIBRARY_DLL)
endif()

if (NANOGUI_BUILD_GLAD)
  # Build and include GLAD on Windows
  list(APPEND NANOGUI_EXTRA
    ext/glad/src/glad.c
    ext/glad/include/glad/glad.h
    ext/glad/include/KHR/khrplatform.h)
  if (MSVC)
    set_source_files_properties(ext/glad/src/glad.c
      PROPERTIES COMPILE_FLAGS /wd4055)
  endif()
endif()

if (${CMAKE_SYSTEM_NAME} MATCHES "BSD")
  include_directories(/usr/local/include)
  link_directories(/usr/local/lib)
  if (${CMAKE_SYSTEM_NAME} MATCHES "OpenBSD")
    include_directories(/usr/X11R6/include/)
    link_directories(/usr/X11R6/lib)
  endif()
endif()

# Required core libraries on various platforms
if (${CMAKE_CXX_COMPILER_ID} MATCHES "Emscripten")
  # No dependencies
elseif (WIN32)
  list(APPEND NANOGUI_LIBS opengl32)
elseif (APPLE)
  find_library(cocoa_library Cocoa)
  find_library(iokit_library IOKit)
  list(APPEND NANOGUI_LIBS ${cocoa_library} ${iokit_library} m)
  mark_as_advanced(cocoa_library iokit_library)

  if (NANOGUI_BACKEND STREQUAL "OpenGL")
    find_library(corevideo_library CoreVideo)
    find_library(opengl_library OpenGL)

    # Disable OpenGL deprecation warnings
    add_definitions(-DGL_SILENCE_DEPRECATION)
    list(APPEND NANOGUI_LIBS ${opengl_library} ${corevideo_library})
    mark_as_advanced(opengl_library corevideo_library)
  else()
    find_library(metal_library Metal)
    find_library(quartzcore_library QuartzCore)
    list(APPEND NANOGUI_LIBS ${metal_library} ${quartzcore_library})
    mark_as_advanced(metal_library quartzcore_library)
  endif()
  list(APPEND NANOGUI_EXTRA src/darwin.mm)
elseif (CMAKE_SYSTEM MATCHES "Linux" OR CMAKE_SYSTEM_NAME MATCHES "BSD")
  # X11 libs still useful when GLFW_BUILD_X11 is on (default). GLFW 3.4
  # dlopens Wayland libs at runtime, but we still need X11 at link time for
  # the X11 backend object code when both backends are compiled in.
  list(APPEND NANOGUI_LIBS X11 Xcursor Xinerama Xrandr Xi)
  list(APPEND NANOGUI_LIBS pthread)
  if (NANOGUI_BACKEND STREQUAL "OpenGL")
    list(APPEND NANOGUI_LIBS GL)
  elseif (NANOGUI_BACKEND STREQUAL "GLES 2")
    list(APPEND NANOGUI_LIBS GLESv2)
  elseif (NANOGUI_BACKEND STREQUAL "GLES 3")
    list(APPEND NANOGUI_LIBS GLESv3)
  endif()
  if (NOT CMAKE_SYSTEM_NAME MATCHES "OpenBSD")
    list(APPEND NANOGUI_LIBS rt)
  endif()
  if (CMAKE_SYSTEM MATCHES "Linux")
    list(APPEND NANOGUI_LIBS dl m)
  endif()

  # ---- Optional Wayland pinch / multitouch (src/wayland.cpp) -----------------
  # GLFW 3.4 provides windowing + libdecor; we still generate the pointer-
  # gestures protocol for touchpad pinch and compile src/wayland.cpp.
  if (NANOGUI_USE_WAYLAND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
    pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols>=1.15)
    find_program(WAYLAND_SCANNER_EXECUTABLE NAMES wayland-scanner REQUIRED)
    pkg_get_variable(WAYLAND_PROTOCOLS_BASE wayland-protocols pkgdatadir)

    set(_PG_XML "${WAYLAND_PROTOCOLS_BASE}/unstable/pointer-gestures/pointer-gestures-unstable-v1.xml")
    if (NOT EXISTS "${_PG_XML}")
      message(FATAL_ERROR "pointer-gestures protocol not found at ${_PG_XML}. "
        "Install wayland-protocols (e.g. apt install wayland-protocols).")
    endif()

    set(_PG_HDR "${CMAKE_CURRENT_BINARY_DIR}/pointer-gestures-unstable-v1-client-protocol.h")
    set(_PG_SRC "${CMAKE_CURRENT_BINARY_DIR}/pointer-gestures-unstable-v1-protocol.c")
    add_custom_command(
      OUTPUT "${_PG_HDR}" "${_PG_SRC}"
      COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" client-header "${_PG_XML}" "${_PG_HDR}"
      COMMAND "${WAYLAND_SCANNER_EXECUTABLE}" private-code  "${_PG_XML}" "${_PG_SRC}"
      DEPENDS "${_PG_XML}"
      COMMENT "Generating Wayland pointer-gestures protocol"
      VERBATIM)

    list(APPEND NANOGUI_EXTRA
      src/wayland.cpp
      "${_PG_SRC}"
      "${_PG_HDR}")
    list(APPEND NANOGUI_LIBS ${WAYLAND_CLIENT_LIBRARIES})
    include_directories(${WAYLAND_CLIENT_INCLUDE_DIRS})
    add_definitions(-DNANOGUI_WAYLAND)
    message(STATUS "NanoGUI: Wayland pinch gestures enabled (src/wayland.cpp).")
  endif()
endif()

# SQLite support on all desktop platforms
if (NOT ${CMAKE_CXX_COMPILER_ID} MATCHES "Emscripten")
  list(APPEND NANOGUI_LIBS sqlite3)
endif()

# Use freetype library
if (NANOGUI_USE_FREETYPE)
  add_definitions(-DFONS_USE_FREETYPE)
  find_package(Freetype REQUIRED)
  list(APPEND NANOGUI_LIBS ${FREETYPE_LIBRARIES})
  include_directories(${FREETYPE_INCLUDE_DIRS})

  find_package(PkgConfig)
  pkg_check_modules(HARFBUZZ harfbuzz)
  if (HARFBUZZ_FOUND)
    message(STATUS "HARFBUZZ_LIBRARIES (pkg-config): ${HARFBUZZ_LIBRARIES}")
    message(STATUS "HARFBUZZ_INCLUDE_DIRS (pkg-config): ${HARFBUZZ_INCLUDE_DIRS}")
    list(APPEND NANOGUI_LIBS ${HARFBUZZ_LIBRARIES})
    include_directories(${HARFBUZZ_INCLUDE_DIRS})
  endif()
endif()

# Run simple cmake converter to put font files into the data segment

# Glob up resource files
file(GLOB resources
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/*.ttf"
  "${NANOGUI_GLOB}")

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/resources")

# Precompile .metal shaders to .metallib files
foreach(fname_in IN LISTS resources)
  if (NANOGUI_BACKEND STREQUAL "Metal" AND fname_in MATCHES "\\.metal")
    get_filename_component(fname_out ${fname_in} NAME)
    set(fname_out "${CMAKE_CURRENT_BINARY_DIR}/resources/${fname_out}lib")
    add_custom_command(
        OUTPUT ${fname_out}
        DEPENDS ${fname_in}
        COMMAND xcrun -sdk macosx metal -std=osx-metal2.0 -O3 "${fname_in}" -o "${fname_out}"
        VERBATIM
    )
  else()
    set(fname_out "${fname_in}")
  endif()
  list(APPEND resources_processed ${fname_out})
endforeach()

# Concatenate resource files into a comma separated string
string(REGEX REPLACE "([^\\]|^);" "\\1," resources_string "${resources_processed}")
string(REGEX REPLACE "[\\](.)" "\\1" resources_string "${resources_string}")

# Create command line for running bin2c cmake script
set(bin2c_cmdline
  -DOUTPUT_C=nanogui_resources.cpp
  -DOUTPUT_H=nanogui_resources.h
  "-DINPUT_FILES=${resources_string}"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/resources/bin2c.cmake")


# Run bin2c on resource files
add_custom_command(
  OUTPUT nanogui_resources.cpp nanogui_resources.h
  COMMAND ${CMAKE_COMMAND} ARGS ${bin2c_cmdline}
  DEPENDS ${resources} ${resources_processed}
  COMMENT "Running bin2c"
  PRE_BUILD VERBATIM)

# Needed to generated files
include_directories(${CMAKE_CURRENT_BINARY_DIR})

# Set library type
if (NANOGUI_BUILD_SHARED)
  set(NANOGUI_LIBRARY_TYPE "SHARED")
else()
  set(NANOGUI_LIBRARY_TYPE "STATIC")
endif()

if (APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  # Use automatic reference counting for Objective-C portions
  add_compile_options(-fobjc-arc)
endif()

# Disable noisy warnings from header-only libraries
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang")
    add_compile_options(-Wno-unused-function -Wno-missing-braces -Wno-unused-parameter)
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options(-Wno-unused-function -Wno-unused-parameter)
endif()

# Compile main NanoGUI library
add_library(nanogui ${NANOGUI_LIBRARY_TYPE}
  # Merge NanoVG into the NanoGUI library
  ext/nanovgd/src/nanovg.c
  ext/nanovgd/src/stb_image.h

  # Merge GLAD into the NanoGUI library (only if needed)
  ${NANOGUI_EXTRA}

  # Fonts etc.
  nanogui_resources.cpp
  include/nanogui/common.h src/common.cpp
  include/nanogui/widget.h src/widget.cpp
  include/nanogui/theme.h src/theme.cpp
  include/nanogui/layout.h src/layout.cpp
  include/nanogui/screen.h src/screen.cpp
  include/nanogui/label.h src/label.cpp
  include/nanogui/window.h src/window.cpp
  include/nanogui/popup.h src/popup.cpp
  include/nanogui/menu.h src/menu.cpp
  include/nanogui/checkbox.h src/checkbox.cpp
  include/nanogui/button.h src/button.cpp
  include/nanogui/popupbutton.h src/popupbutton.cpp
  include/nanogui/combobox.h src/combobox.cpp
  include/nanogui/progressbar.h src/progressbar.cpp
  include/nanogui/slider.h src/slider.cpp
  include/nanogui/messagedialog.h src/messagedialog.cpp
  include/nanogui/folderdialog.h src/folderdialog.cpp
  include/nanogui/textbox.h src/textbox.cpp
  include/nanogui/textarea.h src/textarea.cpp
  include/nanogui/document.h src/document.cpp
  include/nanogui/texteditor.h src/texteditor.cpp
  include/nanogui/imagepanel.h src/imagepanel.cpp
  include/nanogui/scrollpanel.h src/scrollpanel.cpp
    include/nanogui/zoomscrollpanel.h src/zoomscrollpanel.cpp
  include/nanogui/colorwheel.h src/colorwheel.cpp
  include/nanogui/colorpicker.h src/colorpicker.cpp
  include/nanogui/graph.h src/graph.cpp
  include/nanogui/tabwidget.h src/tabwidget.cpp
  include/nanogui/canvas.h src/canvas.cpp
  include/nanogui/texture.h src/texture.cpp
  include/nanogui/shader.h src/shader.cpp
  include/nanogui/imageview.h src/imageview.cpp
  include/nanogui/treeview.h src/treeview.cpp
  include/nanogui/datagrid.h src/datagrid.cpp
  include/nanogui/dataadapter.h src/dataadapter.cpp
  include/nanogui/sqliteadapter.h src/sqliteadapter.cpp
  include/nanogui/csvadapter.h src/csvadapter.cpp
  include/nanogui/traits.h src/traits.cpp
  include/nanogui/renderpass.h
  include/nanogui/formhelper.h
  include/nanogui/icons.h
  include/nanogui/toolbutton.h
  include/nanogui/opengl.h
  include/nanogui/nanogui.h
)

target_compile_definitions(nanogui
  PUBLIC
    ${NANOGUI_BACKEND_DEFS}

  PRIVATE
    -DNANOGUI_BUILD
    -DNVG_STB_IMAGE_IMPLEMENTATION
)

target_include_directories(nanogui
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include

  PRIVATE
    ext/nanovgd/src
    ${CMAKE_CURRENT_BINARY_DIR}
)

if (NANOGUI_BUILD_SHARED)
  target_compile_definitions(nanogui
    PUBLIC
      -DNANOGUI_SHARED -DNVG_SHARED
    PRIVATE
      -DNVG_BUILD
  )

  if (NANOGUI_BUILD_GLAD)
    target_compile_definitions(nanogui
      PUBLIC
        -DGLAD_GLAPI_EXPORT
      PRIVATE
        -DGLAD_GLAPI_EXPORT_BUILD
    )
  endif()

  # Static embedded GLFW: do not define _GLFW_BUILD_DLL / GLFW_DLL.
endif()

target_include_directories(nanogui PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/ext/nanovgd/src")

if (NANOGUI_BUILD_GLAD)
  target_compile_definitions(nanogui PUBLIC -DNANOGUI_GLAD)
  target_include_directories(nanogui
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/ext/glad/include")
endif()

if (NANOGUI_BUILD_GLFW)
  target_include_directories(nanogui
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/ext/glfw/include")
endif()

if (NANOGUI_BUILD_SHARED)
  target_link_libraries(nanogui PRIVATE ${NANOGUI_LIBS})
  set_property(TARGET nanogui PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)
  set_property(TARGET nanogui PROPERTY DEFINE_SYMBOL "")
else()
  target_link_libraries(nanogui PUBLIC ${NANOGUI_LIBS})
endif()

# Auto-detect GLES include directory on Raspberry PI
if (EXISTS /opt/vc/include)
  target_include_directories(nanogui PUBLIC /opt/vc/include)
endif()

if (NANOGUI_INSTALL)
  install(TARGETS nanogui
          LIBRARY DESTINATION lib
          ARCHIVE DESTINATION lib)

  install(DIRECTORY include/nanogui DESTINATION include
          FILES_MATCHING PATTERN "*.h")
endif()

# Quench warnings while compiling NanoVG
if (CMAKE_COMPILER_IS_GNUCC)
  set_source_files_properties(ext/nanovgd/src/nanovg.c PROPERTIES COMPILE_FLAGS -Wno-unused-result)
elseif(MSVC)
  set_source_files_properties(ext/nanovgd/src/nanovg.c PROPERTIES COMPILE_FLAGS "/wd4005 /wd4456 /wd4457")
endif()

# Optional QuickJS-NG scripting engine
if (NANOGUI_USE_QUICKJS)
  if (NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ext/quickjs-ng/CMakeLists.txt")
    message(FATAL_ERROR "QuickJS-NG submodule is missing. "
      "Run: git submodule update --init ext/quickjs-ng")
  endif()

  # Build quickjs-ng as a static library with -fPIC so it can be absorbed
  # into libnanogui.so when building a shared nanogui.
  set(BUILD_SHARED_LIBS_SAVED ${BUILD_SHARED_LIBS})
  set(BUILD_SHARED_LIBS OFF)
  set(QJS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(QJS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(QJS_BUILD_WERROR   OFF CACHE BOOL "" FORCE)
  set(CMAKE_POSITION_INDEPENDENT_CODE ON)
  add_subdirectory(ext/quickjs-ng EXCLUDE_FROM_ALL)
  set(BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS_SAVED})

  # JS widget binding sources (C++ → QuickJS bridge)
  set(NANOGUI_JS_SOURCES
    src/js/js_widget.cpp
    src/js/js_screen.cpp
    src/js/js_window.cpp
    src/js/js_label.cpp
    src/js/js_button.cpp
    src/js/js_layout.cpp
    src/js/js_module.cpp
    src/nanojs/nanojs.cpp
  )

  target_sources(nanogui PRIVATE ${NANOGUI_JS_SOURCES})
  target_link_libraries(nanogui PUBLIC qjs)

  # Consumers of nanogui get quickjs.h and our JS headers on their include path
  target_include_directories(nanogui
    PUBLIC
      ${CMAKE_CURRENT_SOURCE_DIR}/ext/quickjs-ng
      ${CMAKE_CURRENT_SOURCE_DIR}/src/nanojs
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src/js
  )

  target_compile_definitions(nanogui PUBLIC NANOGUI_USE_QUICKJS)

  # nanojs — standalone JS script runner (uses NanoJS runtime)
  add_executable(nanojs src/nanojs/nanojs_cli.cpp)
  target_link_libraries(nanojs PRIVATE nanogui)

  # Headless smoke-test for the NanoJS runtime (no GL window needed)
  add_executable(test_nanojs testapp/test_nanojs.cpp)
  target_link_libraries(test_nanojs PRIVATE nanogui)

  message(STATUS "QuickJS-NG: enabled (ext/quickjs-ng)")
  message(STATUS "  JS binding sources: ${NANOGUI_JS_SOURCES}")
else()
  message(STATUS "QuickJS-NG: disabled (pass -DNANOGUI_USE_QUICKJS=ON to enable)")
endif()

# Build example application if desired
if (NANOGUI_BUILD_EXAMPLES)
  add_executable(example1      src/example1.cpp)
  add_executable(example2      src/example2.cpp)
  add_executable(example3      src/example3.cpp)
  add_executable(example4      src/example4.cpp)
  add_executable(example_icons src/example_icons.cpp)
  add_executable(ngserver      examples/ngserver.cpp examples/guieditor_json.cpp)
  add_executable(nslabels      examples/nslabels.cpp)
  add_executable(nanovg-colorfont-atlas examples/nanovg-colorfont-atlas.cpp)
  add_executable(imagestash_test examples/imagestash_test.cpp)
  add_executable(flexdemo 	   examples/flexdemo.cpp)
  add_executable(flexsample    examples/flexsample.cpp)
  add_executable(flowsheet     examples/flowsheet.cpp)
  add_executable(flextest 	   examples/flextest.cpp)
  add_executable(guieditor     examples/guieditor.cpp examples/guieditor_json.cpp examples/guieditor_json_editor.cpp)
  add_executable(json2cpp      examples/json2cpp.cpp)
  add_executable(folderview    examples/folderview.cpp)
  add_executable(datatest      examples/datatest.cpp)
  # Vendored Gumbo HTML5 parser (ext/gumbo submodule), used by nmail to
  # render text/html message bodies.
  file(GLOB GUMBO_SOURCES CONFIGURE_DEPENDS ext/gumbo/src/*.c)
  add_library(gumbo STATIC ${GUMBO_SOURCES})
  target_include_directories(gumbo PUBLIC ext/gumbo/src)
  set_target_properties(gumbo PROPERTIES POSITION_INDEPENDENT_CODE ON)

  add_executable(nmail         nmail/nmail.cpp nmail/imap_client.cpp nmail/smtp_client.cpp nmail/nmail_socket.c)

  target_link_libraries(example1      nanogui)
  target_link_libraries(example2      nanogui)
  target_link_libraries(example3      nanogui ${NANOGUI_LIBS}) # For OpenGL
  target_link_libraries(example4      nanogui)
  target_link_libraries(example_icons nanogui)
  target_link_libraries(ngserver      nanogui)
  target_link_libraries(nslabels      nanogui)
  target_link_libraries(flexdemo	  nanogui)
  target_link_libraries(flexsample	  nanogui)
  target_link_libraries(flowsheet	  nanogui)
  target_link_libraries(flextest	  nanogui)
  target_link_libraries(folderview    nanogui)
  target_link_libraries(datatest      nanogui)
  target_include_directories(nmail PRIVATE
      nmail/
      ${CMAKE_CURRENT_SOURCE_DIR})
  # Output name "nmail" would collide with the nmail/ source directory
  set_target_properties(nmail PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin)
  target_link_libraries(nmail         nanogui pthread gumbo)
  # STARTTLS / imaps support in nmail_socket.c (guarded by HAVE_OPENSSL)
  find_package(OpenSSL)
  if (OPENSSL_FOUND)
    target_compile_definitions(nmail PRIVATE HAVE_OPENSSL)
    target_link_libraries(nmail OpenSSL::SSL OpenSSL::Crypto)
  endif()
  target_link_libraries(nanovg-colorfont-atlas nanogui ${NANOGUI_LIBS})
  target_link_libraries(imagestash_test nanogui ${NANOGUI_LIBS})
  target_link_libraries(guieditor nanogui ${NANOGUI_LIBS})


  # Copy icons for example application
  file(COPY resources/icons DESTINATION ${CMAKE_CURRENT_BINARY_DIR})
endif()

if (NANOGUI_BUILD_PYTHON)
  message(STATUS "NanoGUI: building the Python plugin.")
  if (NOT TARGET pybind11::module)
    find_package(Python COMPONENTS Interpreter Development REQUIRED)

    # Allow overriding the pybind11 library used to compile NanoGUI
    set(NANOGUI_PYBIND11_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ext/pybind11"
      CACHE STRING "Path containing the 'pybind11' library used to compile nanogui.")

    add_subdirectory(${NANOGUI_PYBIND11_DIR} ext/pybind11)

    mark_as_advanced(
      USE_PYTHON_INCLUDE_DIR PYBIND11_INSTALL PYBIND11_TEST PYBIND11_NOPYTHON
      PYBIND11_FINDPYTHON PYBIND11_PYTHON_VERSION PYBIND11_WERROR
      NANOGUI_PYBIND11_DIR
    )
  endif()

  add_subdirectory(python)
else()
  message(STATUS "NanoGUI: not building the Python plugin.")
endif()

if (NANOGUI_BUILD_PYTHON AND NANOGUI_MASTER_PROJECT)
  # Create documentation for python plugin (optional target for developers)
  string(REPLACE " " ";" MKDOC_CXX_FLAGS_LIST ${CMAKE_CXX_FLAGS})
  get_property(MKDOC_INCLUDE_DIRECTORIES DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY INCLUDE_DIRECTORIES)
  get_property(MKDOC_COMPILE_DEFINITIONS DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY COMPILE_DEFINITIONS)

  foreach (value ${MKDOC_INCLUDE_DIRECTORIES})
    list(APPEND MKDOC_CXX_FLAGS_LIST -I${value})
  endforeach()

  # Make sure platform specific code gets kept in py_doc.h (specifically __doc_nanogui_chdir_to_bundle_parent)
  list(APPEND MKDOC_COMPILE_DEFINITIONS "DOXYGEN_DOCUMENTATION_BUILD")
  foreach (value ${MKDOC_COMPILE_DEFINITIONS})
    list(APPEND MKDOC_CXX_FLAGS_LIST -D${value})
  endforeach()

  add_custom_target(mkdoc USES_TERMINAL COMMAND
    python3 ${PROJECT_SOURCE_DIR}/docs/mkdoc_rst.py
      ${MKDOC_CXX_FLAGS_LIST}
      ${PROJECT_SOURCE_DIR}/include/nanogui/*.h
      > ${CMAKE_CURRENT_SOURCE_DIR}/python/py_doc.h)
endif()

# vim: set et ts=2 sw=2 ft=cmake nospell:
