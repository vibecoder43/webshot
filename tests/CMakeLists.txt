set(WEBSHOT_TEST_DB_DSN
    "postgresql://postgres@localhost:15434/postgres"
    CACHE STRING "PostgreSQL DSN used by testsuite (--postgresql)")
set(WEBSHOT_LLVM_COV_BIN
    "llvm-cov-19"
    CACHE STRING "llvm-cov binary used for coverage reporting")
set(WEBSHOT_LLVM_PROFDATA_BIN
    "llvm-profdata-19"
    CACHE STRING "llvm-profdata binary used for coverage reporting")

function(webshot_add_cpp_test target)
  cmake_parse_arguments(ARGS "" "" "SOURCES;LIBS;INCLUDE_DIRS" ${ARGN})

  add_executable(${target} ${ARGS_SOURCES})
  target_link_libraries(
    ${target}
    PRIVATE userver::utest
            fmt::fmt
            ada::ada
            absl::str_format
            absl::strings
            absl::log
            absl::raw_hash_set
            ${ARGS_LIBS})
  target_include_directories(
    ${target} PRIVATE ${CMAKE_SOURCE_DIR}/src/include ${PROJECT_BINARY_DIR}/src
                      ${ARGS_INCLUDE_DIRS})
  apply_defaults(${target})
  add_test(NAME ${target} COMMAND ${target})
  set_tests_properties(${target} PROPERTIES ENVIRONMENT
                                            "LSAN_OPTIONS=detect_leaks=0")

  if(WEBSHOT_ENABLE_COVERAGE)
    target_compile_options(${target} PRIVATE ${WEBSHOT_COVERAGE_FLAGS})
    target_link_options(${target} PRIVATE ${WEBSHOT_COVERAGE_FLAGS})
  endif()
endfunction()

webshot_add_cpp_test(
  is_valid_webshot_url_test SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/is_valid_webshot_url_test.cpp
  ${CMAKE_SOURCE_DIR}/src/link.cpp ${CMAKE_SOURCE_DIR}/src/ip_utils.cpp)

webshot_add_cpp_test(
  s3_v4_client_test
  SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/s3_v4_client_test.cpp
  ${CMAKE_SOURCE_DIR}/src/s3/sigv4_signer.cpp
  ${CMAKE_SOURCE_DIR}/src/s3/s3_v4_client.cpp
  LIBS
  userver::core
  INCLUDE_DIRS
  ${CMAKE_SOURCE_DIR}/src)

webshot_add_cpp_test(
  webshot_cursor_test SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/webshot_cursor_test.cpp
  ${CMAKE_SOURCE_DIR}/src/webshot_cursor.cpp LIBS ${PROJECT_NAME}-chgen)

webshot_add_cpp_test(
  webshot_pagination_test
  SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/webshot_pagination_test.cpp
  ${CMAKE_SOURCE_DIR}/src/webshot_cursor.cpp
  ${CMAKE_SOURCE_DIR}/src/webshot_pagination.cpp
  LIBS
  ${PROJECT_NAME}-chgen)

webshot_add_cpp_test(
  webshot_prefix_pagination_test
  SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/webshot_prefix_pagination_test.cpp
  ${CMAKE_SOURCE_DIR}/src/webshot_cursor.cpp
  ${CMAKE_SOURCE_DIR}/src/webshot_prefix_pagination.cpp
  LIBS
  ${PROJECT_NAME}-chgen)

webshot_add_cpp_test(
  ip_utils_test SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/ip_utils_test.cpp
  ${CMAKE_SOURCE_DIR}/src/ip_utils.cpp)

webshot_add_cpp_test(
  host_policy_test
  SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/host_policy_test.cpp
  ${CMAKE_SOURCE_DIR}/src/host_policy.cpp
  ${CMAKE_SOURCE_DIR}/src/ip_utils.cpp
  LIBS
  userver::core
  INCLUDE_DIRS
  ${CMAKE_SOURCE_DIR}/src)

webshot_add_cpp_test(
  container_guard_test SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/container_guard_test.cpp
  ${CMAKE_SOURCE_DIR}/src/container_guard.cpp LIBS userver::core)

webshot_add_cpp_test(
  error_utils_test SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/error_utils_test.cpp
  ${CMAKE_SOURCE_DIR}/src/error_utils.cpp LIBS ${PROJECT_NAME}-chgen)

add_executable(docker ${CMAKE_CURRENT_SOURCE_DIR}/docker_stub_main.cpp)
apply_defaults(docker)

if(USERVER_FEATURE_TESTSUITE)
  add_custom_target(webshot-testsuite)
  add_dependencies(webshot-testsuite ${PROJECT_NAME})

  userver_testsuite_add(
    SERVICE_TARGET
    webshot-testsuite
    TEST_SUFFIX
    tests
    WORKING_DIRECTORY
    ${CMAKE_CURRENT_SOURCE_DIR}
    PYTEST_ARGS
    --service-config=${CMAKE_SOURCE_DIR}/userver_config.yaml
    --service-binary=$<TARGET_FILE:${PROJECT_NAME}>
    --service-secdist=${CMAKE_SOURCE_DIR}/secrets/secdist.json
    --service-source-dir=${CMAKE_SOURCE_DIR}
    --postgresql=${WEBSHOT_TEST_DB_DSN})

  add_custom_target(
    webshot-coverage
    COMMAND ${CMAKE_COMMAND} -E make_directory
            ${CMAKE_CURRENT_BINARY_DIR}/coverage
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target
            webshot-testsuite
    COMMAND
      ${CMAKE_COMMAND} -E env
      LLVM_PROFILE_FILE=${CMAKE_CURRENT_BINARY_DIR}/coverage/webshot-%p.profraw
      ${CMAKE_CTEST_COMMAND} --output-on-failure
    COMMAND
      bash -lc
      "${WEBSHOT_LLVM_PROFDATA_BIN} merge -sparse ${CMAKE_CURRENT_BINARY_DIR}/coverage/webshot-*.profraw -o ${CMAKE_CURRENT_BINARY_DIR}/coverage/webshot.profdata"
    COMMAND
      ${WEBSHOT_LLVM_COV_BIN} report $<TARGET_FILE:${PROJECT_NAME}>
      $<TARGET_FILE:is_valid_webshot_url_test> $<TARGET_FILE:s3_v4_client_test>
      $<TARGET_FILE:webshot_cursor_test> $<TARGET_FILE:webshot_pagination_test>
      $<TARGET_FILE:webshot_prefix_pagination_test> $<TARGET_FILE:ip_utils_test>
      $<TARGET_FILE:host_policy_test> $<TARGET_FILE:container_guard_test>
      -instr-profile=${CMAKE_CURRENT_BINARY_DIR}/coverage/webshot.profdata
      "-ignore-filename-regex=.*/tests/.*"
    USES_TERMINAL VERBATIM)

  add_dependencies(
    webshot-coverage
    ${PROJECT_NAME}
    is_valid_webshot_url_test
    s3_v4_client_test
    webshot_cursor_test
    webshot_pagination_test
    webshot_prefix_pagination_test
    ip_utils_test
    host_policy_test
    container_guard_test)
endif()
