cmake_minimum_required(VERSION 3.25)
project(webshot CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CXX_FLAGS "-Wall -Wextra -Wpedantic \
-Wconversion -Wsign-conversion -Wfloat-equal \
-Wdisabled-optimization -Wctad-maybe-unsupported")
set(CMAKE_CXX_FLAGS_DEBUG "-ggdb3")
set(CMAKE_CXX_FLAGS_RELEASE "-O3")

set(CMAKE_CXX_COMPILER_LAUNCHER ccache)

set(${PROJECT_NAME}_headers
    src/include/webshot_handler.hpp
    src/include/webshot_by_id_handler.hpp
    src/include/webshot_crud.hpp
    src/include/common_definitions.hpp
    src/include/url_validation.hpp
)
add_library(${PROJECT_NAME}_objs OBJECT
    src/webshot_handler.cpp
    src/webshot_by_id_handler.cpp
    src/webshot_crud.cpp
    src/url_validation.cpp
	${${PROJECT_NAME}_headers}
)
target_precompile_headers(${PROJECT_NAME}_objs PRIVATE ${${PROJECT_NAME}_headers})

add_executable(${PROJECT_NAME} src/main.cpp
)

find_package(userver CONFIG REQUIRED COMPONENTS core postgresql chaotic)
find_package(fmt CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME}_objs PRIVATE fmt::fmt)
find_package(ada CONFIG REQUIRED COMPONENTS ada)
target_link_libraries(${PROJECT_NAME}_objs PRIVATE ada::ada)
target_link_libraries(${PROJECT_NAME}_objs PUBLIC userver::core userver::postgresql)
target_link_libraries(${PROJECT_NAME} PRIVATE ${PROJECT_NAME}_objs)

include(CTest)
if (BUILD_TESTING)
  add_executable(is_valid_webshot_url_test tests/is_valid_webshot_url_test.cpp)
  target_link_libraries(is_valid_webshot_url_test PRIVATE userver::utest ada::ada ${PROJECT_NAME}_objs)
  target_include_directories(is_valid_webshot_url_test PRIVATE src/include)
  add_test(NAME is_valid_webshot_url COMMAND is_valid_webshot_url_test)
  set_tests_properties(is_valid_webshot_url PROPERTIES ENVIRONMENT "LSAN_OPTIONS=detect_leaks=0")
endif()

if (USE_SANITIZERS)
  target_compile_options(${PROJECT_NAME} PRIVATE "-fsanitize=address,undefined")
  target_link_options(${PROJECT_NAME} PRIVATE "-fsanitize=address,undefined")
	#	target_compile_options(${TARGET} PRIVATE "-fsanitize=undefined,thread")
	#	target_link_options(${TARGET} PRIVATE "-fsanitize=undefined,thread")
endif()

file(GLOB_RECURSE SCHEMAS ${CMAKE_CURRENT_SOURCE_DIR}/schemas/*.yaml)
userver_target_generate_chaotic(
    ${PROJECT_NAME}-chgen
    GENERATE_SERIALIZERS
    LAYOUT "/components/schemas/([^/]*)/=dto::{0}"
    OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/src
    SCHEMAS ${SCHEMAS}
    RELATIVE_TO ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(${PROJECT_NAME} PRIVATE ${PROJECT_NAME}-chgen)
target_include_directories(${PROJECT_NAME}_objs
                           PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/src/)
