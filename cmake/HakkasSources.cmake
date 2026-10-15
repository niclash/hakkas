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
  ${HAKKAS_SOURCE_ROOT}/app/process_list.c
  ${HAKKAS_SOURCE_ROOT}/app/type_files.c
  ${HAKKAS_SOURCE_ROOT}/blocktypes/block_types.c
  ${HAKKAS_SOURCE_ROOT}/blocktypes/calendar.c
  ${HAKKAS_SOURCE_ROOT}/blocktypes/compare.c
  ${HAKKAS_SOURCE_ROOT}/blocktypes/composite_types.c
  ${HAKKAS_SOURCE_ROOT}/blocktypes/logic.c
  ${HAKKAS_SOURCE_ROOT}/blocktypes/math.c
  ${HAKKAS_SOURCE_ROOT}/blocktypes/process.c
  ${HAKKAS_SOURCE_ROOT}/blocktypes/process_more.c
  ${HAKKAS_SOURCE_ROOT}/blocktypes/timing.c
  ${HAKKAS_SOURCE_ROOT}/drivers/bindings_file.c
  ${HAKKAS_SOURCE_ROOT}/drivers/env_description.c
  ${HAKKAS_SOURCE_ROOT}/drivers/environment.c
  ${HAKKAS_SOURCE_ROOT}/engine/engine.c
  ${HAKKAS_SOURCE_ROOT}/engine/hakkas.c
  ${HAKKAS_SOURCE_ROOT}/engine/loop.c
  ${HAKKAS_SOURCE_ROOT}/engine/version.c
  ${HAKKAS_SOURCE_ROOT}/engine/web_server.c
  ${HAKKAS_SOURCE_ROOT}/engine/ws_server.c
  ${HAKKAS_SOURCE_ROOT}/graphs/block.c
  ${HAKKAS_SOURCE_ROOT}/graphs/deadlines.c
  ${HAKKAS_SOURCE_ROOT}/graphs/graph.c
  ${HAKKAS_SOURCE_ROOT}/graphs/graph_bindings.c
  ${HAKKAS_SOURCE_ROOT}/graphs/graph_json.c
  ${HAKKAS_SOURCE_ROOT}/graphs/graph_links.c
  ${HAKKAS_SOURCE_ROOT}/graphs/graph_ports.c
  ${HAKKAS_SOURCE_ROOT}/graphs/graph_retained.c
  ${HAKKAS_SOURCE_ROOT}/graphs/graphs.c
  ${HAKKAS_SOURCE_ROOT}/graphs/linkable.c
  ${HAKKAS_SOURCE_ROOT}/graphs/runtime.c
  ${HAKKAS_SOURCE_ROOT}/graphs/work_queue.c
  ${HAKKAS_SOURCE_ROOT}/protocol/auth.c
  ${HAKKAS_SOURCE_ROOT}/protocol/dispatcher.c
  ${HAKKAS_SOURCE_ROOT}/protocol/http.c
  ${HAKKAS_SOURCE_ROOT}/protocol/messages.c
  ${HAKKAS_SOURCE_ROOT}/protocol/protocol_ws.c
  ${HAKKAS_SOURCE_ROOT}/protocol/users.c
  ${HAKKAS_SOURCE_ROOT}/protocol/users_file.c
  ${HAKKAS_SOURCE_ROOT}/protocol/websocket.c
  ${HAKKAS_SOURCE_ROOT}/system/config.c
  ${HAKKAS_SOURCE_ROOT}/system/config_file.c
  ${HAKKAS_SOURCE_ROOT}/system/crypto.c
  ${HAKKAS_SOURCE_ROOT}/system/log.c
  ${HAKKAS_SOURCE_ROOT}/system/tz.c
  ${HAKKAS_SOURCE_ROOT}/system/uuid.c
)

# Vendored third-party code that is part of the portable core: every build of
# the core, including a port's own, compiles HAKKAS_THIRD_PARTY_SOURCES with
# HAKKAS_THIRD_PARTY_INCLUDE_DIRS on the include path and
# HAKKAS_THIRD_PARTY_DEFINITIONS defined (the definitions are visible to users
# of the headers too, so they must be the same everywhere). Only standard C
# headers are used. See third_party/<library>/README.md.
#   cJSON (01 §6): CJSON_NESTING_LIMIT bounds the parser's recursion depth on
#   MCU stacks (third_party/cjson/README.md).
#   mbedTLS (01 §6): only the modules behind src/system/crypto.h, configured
#   by hakkas' own third_party/mbedtls-config/hakkas_mbedtls_config.h
#   (third_party/mbedtls/README.md).
set(HAKKAS_THIRD_PARTY_ROOT ${CMAKE_CURRENT_LIST_DIR}/../third_party)
cmake_path(NORMAL_PATH HAKKAS_THIRD_PARTY_ROOT)

set(HAKKAS_MBEDTLS_SOURCES
  ${HAKKAS_THIRD_PARTY_ROOT}/mbedtls/library/base64.c
  ${HAKKAS_THIRD_PARTY_ROOT}/mbedtls/library/constant_time.c
  ${HAKKAS_THIRD_PARTY_ROOT}/mbedtls/library/md.c
  ${HAKKAS_THIRD_PARTY_ROOT}/mbedtls/library/pkcs5.c
  ${HAKKAS_THIRD_PARTY_ROOT}/mbedtls/library/platform_util.c
  ${HAKKAS_THIRD_PARTY_ROOT}/mbedtls/library/sha1.c
  ${HAKKAS_THIRD_PARTY_ROOT}/mbedtls/library/sha256.c
)

set(HAKKAS_THIRD_PARTY_SOURCES
  ${HAKKAS_THIRD_PARTY_ROOT}/cjson/cJSON.c
  ${HAKKAS_MBEDTLS_SOURCES}
)
set(HAKKAS_THIRD_PARTY_INCLUDE_DIRS
  ${HAKKAS_THIRD_PARTY_ROOT}/cjson
  ${HAKKAS_THIRD_PARTY_ROOT}/mbedtls/include
  ${HAKKAS_THIRD_PARTY_ROOT}/mbedtls-config
)
set(HAKKAS_THIRD_PARTY_DEFINITIONS
  CJSON_NESTING_LIMIT=32
  MBEDTLS_CONFIG_FILE="hakkas_mbedtls_config.h"
)

# The POSIX PAL implementation (01 §0): the hakkas_pal_posix library, linked
# by the POSIX runtime and by tests that exercise the PAL.
set(HAKKAS_POSIX_PAL_SOURCES
  ${HAKKAS_SOURCE_ROOT}/platform/posix/env_link.c
  ${HAKKAS_SOURCE_ROOT}/platform/posix/log_stderr.c
  ${HAKKAS_SOURCE_ROOT}/platform/posix/pal.c
  ${HAKKAS_SOURCE_ROOT}/platform/posix/signals.c
  ${HAKKAS_SOURCE_ROOT}/platform/posix/socket.c
  ${HAKKAS_SOURCE_ROOT}/platform/posix/storage.c
  ${HAKKAS_SOURCE_ROOT}/platform/posix/wakeup.c
)

# The POSIX command line (05 §1.2): the hakkas_cli_posix library, linked by
# the runtime and by its tests.
set(HAKKAS_POSIX_CLI_SOURCES
  ${HAKKAS_SOURCE_ROOT}/platform/posix/cli.c
  ${HAKKAS_SOURCE_ROOT}/platform/posix/host.c
  ${HAKKAS_SOURCE_ROOT}/platform/posix/password.c
)

# The POSIX runtime executable on top of it.
set(HAKKAS_POSIX_RUNTIME_SOURCES
  ${HAKKAS_SOURCE_ROOT}/platform/posix/main.c
)
