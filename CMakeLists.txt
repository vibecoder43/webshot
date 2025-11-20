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

target_sources(
  ${PROJECT_NAME}_objs
  PRIVATE src/s3/sigv4_signer.cpp src/s3/s3_v4_client.cpp
          src/include/s3_v4_client.hpp src/s3/sigv4_signer.hpp)
include(CTest)
if(BUILD_TESTING)
  add_subdirectory(tests)
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
