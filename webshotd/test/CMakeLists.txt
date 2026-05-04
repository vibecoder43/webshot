function(_add_cpp_test_target target)
  cmake_parse_arguments(ARGS "WITH_UTEST;REGISTER_CTEST;ENABLE_COVERAGE" ""
                        "SOURCES;LIBS;INCLUDE_DIRS" ${ARGN})

  set(linkLibs ada::ada uni-algo::uni-algo absl::strings ${ARGS_LIBS})
  if(ARGS_WITH_UTEST)
    list(PREPEND linkLibs userver::utest)
  endif()

  add_executable(${target} ${ARGS_SOURCES})
  target_link_libraries(${target} PRIVATE ${linkLibs})
  target_include_directories(
    ${target} PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/include
                      ${PROJECT_BINARY_DIR}/src ${ARGS_INCLUDE_DIRS})
  apply_defaults(${target})
  apply_link_defaults(${target})
  apply_sanitizer_compile_defaults(${target})
  apply_sanitizer_link_defaults(${target})

  if(ARGS_REGISTER_CTEST)
    add_test(NAME ${target} COMMAND ${target})
    set(test_env "LSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/lsan.supp")
    set_tests_properties(${target} PROPERTIES ENVIRONMENT "${test_env}")
  endif()

  if(ARGS_ENABLE_COVERAGE AND WEBSHOT_ENABLE_COVERAGE)
    target_compile_options(${target} PRIVATE ${WEBSHOT_COVERAGE_FLAGS})
    target_link_options(${target} PRIVATE ${WEBSHOT_COVERAGE_FLAGS})
  endif()
endfunction()

function(add_cpp_test target)
  _add_cpp_test_target(${target} WITH_UTEST REGISTER_CTEST ENABLE_COVERAGE
                       ${ARGN})
  set_property(GLOBAL APPEND PROPERTY WEBSHOT_CPP_TESTS ${target})
endfunction()

function(add_cpp_helper target)
  _add_cpp_test_target(${target} ${ARGN})
endfunction()

add_cpp_test(
  link_test SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/link.cpp
  ${CMAKE_SOURCE_DIR}/src/link.cpp ${CMAKE_SOURCE_DIR}/src/prefix_utils.cpp
  ${CMAKE_SOURCE_DIR}/src/url.cpp)

add_cpp_test(
  s3_v4_client_test
  SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/s3_v4_client.cpp
  ${CMAKE_SOURCE_DIR}/src/s3/sigv4_signer.cpp
  ${CMAKE_SOURCE_DIR}/src/s3/s3_v4_client.cpp
  ${CMAKE_SOURCE_DIR}/src/s3/s3_url_utils.cpp
  ${CMAKE_SOURCE_DIR}/src/link.cpp
  ${CMAKE_SOURCE_DIR}/src/url.cpp
  LIBS
  userver::core
  INCLUDE_DIRS
  ${CMAKE_SOURCE_DIR}/src)

add_cpp_test(
  s3_url_utils_test SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/s3_url_utils.cpp
  ${CMAKE_SOURCE_DIR}/src/s3/s3_url_utils.cpp ${CMAKE_SOURCE_DIR}/src/url.cpp)

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

add_cpp_test(cursor_test SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/cursor.cpp
             ${CMAKE_SOURCE_DIR}/src/cursor.cpp LIBS ${PROJECT_NAME}-chgen)

add_cpp_test(
  pagination_test
  SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/pagination.cpp
  ${CMAKE_SOURCE_DIR}/src/cursor.cpp
  ${CMAKE_SOURCE_DIR}/src/pagination.cpp
  LIBS
  ${PROJECT_NAME}-chgen)

add_cpp_test(
  prefix_pagination_test
  SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/prefix_pagination.cpp
  ${CMAKE_SOURCE_DIR}/src/cursor.cpp
  ${CMAKE_SOURCE_DIR}/src/prefix_pagination.cpp
  LIBS
  ${PROJECT_NAME}-chgen)

add_cpp_test(
  error_utils_test SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/error_utils.cpp
  ${CMAKE_SOURCE_DIR}/src/error_utils.cpp LIBS ${PROJECT_NAME}-chgen)

add_cpp_test(integers_test SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/integers.cpp)

add_cpp_test(text_test SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/text.cpp)

add_cpp_test(ip_test SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/ip.cpp)

add_cpp_test(s3_secdist_test SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/s3_secdist.cpp
             LIBS userver::core)

add_cpp_test(s3_refresh_schedule_test SOURCES
             ${CMAKE_CURRENT_SOURCE_DIR}/s3_refresh_schedule.cpp)

add_cpp_test(
  storage_url_test SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/storage_url.cpp
  ${CMAKE_SOURCE_DIR}/src/storage_url.cpp ${CMAKE_SOURCE_DIR}/src/url.cpp)

add_cpp_test(deadline_utils_test SOURCES
             ${CMAKE_CURRENT_SOURCE_DIR}/deadline_utils.cpp)

add_cpp_test(expected_test SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/expected.cpp)

add_cpp_test(try_test SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/try.cpp)

add_cpp_test(crawler_fallback_test SOURCES
             ${CMAKE_CURRENT_SOURCE_DIR}/crawler_fallback.cpp)

add_cpp_test(cdp_request_tracker_test SOURCES
             ${CMAKE_CURRENT_SOURCE_DIR}/cdp_request_tracker.cpp)

add_cpp_test(
  browser_page_lifecycle_test SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/browser_page_lifecycle.cpp
  ${CMAKE_SOURCE_DIR}/src/crawler/browser_page_lifecycle.cpp LIBS sml::sml)
target_compile_definitions(browser_page_lifecycle_test
                           PRIVATE BOOST_SML_CFG_DISABLE_MIN_SIZE)

get_property(WEBSHOT_CPP_TESTS GLOBAL PROPERTY WEBSHOT_CPP_TESTS)
set(WEBSHOT_COVERAGE_TARGETS ${PROJECT_NAME} ${WEBSHOT_CPP_TESTS})
set(WEBSHOT_COVERAGE_TARGET_FILES "")
foreach(tgt IN LISTS WEBSHOT_COVERAGE_TARGETS)
  list(APPEND WEBSHOT_COVERAGE_TARGET_FILES $<TARGET_FILE:${tgt}>)
endforeach()

if(USERVER_FEATURE_TESTSUITE)
  set(testsuite_static_config_path
      "${CMAKE_CURRENT_BINARY_DIR}/static_config.testsuite.yaml")
  set(testsuite_components_config_path
      "${CMAKE_CURRENT_SOURCE_DIR}/static_config.testsuite_components.yaml.inc")
  set_property(
    DIRECTORY
    APPEND
    PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_SOURCE_DIR}/config/static_config.yaml"
             "${testsuite_components_config_path}")
  set(testsuite_static_config_marker "    # WEBSHOT_TESTSUITE_COMPONENTS")
  file(READ "${testsuite_components_config_path}" testsuite_components_config)
  file(READ "${CMAKE_SOURCE_DIR}/config/static_config.yaml"
       testsuite_static_config)
  string(FIND "${testsuite_static_config}" "${testsuite_static_config_marker}"
              testsuite_static_config_marker_pos)
  if(testsuite_static_config_marker_pos LESS 0)
    message(
      FATAL_ERROR
        "Missing testsuite component marker in config/static_config.yaml")
  endif()
  string(REPLACE "${testsuite_static_config_marker}"
                 "${testsuite_components_config}" testsuite_static_config
                 "${testsuite_static_config}")
  webshot_write_file_if_different("${testsuite_static_config_path}"
                                  "${testsuite_static_config}")

  add_custom_target(testsuite)
  add_dependencies(testsuite ${PROJECT_NAME})
  set(testsuite_runner_path
      ${CMAKE_CURRENT_BINARY_DIR}/runtests-testsuite-tests)

  userver_testsuite_add(
    SERVICE_TARGET
    testsuite
    TEST_SUFFIX
    tests
    $<$<BOOL:${WEBSHOT_ENABLE_SQL_COVERAGE}>:SQL_LIBRARY>
    $<$<BOOL:${WEBSHOT_ENABLE_SQL_COVERAGE}>:${PROJECT_NAME}_sql>
    WORKING_DIRECTORY
    ${CMAKE_SOURCE_DIR}
    PYTEST_ARGS
    -c
    ${CMAKE_CURRENT_SOURCE_DIR}/pytest.ini
    --service-config=${testsuite_static_config_path}
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
      --runner=${testsuite_runner_path} --service-port=8080 --monitor-port=8081
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
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target testsuite
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
