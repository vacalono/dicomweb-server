cmake_minimum_required(VERSION 3.16)
project(dicomweb-server
        VERSION 0.1.0
        DESCRIPTION "Lightweight DICOMweb (STOW/QIDO/WADO) server in C++17"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(DWS_BUILD_TESTS   "Build unit tests"          ON)
option(DWS_HAVE_DCMTK    "Compile DCMTK-dependent code" ON)
option(DWS_USE_POSTGRES  "Use PostgreSQL instead of SQLite" OFF)

add_compile_options(-Wall -Wextra -Wpedantic)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  add_compile_options(-O2 -g)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Dependencies)

# ---------------------------------------------------------------- core library
add_library(dws_core STATIC
  src/util/ThreadPool.cpp
  src/util/MpscThreadPool.cpp
  src/net/Buffer.cpp
  src/net/EpollPoller.cpp
  src/net/EventLoop.cpp
  src/net/EventLoopThread.cpp
  src/net/EventLoopThreadPool.cpp
  src/net/TcpServer.cpp
  src/http/HttpParser.cpp
  src/api/Router.cpp
  src/store/MetadataDao.cpp
  src/store/ObjectStore.cpp
)

target_include_directories(dws_core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(dws_core PUBLIC Threads::Threads)

# DicomFile.cpp 始终参与编译：未找到 DCMTK 时退化为 stub（DICOM magic 校验 + 占位 UID）
target_sources(dws_core PRIVATE src/dicom/DicomFile.cpp)

if(DWS_HAVE_DCMTK AND DCMTK_FOUND)
  target_compile_definitions(dws_core PUBLIC DWS_HAVE_DCMTK=1)
  target_link_libraries(dws_core PUBLIC ${DCMTK_LIBRARIES})
  target_include_directories(dws_core PUBLIC ${DCMTK_INCLUDE_DIRS})
else()
  message(STATUS "DCMTK not found or disabled -> DICOM codec compiled as stub")
endif()

if(SQLite3_FOUND)
  target_compile_definitions(dws_core PUBLIC DWS_HAVE_SQLITE3=1)
  target_link_libraries(dws_core PUBLIC SQLite::SQLite3)
endif()

if(DWS_USE_POSTGRES)
  target_compile_definitions(dws_core PUBLIC DWS_USE_POSTGRES=1)
endif()

# ---------------------------------------------------------------- executable
add_executable(dicomweb-server src/main.cpp)
target_link_libraries(dicomweb-server PRIVATE dws_core)

# ---------------------------------------------------------------- tests
if(DWS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

message(STATUS "dicomweb-server ${PROJECT_VERSION}  |  C++${CMAKE_CXX_STANDARD}  |  ${CMAKE_BUILD_TYPE}")
