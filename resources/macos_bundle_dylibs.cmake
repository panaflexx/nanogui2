# Copy non-system dylibs into $APP/Contents/Frameworks and rewrite their
# install names to @rpath so nmail.app runs without Homebrew prefixes.
#
# Invoked as a POST_BUILD -P script:
#   cmake -DAPP=<bundle dir> -DEXE=<binary> -DSEARCH_DIRS=<lib dir> -P ...
#
# APP / EXE / SEARCH_DIRS are required (passed with -D on the command line).

if (NOT APP OR NOT EXE)
  message(FATAL_ERROR "macos_bundle_dylibs.cmake needs -DAPP= and -DEXE=")
endif()

set(_fw "${APP}/Contents/Frameworks")
file(MAKE_DIRECTORY "${_fw}")

set(_search "${SEARCH_DIRS}")
if (_search)
  string(REPLACE ":" ";" _search "${_search}")
endif()
list(APPEND _search "${_fw}" "${APP}/Contents/MacOS")
get_filename_component(_exe_dir "${EXE}" DIRECTORY)
list(APPEND _search "${_exe_dir}")

# True for OS / SDK libraries that must stay where they are.
function(_is_system_lib path out)
  if (path MATCHES "^/System/" OR
      path MATCHES "^/usr/lib/" OR
      path MATCHES "^/Library/Apple/" OR
      path MATCHES "^/Library/Developer/")
    set(${out} TRUE PARENT_SCOPE)
  else()
    set(${out} FALSE PARENT_SCOPE)
  endif()
endfunction()

# Install names from `otool -L` (skips the header line and system libs).
function(_otool_deps bin out)
  execute_process(COMMAND otool -L "${bin}"
    OUTPUT_VARIABLE _txt
    ERROR_VARIABLE _err
    RESULT_VARIABLE _rc
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if (NOT _rc EQUAL 0)
    set(${out} "" PARENT_SCOPE)
    return()
  endif()
  set(_deps "")
  string(REPLACE "\n" ";" _lines "${_txt}")
  set(_first TRUE)
  foreach (_line IN LISTS _lines)
    if (_first)
      set(_first FALSE)
      continue()
    endif()
    string(STRIP "${_line}" _line)
    if (_line MATCHES "^[ \t]*([^ \t]+)[ \t]+\\(compatibility")
      set(_name "${CMAKE_MATCH_1}")
      _is_system_lib("${_name}" _sys)
      if (NOT _sys)
        list(APPEND _deps "${_name}")
      endif()
    endif()
  endforeach()
  set(${out} "${_deps}" PARENT_SCOPE)
endfunction()

# Turn @rpath / @loader_path / @executable_path into a real file.
function(_resolve name from_bin out)
  if (IS_ABSOLUTE "${name}" AND EXISTS "${name}")
    set(${out} "${name}" PARENT_SCOPE)
    return()
  endif()
  get_filename_component(_base "${name}" NAME)
  set(_rel "${name}")
  string(REGEX REPLACE "^@rpath/" "" _rel "${_rel}")
  string(REGEX REPLACE "^@loader_path/" "" _rel "${_rel}")
  string(REGEX REPLACE "^@executable_path/" "" _rel "${_rel}")
  get_filename_component(_from_dir "${from_bin}" DIRECTORY)
  foreach (_dir ${_from_dir} ${_search})
    foreach (_cand "${_dir}/${_base}" "${_dir}/${_rel}")
      if (EXISTS "${_cand}")
        get_filename_component(_cand "${_cand}" REALPATH)
        set(${out} "${_cand}" PARENT_SCOPE)
        return()
      endif()
    endforeach()
  endforeach()
  set(${out} "" PARENT_SCOPE)
endfunction()

# BFS from the executable: copy every non-system dylib we can resolve.
set(_queue "${EXE}")
set(_copied "")
set(_seen "${EXE}")
while (_queue)
  list(GET _queue 0 _bin)
  list(REMOVE_AT _queue 0)
  _otool_deps("${_bin}" _deps)
  foreach (_dep IN LISTS _deps)
    _resolve("${_dep}" "${_bin}" _src)
    if (NOT _src)
      continue()
    endif()
    get_filename_component(_base "${_src}" NAME)
    set(_dst "${_fw}/${_base}")
    if (NOT EXISTS "${_dst}")
      execute_process(COMMAND ${CMAKE_COMMAND} -E copy "${_src}" "${_dst}")
      # Copied files are often not writable; install_name_tool needs that.
      execute_process(COMMAND chmod u+w "${_dst}")
    endif()
    list(FIND _seen "${_dst}" _idx)
    if (_idx EQUAL -1)
      list(APPEND _seen "${_dst}")
      list(APPEND _queue "${_dst}")
      list(APPEND _copied "${_dst}")
    endif()
  endforeach()
endwhile()

# Point the executable and every bundled dylib at @rpath/<basename>.
set(_rewrite "${EXE}" ${_copied})
foreach (_bin IN LISTS _rewrite)
  get_filename_component(_bin_base "${_bin}" NAME)
  if (NOT _bin STREQUAL "${EXE}")
    execute_process(COMMAND install_name_tool -id "@rpath/${_bin_base}" "${_bin}"
      RESULT_VARIABLE _rc ERROR_VARIABLE _err)
    if (NOT _rc EQUAL 0)
      message(WARNING "install_name_tool -id failed on ${_bin_base}: ${_err}")
    endif()
  endif()
  _otool_deps("${_bin}" _deps)
  foreach (_dep IN LISTS _deps)
    get_filename_component(_dep_base "${_dep}" NAME)
    if (EXISTS "${_fw}/${_dep_base}" AND NOT _dep STREQUAL "@rpath/${_dep_base}")
      execute_process(
        COMMAND install_name_tool -change "${_dep}" "@rpath/${_dep_base}" "${_bin}"
        RESULT_VARIABLE _rc ERROR_VARIABLE _err)
      if (NOT _rc EQUAL 0)
        message(WARNING "install_name_tool -change ${_dep} failed on ${_bin_base}: ${_err}")
      endif()
    endif()
  endforeach()
endforeach()

# Rewriting load commands invalidates the signature on modern macOS; ad-hoc
# re-sign. Harmless on 10.12 if codesign is missing or refuses.
foreach (_bin IN LISTS _rewrite)
  execute_process(COMMAND codesign --force --sign - "${_bin}"
    RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_QUIET)
endforeach()
