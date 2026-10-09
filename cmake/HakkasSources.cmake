# Source lists. Module layout: 01 §6.
#
# HAKKAS_CORE_SOURCES is the portable core: no RTOS or POSIX headers (enforced
# by the portable_core test, cmake/CheckPortableCore.cmake). It is kept as a
# plain list, rather than only as the hakkas_core target, so that a platform
# port with its own build integration (e.g. a Zephyr module) can compile the
# same files with its own toolchain and flags, together with
# HAKKAS_THIRD_PARTY_SOURCES below.

set(HAKKAS_SOURCE_ROOT ${CMAKE_CURRENT_LIST_DIR}/../src)
cmake_path(NORMAL_PATH HAKKAS_SOURCE_ROOT)

set(HAKKAS_CORE_SOURCES
  ${HAKKAS_SOURCE_ROOT}/app/file_names.c
  ${HAKKAS_SOURCE_ROOT}/app/graph_files.c
  ${HAKKAS_SOURCE_ROOT}/blocktypes/block_types.c
  ${HAKKAS_SOURCE_ROOT}/blocktypes/math.c
  ${HAKKAS_SOURCE_ROOT}/blocktypes/process.c
  ${HAKKAS_SOURCE_ROOT}/blocktypes/timing.c
  ${HAKKAS_SOURCE_ROOT}/engine/version.c
  ${HAKKAS_SOURCE_ROOT}/graphs/block.c
  ${HAKKAS_SOURCE_ROOT}/graphs/deadlines.c
  ${HAKKAS_SOURCE_ROOT}/graphs/graph.c
  ${HAKKAS_SOURCE_ROOT}/graphs/graph_json.c
  ${HAKKAS_SOURCE_ROOT}/graphs/graph_ports.c
  ${HAKKAS_SOURCE_ROOT}/graphs/graph_retained.c
  ${HAKKAS_SOURCE_ROOT}/graphs/graphs.c
  ${HAKKAS_SOURCE_ROOT}/graphs/linkable.c
  ${HAKKAS_SOURCE_ROOT}/graphs/runtime.c
  ${HAKKAS_SOURCE_ROOT}/graphs/work_queue.c
  ${HAKKAS_SOURCE_ROOT}/system/log.c
)

# Vendored third-party code that is part of the portable core: every build of
# the core, including a port's own, compiles HAKKAS_THIRD_PARTY_SOURCES with
# HAKKAS_THIRD_PARTY_INCLUDE_DIRS on the include path and
# HAKKAS_THIRD_PARTY_DEFINITIONS defined (the definitions are visible to users
# of the headers too, so they must be the same everywhere). Only standard C
# headers are used. See third_party/<library>/README.md.
#   cJSON (01 §6): CJSON_NESTING_LIMIT bounds the parser's recursion depth on
#   MCU stacks (third_party/cjson/README.md).
set(HAKKAS_THIRD_PARTY_ROOT ${CMAKE_CURRENT_LIST_DIR}/../third_party)
cmake_path(NORMAL_PATH HAKKAS_THIRD_PARTY_ROOT)

set(HAKKAS_THIRD_PARTY_SOURCES
  ${HAKKAS_THIRD_PARTY_ROOT}/cjson/cJSON.c
)
set(HAKKAS_THIRD_PARTY_INCLUDE_DIRS
  ${HAKKAS_THIRD_PARTY_ROOT}/cjson
)
set(HAKKAS_THIRD_PARTY_DEFINITIONS
  CJSON_NESTING_LIMIT=32
)

# The POSIX PAL implementation (01 §0): the hakkas_pal_posix library, linked
# by the POSIX runtime and by tests that exercise the PAL.
set(HAKKAS_POSIX_PAL_SOURCES
  ${HAKKAS_SOURCE_ROOT}/platform/posix/log_stderr.c
  ${HAKKAS_SOURCE_ROOT}/platform/posix/pal.c
  ${HAKKAS_SOURCE_ROOT}/platform/posix/storage.c
)

# The POSIX runtime executable on top of it.
set(HAKKAS_POSIX_RUNTIME_SOURCES
  ${HAKKAS_SOURCE_ROOT}/platform/posix/main.c
)
