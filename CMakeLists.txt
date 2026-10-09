cmake_minimum_required(VERSION 3.21)

project(hakkas VERSION 0.1.0 LANGUAGES C)

# A plain CMake project with no RTOS dependency:
#   hakkas_core - the portable core library (everything outside src/platform/, 01 §0)
#   hakkas      - the POSIX runtime (Linux / Raspberry Pi / x86_64 desktop)
#   tests       - unit tests (CTest)
# When included from another project via add_subdirectory()/FetchContent, only
# hakkas_core is built by default. Platform ports such as a future Zephyr
# module compile ${HAKKAS_CORE_SOURCES} from cmake/HakkasSources.cmake under
# their own toolchain.

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
add_library(hakkas_core STATIC ${HAKKAS_CORE_SOURCES})
add_library(hakkas::core ALIAS hakkas_core)
target_include_directories(hakkas_core PUBLIC ${HAKKAS_SOURCE_ROOT} ${HAKKAS_GENERATED_DIR})
target_compile_features(hakkas_core PUBLIC c_std_17)
set_target_properties(hakkas_core PROPERTIES C_EXTENSIONS OFF)
target_link_libraries(hakkas_core PRIVATE hakkas_compile_options)

if(HAKKAS_BUILD_POSIX_RUNTIME)
  add_executable(hakkas ${HAKKAS_POSIX_SOURCES})
  target_link_libraries(hakkas PRIVATE hakkas_core hakkas_compile_options)
  # The POSIX PAL targets Linux (timerfd, 01 §0), so it may use GNU extensions.
  target_compile_definitions(hakkas PRIVATE _GNU_SOURCE)
endif()

if(HAKKAS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
