# Warning and sanitizer flags for the POSIX build tree, applied to hakkas'
# own targets only via the hakkas_compile_options interface library.

option(HAKKAS_WERROR "Treat compiler warnings as errors" OFF)
set(HAKKAS_SANITIZERS "" CACHE STRING "Semicolon-separated -fsanitize= list, e.g. address;undefined")

add_library(hakkas_compile_options INTERFACE)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(hakkas_compile_options INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wstrict-prototypes
    -Wmissing-prototypes
    -Wundef
    -Wformat=2
    -Wvla
    -Wcast-align)
  if(HAKKAS_WERROR)
    target_compile_options(hakkas_compile_options INTERFACE -Werror)
  endif()

  if(HAKKAS_SANITIZERS)
    list(JOIN HAKKAS_SANITIZERS "," _hakkas_sanitizers)
    target_compile_options(hakkas_compile_options INTERFACE -fsanitize=${_hakkas_sanitizers} -fno-omit-frame-pointer)
    target_link_options(hakkas_compile_options INTERFACE -fsanitize=${_hakkas_sanitizers})
  endif()
endif()

# hakkas_third_party_warning_exceptions(): vendored files that warn under the
# flags above get that one warning turned off, for those files only (never
# for hakkas' own code). Source file properties are per directory, so every
# directory that compiles HAKKAS_THIRD_PARTY_SOURCES calls this.
#   mbedTLS: library/alignment.h tests MBEDTLS_GCC_VERSION, which is defined
#   only for gcc, in an #if (-Wundef under clang; third_party/mbedtls/README.md).
function(hakkas_third_party_warning_exceptions)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(${HAKKAS_MBEDTLS_SOURCES} PROPERTIES COMPILE_OPTIONS -Wno-undef)
  endif()
endfunction()
