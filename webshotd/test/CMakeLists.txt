function(add_cpp_test target)
  cmake_parse_arguments(ARGS "" "" "SOURCES;LIBS;INCLUDE_DIRS" ${ARGN})

  add_executable(${target} ${ARGS_SOURCES})
  target_link_libraries(
    ${target}
    PRIVATE userver::utest
            ada::ada
            uni-algo::uni-algo
            absl::strings
            ${ARGS_LIBS})
  target_include_directories(
    ${target} PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/include
                      ${PROJECT_BINARY_DIR}/src ${ARGS_INCLUDE_DIRS})
  apply_defaults(${target})
  target_compile_options(${target} PRIVATE -Wno-switch-default)

  if(USE_SANITIZERS)
    target_compile_options(${target} PRIVATE "-fsanitize=address,undefined")
    target_link_options(${target} PRIVATE "-fsanitize=address,undefined")
  endif()

  add_test(NAME ${target} COMMAND ${target})
  set(test_env "LSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/lsan.supp")
  set_tests_properties(${target} PROPERTIES ENVIRONMENT "${test_env}")

  if(WEBSHOT_ENABLE_COVERAGE)
    target_compile_options(${target} PRIVATE ${WEBSHOT_COVERAGE_FLAGS})
    target_link_options(${target} PRIVATE ${WEBSHOT_COVERAGE_FLAGS})
  endif()

  set_property(GLOBAL APPEND PROPERTY WEBSHOT_CPP_TESTS ${target})
endfunction()

add_cpp_test(
  link_test SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/link.cpp
  ${CMAKE_SOURCE_DIR}/src/link.cpp ${CMAKE_SOURCE_DIR}/src/url.cpp)

add_cpp_test(
  s3_v4_client_test
  SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/s3_v4_client.cpp
  ${CMAKE_SOURCE_DIR}/src/s3/sigv4_signer.cpp
  ${CMAKE_SOURCE_DIR}/src/s3/s3_v4_client.cpp
  ${CMAKE_SOURCE_DIR}/src/link.cpp
  ${CMAKE_SOURCE_DIR}/src/url.cpp
  LIBS
  userver::core
  INCLUDE_DIRS
  ${CMAKE_SOURCE_DIR}/src)

add_cpp_test(
  s3_url_utils_test SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/s3_url_utils.cpp
  ${CMAKE_SOURCE_DIR}/src/s3/s3_url_utils.cpp)

add_cpp_test(
  s3_sts_client_test
  SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/s3_sts_client.cpp
  ${CMAKE_SOURCE_DIR}/src/s3/s3_sts_client.cpp
  ${CMAKE_SOURCE_DIR}/src/s3/sigv4_signer.cpp
  ${CMAKE_SOURCE_DIR}/src/s3/s3_url_utils.cpp
  ${CMAKE_SOURCE_DIR}/src/link.cpp
  ${CMAKE_SOURCE_DIR}/src/url.cpp
  LIBS
  userver::core
  INCLUDE_DIRS
  ${CMAKE_SOURCE_DIR}/src)

add_cpp_test(
  cursor_test SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/cursor.cpp ${CMAKE_SOURCE_DIR}/src/cursor.cpp
  LIBS ${PROJECT_NAME}-chgen)

add_cpp_test(
  pagination_test
  SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/pagination.cpp ${CMAKE_SOURCE_DIR}/src/cursor.cpp
  ${CMAKE_SOURCE_DIR}/src/pagination.cpp
  LIBS
  ${PROJECT_NAME}-chgen)

add_cpp_test(
  prefix_pagination_test
  SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/prefix_pagination.cpp
  ${CMAKE_SOURCE_DIR}/src/cursor.cpp ${CMAKE_SOURCE_DIR}/src/prefix_pagination.cpp
  LIBS
  ${PROJECT_NAME}-chgen)

add_cpp_test(
  error_utils_test SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/error_utils.cpp
  ${CMAKE_SOURCE_DIR}/src/error_utils.cpp LIBS ${PROJECT_NAME}-chgen)

add_cpp_test(integers_test SOURCES
                     ${CMAKE_CURRENT_SOURCE_DIR}/integers.cpp)

add_cpp_test(text_test SOURCES
                     ${CMAKE_CURRENT_SOURCE_DIR}/text.cpp)

add_cpp_test(
  s3_secdist_test SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/s3_secdist.cpp LIBS
  userver::core)

add_cpp_test(s3_refresh_schedule_test SOURCES
                     ${CMAKE_CURRENT_SOURCE_DIR}/s3_refresh_schedule.cpp)

add_cpp_test(deadline_utils_test SOURCES
                     ${CMAKE_CURRENT_SOURCE_DIR}/deadline_utils.cpp)

add_cpp_test(
  crawler_fallback_test SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/crawler_fallback.cpp)

add_cpp_test(
  browser_sandbox_test SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/browser_sandbox.cpp
  ${CMAKE_SOURCE_DIR}/src/crawler/browser_sandbox.cpp)

add_cpp_test(cdp_request_tracker_test SOURCES
                     ${CMAKE_CURRENT_SOURCE_DIR}/cdp_request_tracker.cpp)

get_property(WEBSHOT_CPP_TESTS GLOBAL PROPERTY WEBSHOT_CPP_TESTS)
set(WEBSHOT_COVERAGE_TARGETS ${PROJECT_NAME} ${WEBSHOT_CPP_TESTS})
set(WEBSHOT_COVERAGE_TARGET_FILES "")
foreach(tgt IN LISTS WEBSHOT_COVERAGE_TARGETS)
  list(APPEND WEBSHOT_COVERAGE_TARGET_FILES $<TARGET_FILE:${tgt}>)
endforeach()

if(USERVER_FEATURE_TESTSUITE)
  add_custom_target(testsuite)
  add_dependencies(testsuite ${PROJECT_NAME})
  set(testsuite_runner_path ${CMAKE_CURRENT_BINARY_DIR}/runtests-testsuite-tests)

  userver_testsuite_add(
    SERVICE_TARGET
    testsuite
    TEST_SUFFIX
    tests
    $<$<BOOL:WEBSHOT_ENABLE_SQL_COVERAGE>:SQL_LIBRARY>
    $<$<BOOL:WEBSHOT_ENABLE_SQL_COVERAGE>:${PROJECT_NAME}_sql>
    WORKING_DIRECTORY
    ${CMAKE_SOURCE_DIR}
    PYTEST_ARGS
    -c
    ${CMAKE_CURRENT_SOURCE_DIR}/pytest.ini
    --service-config=${CMAKE_SOURCE_DIR}/config/static_config.yaml
    --service-config-vars=${CMAKE_SOURCE_DIR}/config/config_vars.testsuite.yaml
    --service-binary=${CMAKE_INSTALL_PREFIX}/webshotd/webshotd_wrapper
    --service-secdist=${CMAKE_SOURCE_DIR}/secret/test_secdist.json
    --service-source-dir=${CMAKE_SOURCE_DIR}
    --postgresql=postgresql://postgres@localhost:5432/postgres)

  add_custom_target(
    testsuite-runner-guard ALL
    COMMAND
      ${TESTSUITE_PYTHON_BINARY}
      ${CMAKE_CURRENT_SOURCE_DIR}/patch_testsuite_runner.py
      --runner=${testsuite_runner_path}
      --service-port=8080
      --monitor-port=8081
    DEPENDS testsuite
    VERBATIM)

  # Ensure the testsuite runner is generated as part of the default build
  # whenever tests (and testsuite support) are enabled, so ctest can run
  # testsuite-testsuite-tests without extra manual targets.
  add_custom_target(testsuite-build ALL)
  add_dependencies(testsuite-build testsuite-runner-guard)

  add_custom_target(
    coverage
    COMMAND ${CMAKE_COMMAND} -E remove_directory
            ${CMAKE_CURRENT_BINARY_DIR}/coverage
    COMMAND ${CMAKE_COMMAND} -E make_directory
            ${CMAKE_CURRENT_BINARY_DIR}/coverage
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target
            testsuite
    COMMAND
      ${CMAKE_COMMAND} -E env
      LLVM_PROFILE_FILE=${CMAKE_CURRENT_BINARY_DIR}/coverage/webshotd-%p.profraw
      ${CMAKE_CTEST_COMMAND} --output-on-failure
    COMMAND
      bash -lc
      "llvm-profdata merge -sparse ${CMAKE_CURRENT_BINARY_DIR}/coverage/webshotd-*.profraw -o ${CMAKE_CURRENT_BINARY_DIR}/coverage/coverage.profdata"
    COMMAND
      llvm-cov report ${WEBSHOT_COVERAGE_TARGET_FILES}
      -instr-profile=${CMAKE_CURRENT_BINARY_DIR}/coverage/coverage.profdata
      "-ignore-filename-regex=.*/test/.*|.*/config\\.(cpp|hpp)"
      -show-instantiation-summary
    USES_TERMINAL VERBATIM)

  # Skip coverage instrumentation for config glue; track behavior with the
  # reporting regex instead.
  set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/src/config.cpp
    PROPERTIES COMPILE_OPTIONS
               "-fno-profile-instr-generate;-fno-coverage-mapping")

  add_custom_target(
    coverage-html
    COMMAND ${CMAKE_COMMAND} -E make_directory
            ${CMAKE_CURRENT_BINARY_DIR}/coverage/html
    COMMAND
      llvm-cov show --format=html
      --output-dir=${CMAKE_CURRENT_BINARY_DIR}/coverage/html
      --instr-profile=${CMAKE_CURRENT_BINARY_DIR}/coverage/coverage.profdata
      ${WEBSHOT_COVERAGE_TARGET_FILES}
      "-ignore-filename-regex=.*/test/.*|.*/config\\.(cpp|hpp)"
      -show-instantiation-summary
    DEPENDS coverage
    USES_TERMINAL VERBATIM)

  add_dependencies(coverage ${PROJECT_NAME} ${WEBSHOT_CPP_TESTS})
endif()
