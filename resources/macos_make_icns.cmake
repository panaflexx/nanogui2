# Build a .icns from a PNG. The source may be taller than it is wide; the
# center square is what Dock and Finder show. Invoked as:
#   cmake -DSRC=icon.png -DDST=out.icns -P macos_make_icns.cmake
if (NOT SRC OR NOT DST)
  message(FATAL_ERROR "macos_make_icns: pass -DSRC= and -DDST=")
endif()
if (NOT EXISTS "${SRC}")
  message(FATAL_ERROR "macos_make_icns: missing ${SRC}")
endif()

find_program(SIPS sips)
find_program(ICONUTIL iconutil)
if (NOT SIPS OR NOT ICONUTIL)
  message(FATAL_ERROR "macos_make_icns: sips and iconutil are required")
endif()

execute_process(COMMAND "${SIPS}" -g pixelWidth "${SRC}"
  OUTPUT_VARIABLE _w_info OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND "${SIPS}" -g pixelHeight "${SRC}"
  OUTPUT_VARIABLE _h_info OUTPUT_STRIP_TRAILING_WHITESPACE)
string(REGEX MATCH "pixelWidth: ([0-9]+)" _w_match "${_w_info}")
set(_w "${CMAKE_MATCH_1}")
string(REGEX MATCH "pixelHeight: ([0-9]+)" _h_match "${_h_info}")
set(_h "${CMAKE_MATCH_1}")
if (NOT _w OR NOT _h)
  message(FATAL_ERROR "macos_make_icns: could not read the size of ${SRC}")
endif()
if (_w LESS _h)
  set(_side "${_w}")
else()
  set(_side "${_h}")
endif()

get_filename_component(_dir "${DST}" DIRECTORY)
set(_work "${_dir}/pagemade.iconset")
set(_square "${_dir}/pagemade-icon-square.png")
file(REMOVE_RECURSE "${_work}")
file(MAKE_DIRECTORY "${_work}")

execute_process(COMMAND "${SIPS}" -c ${_side} ${_side} "${SRC}" --out "${_square}"
  RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_VARIABLE _err)
if (NOT _rc EQUAL 0)
  message(FATAL_ERROR "macos_make_icns: crop failed: ${_err}")
endif()

# iconutil's required names. 1024 is scaled up when the art is smaller.
# A pipe separates the pixel size from the filename: a semicolon would be
# flattened when this list is expanded.
set(_sizes
  "16|icon_16x16.png"
  "32|icon_16x16@2x.png"
  "32|icon_32x32.png"
  "64|icon_32x32@2x.png"
  "128|icon_128x128.png"
  "256|icon_128x128@2x.png"
  "256|icon_256x256.png"
  "512|icon_256x256@2x.png"
  "512|icon_512x512.png"
  "1024|icon_512x512@2x.png")
foreach (_pair ${_sizes})
  string(REPLACE "|" ";" _parts "${_pair}")
  list(GET _parts 0 _px)
  list(GET _parts 1 _name)
  execute_process(
    COMMAND "${SIPS}" -z ${_px} ${_px} "${_square}" --out "${_work}/${_name}"
    RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_VARIABLE _err)
  if (NOT _rc EQUAL 0)
    message(FATAL_ERROR "macos_make_icns: resize ${_name} failed: ${_err}")
  endif()
endforeach()

execute_process(COMMAND "${ICONUTIL}" -c icns "${_work}" -o "${DST}"
  RESULT_VARIABLE _rc ERROR_VARIABLE _err)
if (NOT _rc EQUAL 0)
  message(FATAL_ERROR "macos_make_icns: iconutil failed: ${_err}")
endif()
file(REMOVE_RECURSE "${_work}")
file(REMOVE "${_square}")
