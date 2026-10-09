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
  ${HAKKAS_SOURCE_ROOT}/engine/version.c
)

set(HAKKAS_POSIX_SOURCES
  ${HAKKAS_SOURCE_ROOT}/platform/posix/main.c
)
