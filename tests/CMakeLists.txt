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
hakkas_add_test(test_work_queue)
hakkas_add_test(test_block_types)

# The same work queue tests with a tiny capacity, so wrap-around and overflow
# run in a few pushes and the capacity override is exercised. The capacity
# sizes a struct, so this target compiles the core sources itself instead of
# linking hakkas_core (built with HAKKAS_QUEUE_CAPACITY).
add_executable(test_work_queue_small test_work_queue.c ${HAKKAS_CORE_SOURCES})
target_include_directories(test_work_queue_small PRIVATE ${HAKKAS_SOURCE_ROOT} ${HAKKAS_GENERATED_DIR})
target_compile_features(test_work_queue_small PRIVATE c_std_17)
set_target_properties(test_work_queue_small PROPERTIES C_EXTENSIONS OFF)
target_compile_definitions(test_work_queue_small PRIVATE HAKKAS_QUEUE_CAPACITY=4)
target_link_libraries(test_work_queue_small PRIVATE hakkas_compile_options)
if(HAKKAS_MATH_LIBRARY)
  target_link_libraries(test_work_queue_small PRIVATE ${HAKKAS_MATH_LIBRARY})
endif()
add_test(NAME test_work_queue_small COMMAND test_work_queue_small)

# CLAUDE.md "Portable core": no platform headers outside src/platform/<port>/.
add_test(NAME portable_core
  COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${HAKKAS_SOURCE_ROOT} -P ${PROJECT_SOURCE_DIR}/cmake/CheckPortableCore.cmake)
