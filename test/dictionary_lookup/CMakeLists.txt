# Fixture .cpdict is generated at configure time by the real converter so the
# on-device reader is always tested against the current format.
set(FIXTURE_TSV ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/mini.tsv)
set(FIXTURE_CPDICT ${CMAKE_CURRENT_BINARY_DIR}/mini.cpdict)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(
  COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/scripts/build_dictionary.py
          --tsv ${FIXTURE_TSV} --out ${FIXTURE_CPDICT} --verify
  RESULT_VARIABLE FIXTURE_RESULT
)
if(NOT FIXTURE_RESULT EQUAL 0)
  message(FATAL_ERROR "build_dictionary.py failed to generate the test fixture")
endif()

add_executable(DictLookupTest
  DictLookupTest.cpp
  ${REPO_ROOT}/lib/Dictionary/CpDictFile.cpp
  ${REPO_ROOT}/lib/Dictionary/DictKey.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(DictLookupTest PRIVATE
  ${REPO_ROOT}/lib/Utf8
)

target_compile_definitions(DictLookupTest PRIVATE
  FIXTURE_CPDICT_PATH="${FIXTURE_CPDICT}"
)

target_link_libraries(DictLookupTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DictLookupTest)
