cmake_minimum_required(VERSION 3.21)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR
    "In-source builds are not supported; they scatter build files through the source tree.\n"
    "Use a preset, which builds into build/<preset>/:\n"
    "  cmake --preset debug && cmake --build --preset debug\n"
    "or a separate build directory:\n"
    "  cmake -B build/debug && cmake --build build/debug\n"
    "Then remove the CMakeCache.txt and CMakeFiles/ this attempt created.")
endif()

project(hakkas VERSION 0.1.0 LANGUAGES C)

# A plain CMake project with no RTOS dependency:
#   hakkas_core - the portable core library (everything outside src/platform/, 01 §0)
#   hakkas_pal_posix - the POSIX PAL implementation (src/platform/posix/)
#   hakkas_cli_posix - the POSIX command line (src/platform/posix/cli.c)
#   hakkas      - the POSIX runtime (Linux / Raspberry Pi / x86_64 desktop)
#   tests       - unit tests (CTest)
# When included from another project via add_subdirectory()/FetchContent, only
# hakkas_core is built by default. Platform ports such as a future Zephyr
# module compile ${HAKKAS_CORE_SOURCES} and ${HAKKAS_THIRD_PARTY_SOURCES} from
# cmake/HakkasSources.cmake under their own toolchain.

include(cmake/HakkasSources.cmake)
include(cmake/HakkasVersion.cmake)

option(HAKKAS_BUILD_POSIX_RUNTIME "Build the POSIX runtime executable" ${PROJECT_IS_TOP_LEVEL})
option(HAKKAS_BUILD_TESTS "Build the unit tests" ${PROJECT_IS_TOP_LEVEL})

if(PROJECT_IS_TOP_LEVEL)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

  if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
  endif()

  find_program(CCACHE_PROGRAM ccache)
  if(CCACHE_PROGRAM AND NOT CMAKE_C_COMPILER_LAUNCHER)
    set(CMAKE_C_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
  endif()
endif()

include(cmake/HakkasCompileOptions.cmake)

# The core is C17 without compiler extensions or platform headers, so it builds
# unchanged under any hosted C toolchain.
# Vendored libraries (third_party/) are part of it; they get the project's
# flags (and sanitizers) too, less any single warning a vendored file trips
# (hakkas_third_party_warning_exceptions, cmake/HakkasCompileOptions.cmake).
add_library(hakkas_core STATIC ${HAKKAS_CORE_SOURCES} ${HAKKAS_THIRD_PARTY_SOURCES})
hakkas_third_party_warning_exceptions()
add_library(hakkas::core ALIAS hakkas_core)
target_include_directories(hakkas_core PUBLIC ${HAKKAS_SOURCE_ROOT} ${HAKKAS_GENERATED_DIR}
  ${HAKKAS_THIRD_PARTY_INCLUDE_DIRS})
target_compile_definitions(hakkas_core PUBLIC ${HAKKAS_THIRD_PARTY_DEFINITIONS})
target_compile_features(hakkas_core PUBLIC c_std_17)
set_target_properties(hakkas_core PROPERTIES C_EXTENSIONS OFF)
target_link_libraries(hakkas_core PRIVATE hakkas_compile_options)

# The core uses <math.h> (fmod, copysign, ...). Where libm is a separate
# library (glibc), link it; toolchains with the math functions in libc
# (Zephyr's picolibc/newlib builds, macOS) have none and need nothing.
find_library(HAKKAS_MATH_LIBRARY m)
mark_as_advanced(HAKKAS_MATH_LIBRARY)
if(HAKKAS_MATH_LIBRARY)
  target_link_libraries(hakkas_core PUBLIC ${HAKKAS_MATH_LIBRARY})
endif()

# Work queue capacity in entries (01 §3.2 item 9). Public: the capacity sizes
# the WorkQueue struct, so every user of the core must see the same value.
set(HAKKAS_QUEUE_CAPACITY 1024 CACHE STRING "Work queue capacity in entries (01 §3.2)")
if(NOT HAKKAS_QUEUE_CAPACITY MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR "HAKKAS_QUEUE_CAPACITY must be a positive integer, got '${HAKKAS_QUEUE_CAPACITY}'")
endif()
target_compile_definitions(hakkas_core PUBLIC HAKKAS_QUEUE_CAPACITY=${HAKKAS_QUEUE_CAPACITY})

# The POSIX PAL, a library so that tests can link it too.
if(HAKKAS_BUILD_POSIX_RUNTIME OR HAKKAS_BUILD_TESTS)
  add_library(hakkas_pal_posix STATIC ${HAKKAS_POSIX_PAL_SOURCES})
  # The handoff's lock (01 §0.2) is the PAL's only use of threads.
  find_package(Threads REQUIRED)
  target_link_libraries(hakkas_pal_posix PUBLIC hakkas_core Threads::Threads
    PRIVATE hakkas_compile_options)
  # The POSIX PAL targets Linux (getrandom, 01 §0), so it may use GNU
  # extensions; the definition stays private to the PAL sources.
  target_compile_definitions(hakkas_pal_posix PRIVATE _GNU_SOURCE)

  # The command line (05 §1.2), a library so that tests can run it in-process.
  add_library(hakkas_cli_posix STATIC ${HAKKAS_POSIX_CLI_SOURCES})
  target_link_libraries(hakkas_cli_posix PUBLIC hakkas_pal_posix hakkas_core
    PRIVATE hakkas_compile_options)
  target_compile_definitions(hakkas_cli_posix PRIVATE _POSIX_C_SOURCE=200809L)
endif()

if(HAKKAS_BUILD_POSIX_RUNTIME)
  add_executable(hakkas ${HAKKAS_POSIX_RUNTIME_SOURCES})
  # The core calls the PAL (app/, system/config_file.c, engine/loop.c) and
  # the PAL calls the core (logging, the work queue's entries). Naming hakkas_core right after the command line puts it
  # before the PAL on the link line, and CMake adds it again after the PAL.
  target_link_libraries(hakkas PRIVATE hakkas_cli_posix hakkas_core hakkas_compile_options)
endif()

if(HAKKAS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
