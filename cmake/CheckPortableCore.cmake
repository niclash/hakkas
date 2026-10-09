# Fails if any file under src/ outside a platform port directory
# (src/platform/<port>/) includes an RTOS or POSIX header (CLAUDE.md,
# "Portable core"; 01 §0). Portable headers directly in src/platform/, such as
# the PAL interface, are checked too.
#
# Usage: cmake -DSOURCE_DIR=<repo>/src -P CheckPortableCore.cmake

if(NOT SOURCE_DIR)
  message(FATAL_ERROR "SOURCE_DIR not set")
endif()

set(_forbidden
  "zephyr/"
  "unistd\\.h" "sys/" "pthread\\.h" "poll\\.h" "fcntl\\.h" "dirent\\.h" "termios\\.h"
  "netinet/" "arpa/" "netdb\\.h" "sched\\.h" "semaphore\\.h" "dlfcn\\.h" "syslog\\.h"
  "linux/" "pwd\\.h" "grp\\.h" "spawn\\.h" "strings\\.h" "libgen\\.h")
list(JOIN _forbidden "|" _forbidden_alt)
set(_include_re "^[ \t]*#[ \t]*include[ \t]*[<\"](${_forbidden_alt})")

file(GLOB_RECURSE _files LIST_DIRECTORIES false "${SOURCE_DIR}/*.c" "${SOURCE_DIR}/*.h")

set(_violations "")
foreach(_file IN LISTS _files)
  file(RELATIVE_PATH _rel "${SOURCE_DIR}" "${_file}")
  if(_rel MATCHES "^platform/[^/]+/")
    continue()
  endif()
  file(STRINGS "${_file}" _lines REGEX "${_include_re}")
  foreach(_line IN LISTS _lines)
    string(STRIP "${_line}" _line)
    list(APPEND _violations "  src/${_rel}: ${_line}")
  endforeach()
endforeach()

if(_violations)
  list(JOIN _violations "\n" _report)
  message(FATAL_ERROR "Platform headers in the portable core (only src/platform/<port>/ may use them):\n${_report}")
endif()
message(STATUS "Portable core is clean")
