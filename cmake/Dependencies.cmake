# 依赖查找：线程 / SQLite / DCMTK / GoogleTest
include(FetchContent)

find_package(Threads REQUIRED)

find_package(SQLite3 QUIET)
if(SQLite3_FOUND)
  message(STATUS "SQLite3: ${SQLite3_VERSION}")
else()
  message(WARNING "SQLite3 not found -> MetadataDao will not link. Install libsqlite3-dev.")
endif()

if(DWS_HAVE_DCMTK)
  find_package(DCMTK QUIET)
  if(DCMTK_FOUND)
    message(STATUS "DCMTK: ${DCMTK_VERSION_NUMBER}")
    include_directories(${DCMTK_INCLUDE_DIRS})
  else()
    message(STATUS "DCMTK not found (apt: libdcmtk-dev / brew: dcmtk)")
  endif()
endif()

if(DWS_BUILD_TESTS)
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    message(STATUS "GoogleTest not found -> fetching via FetchContent")
    FetchContent_Declare(googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG        v1.14.0)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()
endif()
