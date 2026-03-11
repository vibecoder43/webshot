cmake_minimum_required(VERSION 3.29)
project(webshotd CXX)

include(GNUInstallDirs)

get_filename_component(WEBSHOT_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# Ensure userver codegen target is created only once even with multiple
# generators (SQL, chaotic, testsuite). Pre-initialize the global property that
# guards userver-codegen creation.
set_property(GLOBAL PROPERTY userver_codegen_targets "initialized")

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
set(WEBSHOT_RAPIDOC_ASSETS_DIR
    ""
    CACHE STRING
          "Absolute path to pinned RapiDoc assets")

option(WEBSHOT_ENABLE_SQL_COVERAGE "Enable pytest sql_coverage includetest" ON)

# Ensure that userver integration does not create Python virtual environments
# when this service is configured. Testsuite and SQL/chaotic helpers should use
# the system Python specified by USERVER_PYTHON_PATH instead.
set(USERVER_TESTSUITE_USE_VENV
    OFF
    CACHE BOOL "Disable userver testsuite Python virtualenvs" FORCE)
set(USERVER_SQL_USE_VENV
    OFF
    CACHE BOOL "Disable userver SQL helper Python virtualenvs" FORCE)
set(USERVER_CHAOTIC_USE_VENV
    OFF
    CACHE BOOL "Disable userver chaotic helper Python virtualenvs" FORCE)

set(CMAKE_LINKER_TYPE MOLD)

# Ensure the installed `webshotd` binary is runnable without extra wrappers like
# `LD_LIBRARY_PATH` by embedding runtime search paths for linked dependencies.
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)

set(${PROJECT_NAME}_headers
    include/s3_credentials_types.hpp
    include/s3/s3_sts_client.hpp
    include/s3/s3_v4_client.hpp
    include/text_postgres_formatter.hpp
    include/handler.hpp
    include/job_handler.hpp
    include/by_prefix_handler.hpp
    include/by_id_handler.hpp
    include/disallow_and_purge_handler.hpp
    include/denylist_check_handler.hpp
    include/docs_handler.hpp
    include/crud.hpp
    include/crawlerd_client.hpp
    include/config.hpp
    include/crawler_failure.hpp
    include/error_utils.hpp
    include/http_utils.hpp
    include/denylist.hpp
    include/link.hpp
    include/cursor.hpp
    include/pagination.hpp
    include/prefix_pagination.hpp
    include/prefix_utils.hpp
    include/text.hpp)
add_library(
  ${PROJECT_NAME}_objs OBJECT
  src/handler.cpp
  src/job_handler.cpp
  src/by_prefix_handler.cpp
  src/by_id_handler.cpp
  src/disallow_and_purge_handler.cpp
  src/denylist_check_handler.cpp
  src/docs_handler.cpp
  src/crud.cpp
  src/crawlerd_client.cpp
  src/crawler_failure.cpp
  src/text_postgres_formatter.cpp
  src/error_utils.cpp
  src/denylist.cpp
  src/link.cpp
  src/config.cpp
  src/cursor.cpp
  src/pagination.cpp
  src/prefix_pagination.cpp
  src/prefix_utils.cpp
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
                                           $<$<CONFIG:Release>:-O2>)
  if(USE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(${target} PRIVATE -O1 -fno-omit-frame-pointer)
  endif()
endfunction()

add_executable(${PROJECT_NAME} src/main.cpp
                               $<TARGET_OBJECTS:${PROJECT_NAME}_objs>)
apply_defaults(${PROJECT_NAME}_objs)
apply_defaults(${PROJECT_NAME})
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME webshotd)

if(WEBSHOT_RAPIDOC_ASSETS_DIR STREQUAL "")
  message(FATAL_ERROR "WEBSHOT_RAPIDOC_ASSETS_DIR must be configured")
endif()

target_precompile_headers(${PROJECT_NAME}_objs PRIVATE include/text.hpp)

if(WEBSHOT_ENABLE_COVERAGE)
  target_compile_options(${PROJECT_NAME}_objs PRIVATE ${WEBSHOT_COVERAGE_FLAGS})
  target_link_options(${PROJECT_NAME} PRIVATE ${WEBSHOT_COVERAGE_FLAGS})
endif()

find_package(Boost CONFIG REQUIRED COMPONENTS context)
find_package(userver CONFIG REQUIRED COMPONENTS core postgresql chaotic s3api)
find_package(fmt CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME}_objs PRIVATE fmt::fmt)
find_package(ada CONFIG REQUIRED COMPONENTS ada)
target_link_libraries(${PROJECT_NAME}_objs PRIVATE ada::ada)
find_package(uni-algo CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME}_objs PRIVATE uni-algo::uni-algo)
find_package(absl CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME}_objs PRIVATE absl::strings)
target_link_libraries(${PROJECT_NAME}_objs
                      PUBLIC userver::core userver::postgresql userver::s3api
                             arkhiv::arkhiv)
target_link_libraries(${PROJECT_NAME} PRIVATE arkhiv::arkhiv)
target_sources(
  ${PROJECT_NAME}_objs
  PRIVATE src/s3/sigv4_signer.cpp src/s3/s3_v4_client.cpp
          src/s3/s3_sts_client.cpp src/s3/s3_url_utils.cpp
          include/s3/sigv4_signer.hpp include/s3/s3_url_utils.hpp)

userver_add_sql_library(
  ${PROJECT_NAME}_sql
  NAMESPACE
  webshot
  SOURCE_DIR
  sql/query
  OUTPUT_DIR
  ${CMAKE_CURRENT_BINARY_DIR}
  SQL_FILES
  *.sql)
target_link_libraries(${PROJECT_NAME}_objs PRIVATE ${PROJECT_NAME}_sql)
target_link_libraries(${PROJECT_NAME} PRIVATE ${PROJECT_NAME}_sql)

include(CTest)
add_subdirectory(${WEBSHOT_REPO_ROOT}/arkhiv ${CMAKE_CURRENT_BINARY_DIR}/arkhiv)
if(BUILD_TESTING)
  add_subdirectory(test)
endif()

if(USE_SANITIZERS)
  target_compile_options(${PROJECT_NAME} PRIVATE "-fsanitize=address,undefined")
  target_link_options(${PROJECT_NAME} PRIVATE "-fsanitize=address,undefined")
endif()

set(SCHEMAS
    ${WEBSHOT_REPO_ROOT}/schema/crawlerd.yaml
    ${WEBSHOT_REPO_ROOT}/schema/webshot.yaml)
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
  ${WEBSHOT_REPO_ROOT})
apply_defaults(${PROJECT_NAME}-chgen)
target_link_libraries(
  ${PROJECT_NAME}
  PRIVATE ${PROJECT_NAME}-chgen
          fmt::fmt
          ada::ada
          userver::core
          userver::postgresql
          userver::s3api
          absl::strings
          uni-algo::uni-algo)
target_include_directories(
  ${PROJECT_NAME}_objs PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/src/
                               ${CMAKE_CURRENT_SOURCE_DIR}/include)

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/config/"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/webshot/config")
install(DIRECTORY "${WEBSHOT_REPO_ROOT}/schema/"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/webshot/schema")
