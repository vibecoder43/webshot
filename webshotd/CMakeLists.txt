cmake_minimum_required(VERSION 3.29)
project(webshotd CXX)

include(GNUInstallDirs)

get_filename_component(WEBSHOT_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX
      "${CMAKE_BINARY_DIR}/runtime_root"
      CACHE PATH "Local runtime staging prefix" FORCE)
endif()

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

set_property(
  DIRECTORY
  APPEND
  PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/browser_sandbox.sh")

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/browser_sandbox.sh"
     WEBSHOT_BROWSER_SANDBOX_SH_CONTENT)
set(_webshot_browser_sandbox_delim "WSBSH")
set(_webshot_browser_sandbox_terminator ")${_webshot_browser_sandbox_delim}\"")
string(FIND "${WEBSHOT_BROWSER_SANDBOX_SH_CONTENT}"
            "${_webshot_browser_sandbox_terminator}"
            _webshot_browser_sandbox_terminator_pos)
if(NOT _webshot_browser_sandbox_terminator_pos EQUAL -1)
  message(
    FATAL_ERROR
      "browser_sandbox.sh contains raw-string terminator '${_webshot_browser_sandbox_terminator}'. Please change the delimiter in CMakeLists.txt.")
endif()

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/src/crawler")
file(
  WRITE "${CMAKE_CURRENT_BINARY_DIR}/src/crawler/browser_sandbox.sh.hpp"
  "#pragma once\n"
  "\n"
  "#include <string_view>\n"
  "\n"
  "namespace v1::crawler {\n"
  "\n"
  "inline constexpr std::string_view kBrowserSandboxScript = R\"${_webshot_browser_sandbox_delim}(\n"
  "${WEBSHOT_BROWSER_SANDBOX_SH_CONTENT}\n"
  ")${_webshot_browser_sandbox_delim}\";\n"
  "\n"
  "} // namespace v1::crawler\n")

set(${PROJECT_NAME}_headers
    include/integers.hpp
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
	    include/crawler/runner.hpp
	    include/crawler/artifacts.hpp
	    include/crawler/browser_sandbox.hpp
	    include/crawler/launch_policy.hpp
	    include/crawler/cdp_client.hpp
    include/config.hpp
    include/crawler/failure.hpp
    include/crawler/fallback.hpp
    include/error_utils.hpp
    include/http_utils.hpp
    include/ip_utils.hpp
    include/denylist.hpp
    include/link.hpp
    include/url.hpp
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
  src/url.cpp
  src/crawler/runner.cpp
  src/crawler/artifacts.cpp
  src/crawler/browser_sandbox.cpp
  src/crawler/cdp_client.cpp
  src/crawler/failure.cpp
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
add_custom_target(
  ${PROJECT_NAME}_stage_runtime_layout ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_INSTALL_PREFIX}/webshotd"
  COMMAND
    ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${PROJECT_NAME}>
    "${CMAKE_INSTALL_PREFIX}/webshotd/webshotd"
  COMMAND
    ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_BINARY_DIR}/CMakeCache.txt"
    "${CMAKE_INSTALL_PREFIX}/webshotd/CMakeCache.txt"
  DEPENDS ${PROJECT_NAME} "${CMAKE_BINARY_DIR}/CMakeCache.txt"
  VERBATIM)

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
                             )

if(CMAKE_CXX_INCLUDE_WHAT_YOU_USE)
  set(_webshot_iwyu_cmd ${CMAKE_CXX_INCLUDE_WHAT_YOU_USE})
  list(GET _webshot_iwyu_cmd 0 _webshot_iwyu_tool)
  find_program(_webshot_iwyu_path NAMES "${_webshot_iwyu_tool}")
  if(NOT _webshot_iwyu_path)
    message(
      FATAL_ERROR
        "CMAKE_CXX_INCLUDE_WHAT_YOU_USE is set to '${_webshot_iwyu_tool}', but include-what-you-use was not found in PATH"
    )
  endif()
  list(REMOVE_AT _webshot_iwyu_cmd 0)
  set(_webshot_iwyu_cmd "${_webshot_iwyu_path}" ${_webshot_iwyu_cmd})
  set_property(TARGET ${PROJECT_NAME}_objs PROPERTY CXX_INCLUDE_WHAT_YOU_USE
                                                    "${_webshot_iwyu_cmd}")
  set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_INCLUDE_WHAT_YOU_USE
                                               "${_webshot_iwyu_cmd}")

  # Prevent IWYU from running in subdirectories (arkhiv) and codegen targets.
  set(CMAKE_CXX_INCLUDE_WHAT_YOU_USE "")
endif()

add_subdirectory("${WEBSHOT_REPO_ROOT}/arkhiv" "${CMAKE_CURRENT_BINARY_DIR}/arkhiv")
target_link_libraries(${PROJECT_NAME}_objs PRIVATE arkhiv::arkhiv)
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
if(BUILD_TESTING)
  add_subdirectory(test)
endif()

if(USE_SANITIZERS)
  target_compile_options(${PROJECT_NAME} PRIVATE "-fsanitize=address,undefined")
  target_link_options(${PROJECT_NAME} PRIVATE "-fsanitize=address,undefined")
endif()

set(SCHEMAS
    ${WEBSHOT_REPO_ROOT}/schema/cdp.yaml
    ${WEBSHOT_REPO_ROOT}/schema/browsertrix_pages.yaml
    ${WEBSHOT_REPO_ROOT}/schema/wacz.yaml
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
target_link_libraries(${PROJECT_NAME}_objs PRIVATE ${PROJECT_NAME}-chgen)
target_link_libraries(
  ${PROJECT_NAME}
  PRIVATE ${PROJECT_NAME}-chgen
          arkhiv::arkhiv
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

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION "webshotd")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/config/"
        DESTINATION "webshotd/config")
install(DIRECTORY "${WEBSHOT_REPO_ROOT}/schema/"
        DESTINATION "schema")
