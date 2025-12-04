set(WEBSHOT_LLVM_COV_BIN
    "llvm-cov"
    CACHE STRING "llvm-cov binary used for coverage reporting")
set(WEBSHOT_LLVM_PROFDATA_BIN
    "llvm-profdata"
    CACHE STRING "llvm-profdata binary used for coverage reporting")

function(webshot_add_cpp_test target)
  cmake_parse_arguments(ARGS "" "" "SOURCES;LIBS;INCLUDE_DIRS" ${ARGN})

  add_executable(${target} ${ARGS_SOURCES})
  target_link_libraries(
    ${target}
    PRIVATE userver::utest
            fmt::fmt
            ada::ada
            uni-algo::uni-algo
            absl::str_format
            absl::strings
            absl::log
            absl::raw_hash_set
            ${ARGS_LIBS})
  target_include_directories(
    ${target} PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/include
                      ${PROJECT_BINARY_DIR}/src ${ARGS_INCLUDE_DIRS})
  apply_defaults(${target})
  add_test(NAME ${target} COMMAND ${target})
  set(test_env "PATH=${CMAKE_CURRENT_BINARY_DIR}:$ENV{PATH}")
  # Keep leak reports clean: rely on project-wide LSAN suppression file.
  set(test_env
      "${test_env};LSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/lsan.supp")
  set_tests_properties(${target} PROPERTIES ENVIRONMENT "${test_env}")

  if(WEBSHOT_ENABLE_COVERAGE)
    target_compile_options(${target} PRIVATE ${WEBSHOT_COVERAGE_FLAGS})
    target_link_options(${target} PRIVATE ${WEBSHOT_COVERAGE_FLAGS})
  endif()

  set_property(GLOBAL APPEND PROPERTY WEBSHOT_CPP_TESTS ${target})
endfunction()

webshot_add_cpp_test(
  link_test SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/link_test.cpp
  ${CMAKE_SOURCE_DIR}/src/link.cpp ${CMAKE_SOURCE_DIR}/src/ip_utils.cpp)

webshot_add_cpp_test(
  s3_v4_client_test
  SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/s3_v4_client_test.cpp
  ${CMAKE_SOURCE_DIR}/src/s3/sigv4_signer.cpp
  ${CMAKE_SOURCE_DIR}/src/s3/s3_v4_client.cpp
  ${CMAKE_SOURCE_DIR}/src/link.cpp
  ${CMAKE_SOURCE_DIR}/src/ip_utils.cpp
  LIBS
  userver::core
  INCLUDE_DIRS
  ${CMAKE_SOURCE_DIR}/src)

webshot_add_cpp_test(
  s3_url_utils_test SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/s3_url_utils_test.cpp
  ${CMAKE_SOURCE_DIR}/src/s3/s3_url_utils.cpp)

webshot_add_cpp_test(
  s3_sts_client_test
  SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/s3_sts_client_test.cpp
  ${CMAKE_SOURCE_DIR}/src/s3/s3_sts_client.cpp
  ${CMAKE_SOURCE_DIR}/src/s3/sigv4_signer.cpp
  ${CMAKE_SOURCE_DIR}/src/s3/s3_url_utils.cpp
  ${CMAKE_SOURCE_DIR}/src/link.cpp
  ${CMAKE_SOURCE_DIR}/src/ip_utils.cpp
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

webshot_add_cpp_test(text_test SOURCES
                     ${CMAKE_CURRENT_SOURCE_DIR}/text_test.cpp)

webshot_add_cpp_test(
  s3_secdist_test SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/s3_secdist_test.cpp LIBS
  userver::core)

webshot_add_cpp_test(s3_refresh_schedule_test SOURCES
                     ${CMAKE_CURRENT_SOURCE_DIR}/s3_refresh_schedule_test.cpp)

webshot_add_cpp_test(deadline_utils_test SOURCES
                     ${CMAKE_CURRENT_SOURCE_DIR}/deadline_utils_test.cpp)

webshot_add_cpp_test(
  handler_simple_server_test SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/handler_simple_server_test.cpp LIBS userver::core)

get_property(WEBSHOT_CPP_TESTS GLOBAL PROPERTY WEBSHOT_CPP_TESTS)
set(WEBSHOT_COVERAGE_TARGETS ${PROJECT_NAME} ${WEBSHOT_CPP_TESTS})
set(WEBSHOT_COVERAGE_TARGET_FILES "")
foreach(tgt IN LISTS WEBSHOT_COVERAGE_TARGETS)
  list(APPEND WEBSHOT_COVERAGE_TARGET_FILES $<TARGET_FILE:${tgt}>)
endforeach()

add_executable(docker ${CMAKE_CURRENT_SOURCE_DIR}/docker_stub_main.cpp)
apply_defaults(docker)
set_target_properties(docker PROPERTIES OUTPUT_NAME podman)

if(USERVER_FEATURE_TESTSUITE)
  add_custom_target(webshot-testsuite)
  add_dependencies(webshot-testsuite ${PROJECT_NAME} docker)

  userver_testsuite_add(
    SERVICE_TARGET
    webshot-testsuite
    TEST_SUFFIX
    tests
    $<$<BOOL:WEBSHOT_ENABLE_SQL_COVERAGE>:SQL_LIBRARY>
    $<$<BOOL:WEBSHOT_ENABLE_SQL_COVERAGE>:${PROJECT_NAME}_sql>
    WORKING_DIRECTORY
    ${CMAKE_CURRENT_SOURCE_DIR}
    PYTEST_ARGS
    --service-config=${CMAKE_SOURCE_DIR}/config/userver_config.yaml
    --service-config-vars=${CMAKE_SOURCE_DIR}/config/config_vars.testsuite.yaml
    --service-binary=$<TARGET_FILE:${PROJECT_NAME}>
    --service-secdist=${CMAKE_SOURCE_DIR}/secrets/test_secdist.json
    --service-source-dir=${CMAKE_SOURCE_DIR}
    --postgresql=postgresql://postgres@localhost:5432/postgres)

  # Ensure the testsuite runner is generated as part of the default build
  # whenever tests (and testsuite support) are enabled, so ctest can run
  # testsuite-webshot-testsuite-tests without extra manual targets.
  add_custom_target(webshot-testsuite-build ALL)
  add_dependencies(webshot-testsuite-build webshot-testsuite)

  add_custom_target(
    webshot-coverage
    COMMAND ${CMAKE_COMMAND} -E remove_directory
            ${CMAKE_CURRENT_BINARY_DIR}/coverage
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
      ${WEBSHOT_LLVM_COV_BIN} report ${WEBSHOT_COVERAGE_TARGET_FILES}
      -instr-profile=${CMAKE_CURRENT_BINARY_DIR}/coverage/webshot.profdata
      "-ignore-filename-regex=.*/tests/.*|.*/webshot_config\\.(cpp|hpp)"
      -show-instantiation-summary
    USES_TERMINAL VERBATIM)

  # Skip coverage instrumentation for config glue; track behavior with the
  # reporting regex instead.
  set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/src/webshot_config.cpp
    PROPERTIES COMPILE_OPTIONS
               "-fno-profile-instr-generate;-fno-coverage-mapping")

  add_custom_target(
    webshot-coverage-html
    COMMAND ${CMAKE_COMMAND} -E make_directory
            ${CMAKE_CURRENT_BINARY_DIR}/coverage/html
    COMMAND
      ${WEBSHOT_LLVM_COV_BIN} show --format=html
      --output-dir=${CMAKE_CURRENT_BINARY_DIR}/coverage/html
      --instr-profile=${CMAKE_CURRENT_BINARY_DIR}/coverage/webshot.profdata
      ${WEBSHOT_COVERAGE_TARGET_FILES}
      "-ignore-filename-regex=.*/tests/.*|.*/webshot_config\\.(cpp|hpp)"
      -show-instantiation-summary
    DEPENDS webshot-coverage
    USES_TERMINAL VERBATIM)

  add_dependencies(webshot-coverage ${PROJECT_NAME} docker ${WEBSHOT_CPP_TESTS})
endif()

get_property(
  WEBSHOT_ALL_TESTS
  DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  PROPERTY TESTS)
if(WEBSHOT_ALL_TESTS)
  foreach(t ${WEBSHOT_ALL_TESTS})
    if(WEBSHOT_ENABLE_LLVM_SYMBOLIZER AND WEBSHOT_LLVM_SYMBOLIZER_BIN)
      set_property(
        TEST ${t}
        APPEND
        PROPERTY ENVIRONMENT
                 "ASAN_SYMBOLIZER_PATH=${WEBSHOT_LLVM_SYMBOLIZER_BIN}")
      set_property(
        TEST ${t}
        APPEND
        PROPERTY
          ENVIRONMENT
          "ASAN_OPTIONS=symbolize=1:external_symbolizer_path=${WEBSHOT_LLVM_SYMBOLIZER_BIN}"
      )
    endif()
  endforeach()
endif()
