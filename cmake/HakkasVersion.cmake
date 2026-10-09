# Generates hakkas_version.h with the build version string reported as
# "version" in runtime:runtime (02 §3.2.1): the project version plus, when
# building from a git checkout, `git describe` output. Evaluated at configure
# time.

set(HAKKAS_VERSION_STRING ${PROJECT_VERSION})

find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} describe --always --dirty
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    OUTPUT_VARIABLE _hakkas_git_describe
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE _hakkas_git_result)
  if(_hakkas_git_result EQUAL 0 AND _hakkas_git_describe)
    string(APPEND HAKKAS_VERSION_STRING "+${_hakkas_git_describe}")
  endif()
endif()

set(HAKKAS_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
configure_file(${CMAKE_CURRENT_LIST_DIR}/hakkas_version.h.in ${HAKKAS_GENERATED_DIR}/hakkas_version.h @ONLY)
