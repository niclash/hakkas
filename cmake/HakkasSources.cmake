# Source lists. Module layout: 01 §6.
#
# HAKKAS_CORE_SOURCES is the portable core: no RTOS or POSIX headers (enforced
# by the portable_core test, cmake/CheckPortableCore.cmake). It is kept as a
# plain list, rather than only as the hakkas_core target, so that a platform
# port with its own build integration (e.g. a Zephyr module) can compile the
# same files with its own toolchain and flags.

set(HAKKAS_SOURCE_ROOT ${CMAKE_CURRENT_LIST_DIR}/../src)
cmake_path(NORMAL_PATH HAKKAS_SOURCE_ROOT)

set(HAKKAS_CORE_SOURCES
  ${HAKKAS_SOURCE_ROOT}/blocktypes/block_types.c
  ${HAKKAS_SOURCE_ROOT}/blocktypes/math.c
  ${HAKKAS_SOURCE_ROOT}/blocktypes/process.c
  ${HAKKAS_SOURCE_ROOT}/engine/version.c
  ${HAKKAS_SOURCE_ROOT}/graphs/block.c
  ${HAKKAS_SOURCE_ROOT}/graphs/graph.c
  ${HAKKAS_SOURCE_ROOT}/graphs/linkable.c
  ${HAKKAS_SOURCE_ROOT}/graphs/runtime.c
  ${HAKKAS_SOURCE_ROOT}/graphs/work_queue.c
  ${HAKKAS_SOURCE_ROOT}/system/log.c
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
