cmake_minimum_required(VERSION 3.25)
project(webshot CXX)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(
    FATAL_ERROR
      "In-source builds are not supported. Please create a separate build directory and run CMake from there."
  )
endif()

set(WEBSHOT_ENABLE_COVERAGE
    OFF
    CACHE BOOL
          "Enable LLVM coverage instrumentation for the webshot service binary")
set(WEBSHOT_COVERAGE_FLAGS "-fprofile-instr-generate" "-fcoverage-mapping")

set(${PROJECT_NAME}_headers
    src/include/webshot_handler.hpp
    src/include/webshots_by_prefix_handler.hpp
    src/include/webshot_by_id_handler.hpp
    src/include/webshot_disallow_and_purge_handler.hpp
    src/include/webshot_crud.hpp
    src/include/webshot_config.hpp
    src/include/error_utils.hpp
    src/include/http_utils.hpp
    src/include/webshot_denylist.hpp
    src/include/link.hpp
    src/include/host_policy.hpp
    src/include/ip_utils.hpp
    src/include/webshot_cursor.hpp
    src/include/webshot_pagination.hpp
    src/include/webshot_prefix_pagination.hpp)
add_library(
  ${PROJECT_NAME}_objs OBJECT
  src/webshot_handler.cpp
  src/webshots_by_prefix_handler.cpp
  src/webshot_by_id_handler.cpp
  src/webshot_disallow_and_purge_handler.cpp
  src/webshot_crud.cpp
  src/error_utils.cpp
  src/webshot_denylist.cpp
  src/link.cpp
  src/container_guard.cpp
  src/webshot_config.cpp
  src/host_policy.cpp
  src/ip_utils.cpp
  src/webshot_cursor.cpp
  src/webshot_pagination.cpp
  src/webshot_prefix_pagination.cpp
  ${${PROJECT_NAME}_headers})

function(apply_defaults target)
  set_property(TARGET ${target} PROPERTY CXX_STANDARD 20)
  set_property(TARGET ${target} PROPERTY CXX_STANDARD_REQUIRED ON)
  set_property(TARGET ${target} PROPERTY CXX_EXTENSIONS OFF)
  target_compile_options(
    ${target}
    PRIVATE -Wall
            -Wextra
            -Wpedantic
            -Werror
            -Wconversion
            -Wsign-conversion
            -Wfloat-equal
            -Wdisabled-optimization
            -Wctad-maybe-unsupported)
  target_compile_options(${target} PRIVATE $<$<CONFIG:Debug>:-ggdb3>
                                           $<$<CONFIG:Release>:-O3>)
  set_property(TARGET ${target} PROPERTY CXX_COMPILER_LAUNCHER ccache)
endfunction()

add_executable(${PROJECT_NAME} src/main.cpp
                               $<TARGET_OBJECTS:${PROJECT_NAME}_objs>)
apply_defaults(${PROJECT_NAME}_objs)
apply_defaults(${PROJECT_NAME})

if(WEBSHOT_ENABLE_COVERAGE)
  target_compile_options(${PROJECT_NAME}_objs PRIVATE ${WEBSHOT_COVERAGE_FLAGS})
  target_link_options(${PROJECT_NAME} PRIVATE ${WEBSHOT_COVERAGE_FLAGS})
endif()

find_package(userver CONFIG REQUIRED COMPONENTS core postgresql chaotic s3api)
find_package(fmt CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME}_objs PRIVATE fmt::fmt)
find_package(ada CONFIG REQUIRED COMPONENTS ada)
target_link_libraries(${PROJECT_NAME}_objs PRIVATE ada::ada)
# RE2 (static) on Debian/Ubuntu links against Abseil. Pull Abseil targets
# explicitly so lld can resolve symbols exported by libre2.a.
find_package(absl CONFIG REQUIRED)
# RE2's static lib on Debian/Ubuntu depends on Abseil; propagate to consumers of
# the object lib
target_link_libraries(
  ${PROJECT_NAME}_objs PUBLIC absl::str_format absl::strings absl::log
                              absl::raw_hash_set)
target_link_libraries(${PROJECT_NAME}_objs
                      PUBLIC userver::core userver::postgresql userver::s3api)
target_sources(
  ${PROJECT_NAME}_objs
  PRIVATE src/s3/sigv4_signer.cpp src/s3/s3_v4_client.cpp
          src/include/s3_v4_client.hpp src/s3/sigv4_signer.hpp)

include(CTest)
if(BUILD_TESTING)

  set(WEBSHOT_TEST_DB_DSN
      "postgresql://postgres@localhost:15434/postgres"
      CACHE STRING "PostgreSQL DSN used by testsuite (--postgresql)")
  set(WEBSHOT_LLVM_COV_BIN
      "llvm-cov-19"
      CACHE STRING "llvm-cov binary used for coverage reporting")
  set(WEBSHOT_LLVM_PROFDATA_BIN
      "llvm-profdata-19"
      CACHE STRING "llvm-profdata binary used for coverage reporting")
  add_executable(is_valid_webshot_url_test tests/is_valid_webshot_url_test.cpp
                                           src/link.cpp src/ip_utils.cpp)
  target_link_libraries(
    is_valid_webshot_url_test
    PRIVATE userver::utest
            ada::ada
            fmt::fmt
            absl::str_format
            absl::strings
            absl::log
            absl::raw_hash_set)
  target_include_directories(is_valid_webshot_url_test PRIVATE src/include)
  apply_defaults(is_valid_webshot_url_test)
  add_test(NAME is_valid_webshot_url COMMAND is_valid_webshot_url_test)
  set_tests_properties(is_valid_webshot_url
                       PROPERTIES ENVIRONMENT "LSAN_OPTIONS=detect_leaks=0")

  add_executable(s3_v4_client_test tests/s3_v4_client_test.cpp
                                   src/s3/sigv4_signer.cpp)
  target_link_libraries(
    s3_v4_client_test PRIVATE userver::utest fmt::fmt absl::str_format
                              absl::strings absl::log absl::raw_hash_set)
  target_include_directories(s3_v4_client_test PRIVATE src src/include)
  apply_defaults(s3_v4_client_test)
  add_test(NAME s3_v4_client COMMAND s3_v4_client_test)
  set_tests_properties(s3_v4_client PROPERTIES ENVIRONMENT
                                               "LSAN_OPTIONS=detect_leaks=0")

  add_executable(webshot_cursor_test tests/webshot_cursor_test.cpp
                                     src/webshot_cursor.cpp)
  target_link_libraries(
    webshot_cursor_test
    PRIVATE userver::utest
            fmt::fmt
            absl::str_format
            absl::strings
            absl::log
            absl::raw_hash_set
            ${PROJECT_NAME}-chgen)
  target_include_directories(
    webshot_cursor_test PRIVATE src/include ${CMAKE_CURRENT_BINARY_DIR}/src/)
  apply_defaults(webshot_cursor_test)
  add_test(NAME webshot_cursor COMMAND webshot_cursor_test)
  set_tests_properties(webshot_cursor PROPERTIES ENVIRONMENT
                                                 "LSAN_OPTIONS=detect_leaks=0")

  add_executable(
    webshot_pagination_test tests/webshot_pagination_test.cpp
                            src/webshot_cursor.cpp src/webshot_pagination.cpp)
  target_link_libraries(
    webshot_pagination_test
    PRIVATE userver::utest
            fmt::fmt
            absl::str_format
            absl::strings
            absl::log
            absl::raw_hash_set
            ${PROJECT_NAME}-chgen)
  target_include_directories(
    webshot_pagination_test PRIVATE src/include
                                    ${CMAKE_CURRENT_BINARY_DIR}/src/)
  apply_defaults(webshot_pagination_test)
  add_test(NAME webshot_pagination COMMAND webshot_pagination_test)
  set_tests_properties(webshot_pagination
                       PROPERTIES ENVIRONMENT "LSAN_OPTIONS=detect_leaks=0")

  add_executable(
    webshot_prefix_pagination_test
    tests/webshot_prefix_pagination_test.cpp src/webshot_cursor.cpp
    src/webshot_prefix_pagination.cpp)
  target_link_libraries(
    webshot_prefix_pagination_test
    PRIVATE userver::utest
            fmt::fmt
            absl::str_format
            absl::strings
            absl::log
            absl::raw_hash_set
            ${PROJECT_NAME}-chgen)
  target_include_directories(
    webshot_prefix_pagination_test PRIVATE src/include
                                           ${CMAKE_CURRENT_BINARY_DIR}/src/)
  apply_defaults(webshot_prefix_pagination_test)
  add_test(NAME webshot_prefix_pagination
           COMMAND webshot_prefix_pagination_test)
  set_tests_properties(webshot_prefix_pagination
                       PROPERTIES ENVIRONMENT "LSAN_OPTIONS=detect_leaks=0")

  if(WEBSHOT_ENABLE_COVERAGE)
    foreach(test_target
            is_valid_webshot_url_test s3_v4_client_test webshot_cursor_test
            webshot_pagination_test webshot_prefix_pagination_test)
      if(TARGET ${test_target})
        target_compile_options(${test_target} PRIVATE ${WEBSHOT_COVERAGE_FLAGS})
        target_link_options(${test_target} PRIVATE ${WEBSHOT_COVERAGE_FLAGS})
      endif()
    endforeach()
  endif()

  if(USERVER_FEATURE_TESTSUITE)
    add_custom_target(webshot-testsuite)
    add_dependencies(webshot-testsuite ${PROJECT_NAME})

    userver_testsuite_add(
      SERVICE_TARGET
      webshot-testsuite
      TEST_SUFFIX
      tests
      WORKING_DIRECTORY
      ${CMAKE_CURRENT_SOURCE_DIR}/tests
      PYTEST_ARGS
      --service-config=${CMAKE_CURRENT_SOURCE_DIR}/userver_config.yaml
      --service-binary=$<TARGET_FILE:${PROJECT_NAME}>
      --service-secdist=${CMAKE_CURRENT_SOURCE_DIR}/secrets/secdist.json
      --service-source-dir=${CMAKE_CURRENT_SOURCE_DIR}
      --postgresql=${WEBSHOT_TEST_DB_DSN})

    # Aggregate C++ coverage for server sources (including unit tests) in one
    # command.
    add_custom_target(
      webshot-coverage
      COMMAND ${CMAKE_COMMAND} -E make_directory
              ${CMAKE_CURRENT_BINARY_DIR}/coverage
      # Ensure testsuite runner scripts are generated.
      COMMAND ${CMAKE_COMMAND} --build ${CMAKE_CURRENT_BINARY_DIR} --target
              webshot-testsuite
      # Run all CTest tests (unit tests + testsuite) under coverage
      # instrumentation.
      COMMAND
        ${CMAKE_COMMAND} -E env
        LLVM_PROFILE_FILE=${CMAKE_CURRENT_BINARY_DIR}/coverage/webshot-%p.profraw
        ${CMAKE_CTEST_COMMAND} --output-on-failure
      # Merge raw profiles (use a shell so the glob expands).
      COMMAND
        bash -lc
        "${WEBSHOT_LLVM_PROFDATA_BIN} merge -sparse ${CMAKE_CURRENT_BINARY_DIR}/coverage/webshot-*.profraw -o ${CMAKE_CURRENT_BINARY_DIR}/coverage/webshot.profdata"
      # Print a textual coverage summary for service sources only; ignore
      # coverage from tests/.
      COMMAND
        ${WEBSHOT_LLVM_COV_BIN} report $<TARGET_FILE:${PROJECT_NAME}>
        $<TARGET_FILE:is_valid_webshot_url_test>
        $<TARGET_FILE:s3_v4_client_test> $<TARGET_FILE:webshot_cursor_test>
        $<TARGET_FILE:webshot_pagination_test>
        $<TARGET_FILE:webshot_prefix_pagination_test>
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
      webshot_prefix_pagination_test)
  endif()
endif()

if(USE_SANITIZERS)
  target_compile_options(${PROJECT_NAME} PRIVATE "-fsanitize=address,undefined")
  target_link_options(${PROJECT_NAME} PRIVATE "-fsanitize=address,undefined")
  # target_compile_options(${TARGET} PRIVATE "-fsanitize=undefined,thread")
  # target_link_options(${TARGET} PRIVATE "-fsanitize=undefined,thread")
endif()

file(GLOB SQL_QUERY_FILES ${CMAKE_CURRENT_SOURCE_DIR}/sql/queries/*.sql)
add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/src/sql_gen.hpp
  COMMAND
    python3 ${CMAKE_CURRENT_SOURCE_DIR}/sql/gen_sql_header.py --input-dir
    ${CMAKE_CURRENT_SOURCE_DIR}/sql/queries --output
    ${CMAKE_CURRENT_BINARY_DIR}/src/sql_gen.hpp
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/sql/gen_sql_header.py ${SQL_QUERY_FILES}
  COMMENT "Generating SQL header from sql/queries"
  VERBATIM)
add_custom_target(sql_gen DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/src/sql_gen.hpp)
add_dependencies(${PROJECT_NAME}_objs sql_gen)
add_dependencies(${PROJECT_NAME} sql_gen)

file(GLOB_RECURSE SCHEMAS ${CMAKE_CURRENT_SOURCE_DIR}/schemas/*.yaml)
userver_target_generate_chaotic(
  ${PROJECT_NAME}-chgen
  GENERATE_SERIALIZERS
  LAYOUT
  "/components/schemas/([^/]*)/=dto::{0}"
  OUTPUT_DIR
  ${CMAKE_CURRENT_BINARY_DIR}/src
  SCHEMAS
  ${SCHEMAS}
  RELATIVE_TO
  ${CMAKE_CURRENT_SOURCE_DIR})
apply_defaults(${PROJECT_NAME}-chgen)
target_link_libraries(
  ${PROJECT_NAME}
  PRIVATE ${PROJECT_NAME}-chgen
          fmt::fmt
          ada::ada
          userver::core
          userver::postgresql
          userver::s3api
          absl::str_format
          absl::strings
          absl::log
          absl::raw_hash_set)
target_include_directories(${PROJECT_NAME}_objs
                           PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/src/)
