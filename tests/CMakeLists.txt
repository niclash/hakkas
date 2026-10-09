# Unit tests for the POSIX build tree, run with CTest (05 §5).

function(hakkas_add_test name)
  add_executable(${name} ${name}.c)
  target_link_libraries(${name} PRIVATE hakkas_core hakkas_compile_options)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

hakkas_add_test(test_version)
hakkas_add_test(test_linkable)
hakkas_add_test(test_log)

# CLAUDE.md "Portable core": no platform headers outside src/platform/<port>/.
add_test(NAME portable_core
  COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${HAKKAS_SOURCE_ROOT} -P ${PROJECT_SOURCE_DIR}/cmake/CheckPortableCore.cmake)
