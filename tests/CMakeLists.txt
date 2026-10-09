# Unit tests for the POSIX build tree, run with CTest (05 §5).

# hakkas_add_test(<name> [LIBS <library>...]): builds <name>.c against the
# core, plus any extra libraries such as hakkas_pal_posix.
function(hakkas_add_test name)
  cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "LIBS")
  add_executable(${name} ${name}.c)
  target_link_libraries(${name} PRIVATE ${arg_LIBS} hakkas_core hakkas_compile_options)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

hakkas_add_test(test_version)
hakkas_add_test(test_linkable)
hakkas_add_test(test_log)
hakkas_add_test(test_pal_posix LIBS hakkas_pal_posix)

# CLAUDE.md "Portable core": no platform headers outside src/platform/<port>/.
add_test(NAME portable_core
  COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${HAKKAS_SOURCE_ROOT} -P ${PROJECT_SOURCE_DIR}/cmake/CheckPortableCore.cmake)
