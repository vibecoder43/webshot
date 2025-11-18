cmake_minimum_required(VERSION 3.25)
project(webshot CXX)

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
endif()

if(USE_SANITIZERS)
  target_compile_options(${PROJECT_NAME} PRIVATE "-fsanitize=address,undefined")
  target_link_options(${PROJECT_NAME} PRIVATE "-fsanitize=address,undefined")
  # target_compile_options(${TARGET} PRIVATE "-fsanitize=undefined,thread")
  # target_link_options(${TARGET} PRIVATE "-fsanitize=undefined,thread")
endif()

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
