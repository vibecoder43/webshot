find_package(LibArchive REQUIRED)

add_library(arkhiv src/gzip.cpp src/zip_archive.cpp include/arkhiv/gzip.hpp
                   include/arkhiv/zip_archive.hpp)
add_library(arkhiv::arkhiv ALIAS arkhiv)

target_include_directories(arkhiv PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(arkhiv PUBLIC LibArchive::LibArchive)

set_property(TARGET arkhiv PROPERTY CXX_STANDARD 26)
set_property(TARGET arkhiv PROPERTY CXX_STANDARD_REQUIRED ON)
set_property(TARGET arkhiv PROPERTY CXX_EXTENSIONS OFF)
apply_defaults(arkhiv)

if(BUILD_TESTING)
  find_package(GTest CONFIG REQUIRED)
  include(GoogleTest)

  add_executable(arkhiv_zip_archive_test test/zip_archive.cpp)
  target_include_directories(arkhiv_zip_archive_test
                             PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(arkhiv_zip_archive_test
                        PRIVATE arkhiv LibArchive::LibArchive GTest::gtest_main)
  set_property(TARGET arkhiv_zip_archive_test PROPERTY CXX_STANDARD 26)
  set_property(TARGET arkhiv_zip_archive_test PROPERTY CXX_STANDARD_REQUIRED ON)
  set_property(TARGET arkhiv_zip_archive_test PROPERTY CXX_EXTENSIONS OFF)
  apply_defaults(arkhiv_zip_archive_test)
  set_property(
    TARGET arkhiv_zip_archive_test
    PROPERTY
      BUILD_RPATH
      "$<TARGET_FILE_DIR:LibArchive::LibArchive>;$<TARGET_FILE_DIR:GTest::gtest_main>;$<TARGET_FILE_DIR:GTest::gtest>;${CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES}"
  )
  gtest_discover_tests(arkhiv_zip_archive_test)
endif()
