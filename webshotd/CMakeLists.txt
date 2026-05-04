cmake_minimum_required(VERSION 3.29)
project(webshotd CXX)

include(GNUInstallDirs)

get_filename_component(WEBSHOT_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.."
                       ABSOLUTE)

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  message(
    FATAL_ERROR
      "CMAKE_INSTALL_PREFIX must be set explicitly by the devenv configure flags."
  )
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

function(webshot_write_file_if_different path content)
  set(tmp_path "${path}.tmp")
  file(WRITE "${tmp_path}" "${content}")
  file(COPY_FILE "${tmp_path}" "${path}" ONLY_IF_DIFFERENT)
  file(REMOVE "${tmp_path}")
endfunction()

set(WEBSHOT_COVERAGE_FLAGS "-fprofile-instr-generate" "-fcoverage-mapping")
set(WEBSHOT_BROWSER_SANDBOX_CLOSURE_PATHS_FILE
    ""
    CACHE
      STRING
      "Absolute path to newline-delimited browser sandbox runtime closure paths"
)
set(WEBSHOT_BROWSER_SANDBOX_FONTCONFIG_FILE
    ""
    CACHE STRING "Absolute path to the browser sandbox fontconfig file")
set(WEBSHOT_BROWSER_SANDBOX_PATH
    ""
    CACHE STRING "PATH visible inside the browser bubblewrap sandbox")

if(NOT CMAKE_LINKER_TYPE)
  message(FATAL_ERROR "CMAKE_LINKER_TYPE must be configured")
endif()

# Ensure the installed `webshotd` binary is runnable without extra wrappers like
# `LD_LIBRARY_PATH` by embedding runtime search paths for linked dependencies.
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)

set(${PROJECT_NAME}_headers
    include/integers.hpp
    include/s3_credentials_types.hpp
    include/s3/s3_sts_client.hpp
    include/s3/s3_v4_client.hpp
    src/text_postgres_formatter.hpp
    src/handler.hpp
    src/job_handler.hpp
    src/by_prefix_handler.hpp
    src/by_id_handler.hpp
    src/browser_probe_handler.hpp
    src/allowlist_handler.hpp
    src/disallow_and_purge_handler.hpp
    src/denylist_check_handler.hpp
    src/docs_handler.hpp
    src/ui_replay_handler.hpp
    src/test_only_components.hpp
    include/crud.hpp
    include/storage_url.hpp
    src/crawler/runner.hpp
    include/crawler/artifacts.hpp
    src/crawler/browser_session.hpp
    include/crawler/browser_page_lifecycle.hpp
    include/crawler/limits.hpp
    src/crawler/launch_policy.hpp
    src/crawler/cdp_client.hpp
    src/crawler/egress_proxy.hpp
    include/config.hpp
    src/crawler/failure.hpp
    include/crawler/fallback.hpp
    include/client_ip.hpp
    include/error_utils.hpp
    include/http_utils.hpp
    include/ip.hpp
    src/denylist.hpp
    include/link.hpp
    include/uuid_utils.hpp
    include/url.hpp
    include/cursor.hpp
    include/pagination.hpp
    include/prefix_pagination.hpp
    include/prefix_utils.hpp
    src/metrics.hpp
    include/text.hpp)
add_library(
  ${PROJECT_NAME}_objs OBJECT
  src/handler.cpp
  src/job_handler.cpp
  src/by_prefix_handler.cpp
  src/by_id_handler.cpp
  src/allowlist_handler.cpp
  src/disallow_and_purge_handler.cpp
  src/denylist_check_handler.cpp
  src/docs_handler.cpp
  src/ui_replay_handler.cpp
  src/crud.cpp
  src/metrics.cpp
  src/url.cpp
  src/crawler/runner.cpp
  src/crawler/artifacts.cpp
  src/crawler/browser_session.cpp
  src/crawler/browser_page_lifecycle.cpp
  src/crawler/cdp_client.cpp
  src/crawler/egress_proxy.cpp
  src/crawler/failure.cpp
  src/text_postgres_formatter.cpp
  src/error_utils.cpp
  src/denylist.cpp
  src/link.cpp
  src/config.cpp
  src/storage_url.cpp
  src/cursor.cpp
  src/pagination.cpp
  src/prefix_pagination.cpp
  src/prefix_utils.cpp
  ${${PROJECT_NAME}_headers})

if(USERVER_FEATURE_TESTSUITE)
  target_sources(
    ${PROJECT_NAME}_objs PRIVATE src/browser_probe_handler.cpp
                                 src/test_only_components_enabled.cpp)
else()
  target_sources(${PROJECT_NAME}_objs
                 PRIVATE src/test_only_components_disabled.cpp)
endif()

function(apply_defaults target)
  set_property(TARGET ${target} PROPERTY CXX_STANDARD 26)
  set_property(TARGET ${target} PROPERTY CXX_STANDARD_REQUIRED ON)
  set_property(TARGET ${target} PROPERTY CXX_EXTENSIONS OFF)
  set(lifetime_safety_warnings
      -Wexperimental-lifetime-safety-suggestions
      -Wexperimental-lifetime-safety-intra-tu-suggestions
      -Wexperimental-lifetime-safety-cross-tu-suggestions
      -Wexperimental-lifetime-safety-permissive)
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
            -Wctad-maybe-unsupported
            -Wcast-align
            -Wcast-qual
            -Wcomma
            -Wdouble-promotion
            -Wextra-semi
            -Wimplicit-fallthrough
            -Wmissing-noreturn
            -Wnon-virtual-dtor
            -Wnull-dereference
            -Wold-style-cast
            -Woverloaded-virtual
            -Wshadow
            -Wswitch-enum
            -Wtautological-compare
            -Wunreachable-code-aggressive
            -Wzero-as-null-pointer-constant
            ${lifetime_wafety_warnings})
  target_compile_options(
    ${target}
    PRIVATE -Wformat
            -Wformat-security
            -Werror=format-security
            -fno-omit-frame-pointer
            -mno-omit-leaf-frame-pointer
            -fstack-clash-protection
            -fstack-protector-strong
            --param=ssp-buffer-size=4
            -fstrict-flex-arrays=1
            -fwrapv
            -fzero-call-used-regs=used-gpr
            $<$<CONFIG:RelWithDebInfo>:-U_FORTIFY_SOURCE>
            $<$<CONFIG:RelWithDebInfo>:-D_FORTIFY_SOURCE=2>)
  target_compile_options(
    ${target}
    PRIVATE $<$<CONFIG:Debug>:-ggdb3>
            $<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:-fdebug-macro>
            $<$<CONFIG:RelWithDebInfo>:-O2>)
  if(USE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(${target} PRIVATE -O1)
  endif()
endfunction()

function(apply_link_defaults target)
  target_link_options(${target} PRIVATE -pie LINKER:-z,relro LINKER:-z,now)
endfunction()

function(apply_sanitizer_compile_defaults target)
  if(USE_SANITIZERS)
    target_compile_options(${target} PRIVATE "-fsanitize=address,undefined")
  endif()
endfunction()

function(apply_sanitizer_link_defaults target)
  if(USE_SANITIZERS)
    target_link_options(${target} PRIVATE "-fsanitize=address,undefined")
  endif()
endfunction()

add_executable(${PROJECT_NAME} src/main.cpp
                               $<TARGET_OBJECTS:${PROJECT_NAME}_objs>)
apply_defaults(${PROJECT_NAME}_objs)
apply_defaults(${PROJECT_NAME})
apply_link_defaults(${PROJECT_NAME})
apply_sanitizer_compile_defaults(${PROJECT_NAME}_objs)
apply_sanitizer_compile_defaults(${PROJECT_NAME})
apply_sanitizer_link_defaults(${PROJECT_NAME})
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME webshotd)
set(WEBSHOT_RUNTIME_WRAPPER_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/webshotd_wrapper.sh")
set(WEBSHOT_RUNTIME_WRAPPER "${CMAKE_CURRENT_BINARY_DIR}/webshotd_wrapper.sh")
set(WEBSHOT_BWRAP_STATUS_WRAPPER_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/bwrap_status_wrapper.sh")
set(WEBSHOT_BWRAP_STATUS_WRAPPER
    "${CMAKE_CURRENT_BINARY_DIR}/bwrap_status_wrapper.sh")
set_property(
  DIRECTORY
  APPEND
  PROPERTY CMAKE_CONFIGURE_DEPENDS "${WEBSHOT_RUNTIME_WRAPPER_SOURCE}"
           "${WEBSHOT_BWRAP_STATUS_WRAPPER_SOURCE}")
file(COPY_FILE "${WEBSHOT_RUNTIME_WRAPPER_SOURCE}" "${WEBSHOT_RUNTIME_WRAPPER}"
     ONLY_IF_DIFFERENT)
file(COPY_FILE "${WEBSHOT_BWRAP_STATUS_WRAPPER_SOURCE}"
     "${WEBSHOT_BWRAP_STATUS_WRAPPER}" ONLY_IF_DIFFERENT)
file(
  CHMOD
  "${WEBSHOT_RUNTIME_WRAPPER}"
  PERMISSIONS
  OWNER_READ
  OWNER_WRITE
  OWNER_EXECUTE
  GROUP_READ
  GROUP_EXECUTE
  WORLD_READ
  WORLD_EXECUTE)
file(
  CHMOD
  "${WEBSHOT_BWRAP_STATUS_WRAPPER}"
  PERMISSIONS
  OWNER_READ
  OWNER_WRITE
  OWNER_EXECUTE
  GROUP_READ
  GROUP_EXECUTE
  WORLD_READ
  WORLD_EXECUTE)
add_custom_target(
  ${PROJECT_NAME}_stage_runtime_layout ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_INSTALL_PREFIX}/webshotd"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_INSTALL_PREFIX}/web_ui"
  COMMAND
    ${CMAKE_COMMAND} -E copy_if_different
    "${WEBSHOT_REPO_ROOT}/web_ui/index.html"
    "${CMAKE_INSTALL_PREFIX}/web_ui/index.html"
  COMMAND
    ${CMAKE_COMMAND} -E copy_if_different
    "${WEBSHOT_REPO_ROOT}/web_ui/style.css"
    "${CMAKE_INSTALL_PREFIX}/web_ui/style.css"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${PROJECT_NAME}>
          "${CMAKE_INSTALL_PREFIX}/webshotd/webshotd"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${WEBSHOT_RUNTIME_WRAPPER}"
          "${CMAKE_INSTALL_PREFIX}/webshotd/webshotd_wrapper"
  COMMAND
    ${CMAKE_COMMAND} -E copy_if_different "${WEBSHOT_BWRAP_STATUS_WRAPPER}"
    "${CMAKE_INSTALL_PREFIX}/webshotd/bwrap_status_wrapper"
  DEPENDS ${PROJECT_NAME} "${WEBSHOT_REPO_ROOT}/web_ui/index.html"
          "${WEBSHOT_REPO_ROOT}/web_ui/style.css"
  VERBATIM)

if(WEBSHOT_BROWSER_SANDBOX_CLOSURE_PATHS_FILE STREQUAL "")
  message(
    FATAL_ERROR "WEBSHOT_BROWSER_SANDBOX_CLOSURE_PATHS_FILE must be configured")
endif()
if(WEBSHOT_BROWSER_SANDBOX_FONTCONFIG_FILE STREQUAL "")
  message(
    FATAL_ERROR "WEBSHOT_BROWSER_SANDBOX_FONTCONFIG_FILE must be configured")
endif()
if(WEBSHOT_BROWSER_SANDBOX_PATH STREQUAL "")
  message(FATAL_ERROR "WEBSHOT_BROWSER_SANDBOX_PATH must be configured")
endif()

target_precompile_headers(${PROJECT_NAME}_objs PRIVATE include/webshotd_pch.hpp)
target_precompile_headers(${PROJECT_NAME} PRIVATE include/webshotd_pch.hpp)

if(WEBSHOT_ENABLE_COVERAGE)
  target_compile_options(${PROJECT_NAME}_objs PRIVATE ${WEBSHOT_COVERAGE_FLAGS})
  target_link_options(${PROJECT_NAME} PRIVATE ${WEBSHOT_COVERAGE_FLAGS})
endif()

find_package(Boost CONFIG REQUIRED COMPONENTS context)
find_package(PkgConfig REQUIRED)
find_package(sml CONFIG REQUIRED)
find_package(userver CONFIG REQUIRED COMPONENTS core postgresql chaotic s3api)
pkg_check_modules(LibSeccomp REQUIRED IMPORTED_TARGET libseccomp)
include("${userver_DIR}/UserverEmbedFile.cmake")
userver_embed_file(
  webshotd_browser_sandbox_sh
  NAME
  "webshot_browser_sandbox_sh"
  FILEPATH
  "${CMAKE_CURRENT_SOURCE_DIR}/browser_sandbox.sh"
  HPP_FILENAME
  "browser_sandbox.sh")
userver_embed_file(
  webshotd_browser_sandbox_closure_paths
  NAME
  "webshot_browser_sandbox_closure_paths"
  FILEPATH
  "${WEBSHOT_BROWSER_SANDBOX_CLOSURE_PATHS_FILE}"
  HPP_FILENAME
  "browser_sandbox_closure_paths")
set(_webshot_browser_sandbox_path_file
    "${CMAKE_CURRENT_BINARY_DIR}/browser_sandbox_path.txt")
webshot_write_file_if_different("${_webshot_browser_sandbox_path_file}"
                                "${WEBSHOT_BROWSER_SANDBOX_PATH}")
userver_embed_file(
  webshotd_browser_sandbox_path
  NAME
  "webshot_browser_sandbox_path"
  FILEPATH
  "${_webshot_browser_sandbox_path_file}"
  HPP_FILENAME
  "browser_sandbox_path")
set(_webshot_browser_sandbox_embed_hpp
    "${CMAKE_CURRENT_BINARY_DIR}/embedded/include/generated/browser_sandbox.sh.hpp"
)
set(_webshot_browser_sandbox_closure_paths_embed_hpp
    "${CMAKE_CURRENT_BINARY_DIR}/embedded/include/generated/browser_sandbox_closure_paths.hpp"
)
set(_webshot_browser_sandbox_path_embed_hpp
    "${CMAKE_CURRENT_BINARY_DIR}/embedded/include/generated/browser_sandbox_path.hpp"
)
set_source_files_properties(
  src/crawler/runner.cpp src/crawler/browser_session.cpp
  src/browser_probe_handler.cpp
  PROPERTIES
    OBJECT_DEPENDS
    "${_webshot_browser_sandbox_embed_hpp};${_webshot_browser_sandbox_closure_paths_embed_hpp};${_webshot_browser_sandbox_path_embed_hpp}"
)
target_link_libraries(${PROJECT_NAME}_objs PRIVATE webshotd_browser_sandbox_sh)
target_link_libraries(${PROJECT_NAME} PRIVATE webshotd_browser_sandbox_sh)
target_link_libraries(${PROJECT_NAME}_objs
                      PRIVATE webshotd_browser_sandbox_closure_paths)
target_link_libraries(${PROJECT_NAME}
                      PRIVATE webshotd_browser_sandbox_closure_paths)
target_link_libraries(${PROJECT_NAME}_objs
                      PRIVATE webshotd_browser_sandbox_path)
target_link_libraries(${PROJECT_NAME} PRIVATE webshotd_browser_sandbox_path)
target_link_libraries(${PROJECT_NAME}_objs PRIVATE PkgConfig::LibSeccomp)
target_link_libraries(${PROJECT_NAME} PRIVATE PkgConfig::LibSeccomp)
target_link_libraries(${PROJECT_NAME}_objs PRIVATE sml::sml)
target_compile_definitions(${PROJECT_NAME}_objs
                           PRIVATE BOOST_SML_CFG_DISABLE_MIN_SIZE)
find_package(ada CONFIG REQUIRED COMPONENTS ada)
target_link_libraries(${PROJECT_NAME}_objs PRIVATE ada::ada)
find_package(uni-algo CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME}_objs PRIVATE uni-algo::uni-algo)
find_package(absl CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME}_objs PRIVATE absl::strings)
target_link_libraries(${PROJECT_NAME}_objs
                      PUBLIC userver::core userver::postgresql userver::s3api)
set_property(
  SOURCE src/crawler/browser_session.cpp
  APPEND
  PROPERTY
    COMPILE_DEFINITIONS
    WEBSHOT_BWRAP_STATUS_WRAPPER_PATH="${CMAKE_INSTALL_PREFIX}/webshotd/bwrap_status_wrapper"
    WEBSHOT_BROWSER_SANDBOX_FONTCONFIG_FILE="${WEBSHOT_BROWSER_SANDBOX_FONTCONFIG_FILE}"
)

if(CMAKE_CXX_INCLUDE_WHAT_YOU_USE)
  set(_webshot_iwyu_cmd ${CMAKE_CXX_INCLUDE_WHAT_YOU_USE})
  list(GET _webshot_iwyu_cmd 0 _webshot_iwyu_tool)
  if(IS_ABSOLUTE "${_webshot_iwyu_tool}")
    set(_webshot_iwyu_path "${_webshot_iwyu_tool}")
  else()
    find_program(_webshot_iwyu_path NAMES "${_webshot_iwyu_tool}")
  endif()
  if(NOT EXISTS "${_webshot_iwyu_path}")
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

add_subdirectory("${WEBSHOT_REPO_ROOT}/arkhiv"
                 "${CMAKE_CURRENT_BINARY_DIR}/arkhiv")
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
set_source_files_properties(
  "${CMAKE_CURRENT_BINARY_DIR}/src/webshot/sql_queries.cpp"
  PROPERTIES SKIP_LINTING ON)
target_link_libraries(${PROJECT_NAME}_objs PRIVATE ${PROJECT_NAME}_sql)
target_link_libraries(${PROJECT_NAME} PRIVATE ${PROJECT_NAME}_sql)
apply_sanitizer_compile_defaults(${PROJECT_NAME}_sql)

include(CTest)
if(BUILD_TESTING)
  add_subdirectory(test)
endif()

set(SCHEMAS
    ${WEBSHOT_REPO_ROOT}/schema/browser_probe.yaml
    ${WEBSHOT_REPO_ROOT}/schema/cdp.yaml
    ${WEBSHOT_REPO_ROOT}/schema/browsertrix_pages.yaml
    ${WEBSHOT_REPO_ROOT}/schema/wacz.yaml
    ${WEBSHOT_REPO_ROOT}/schema/common/common.yaml
    ${WEBSHOT_REPO_ROOT}/schema/public/webshot.yaml)
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
apply_sanitizer_compile_defaults(${PROJECT_NAME}-chgen)
target_link_libraries(${PROJECT_NAME}_objs PRIVATE ${PROJECT_NAME}-chgen)
target_link_libraries(
  ${PROJECT_NAME}
  PRIVATE ${PROJECT_NAME}-chgen
          arkhiv::arkhiv
          ada::ada
          userver::core
          userver::postgresql
          userver::s3api
          absl::strings
          uni-algo::uni-algo)
target_include_directories(
  ${PROJECT_NAME}_objs
  PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/src/ ${CMAKE_CURRENT_SOURCE_DIR}/include
          ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_include_directories(
  ${PROJECT_NAME}
  PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/src/ ${CMAKE_CURRENT_SOURCE_DIR}/include
          ${CMAKE_CURRENT_SOURCE_DIR}/src)

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION "webshotd")
install(
  PROGRAMS "${WEBSHOT_RUNTIME_WRAPPER_SOURCE}"
  DESTINATION "webshotd"
  RENAME "webshotd_wrapper")
install(
  PROGRAMS "${WEBSHOT_BWRAP_STATUS_WRAPPER_SOURCE}"
  DESTINATION "webshotd"
  RENAME "bwrap_status_wrapper")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/config/"
        DESTINATION "webshotd/config")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/sql/schema/"
        DESTINATION "webshotd/sql/schema")
install(DIRECTORY "${WEBSHOT_REPO_ROOT}/schema/" DESTINATION "schema")
install(FILES "${WEBSHOT_REPO_ROOT}/seaweedfs/s3_config.json"
        DESTINATION "seaweedfs")
install(FILES "${WEBSHOT_REPO_ROOT}/web_ui/index.html"
              "${WEBSHOT_REPO_ROOT}/web_ui/style.css" DESTINATION "web_ui")
