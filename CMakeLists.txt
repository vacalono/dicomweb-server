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
option(DWS_HAVE_MYSQL    "Compile MySQL 8 metadata backend" OFF)
option(DWS_HAVE_REDIS    "Compile Redis QIDO cache" OFF)
option(DWS_HAVE_KAFKA    "Compile Kafka async STOW pipeline" OFF)
option(DWS_HAVE_GRPC     "Build gRPC metadata service + bench (P5 comparison)" OFF)

add_compile_options(-Wall -Wextra -Wpedantic)
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  add_compile_options(-O2 -g)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Dependencies)

# ---------------------------------------------------------------- core library
add_library(dws_core STATIC
  src/util/ThreadPool.cpp
  src/util/HealthGuard.cpp
  src/mq/KafkaProducer.cpp
  src/mq/StowConsumer.cpp
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
  src/store/MysqlMetadataDao.cpp
  src/store/QidoCache.cpp
  src/store/DaoFactory.cpp
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

if(DWS_HAVE_MYSQL)
  find_path(MYSQL_INCLUDE_DIR NAMES mysql.h PATH_SUFFIXES mysql mariadb)
  find_library(MYSQL_LIBRARY NAMES mariadb mysqlclient mysql)
  if(MYSQL_INCLUDE_DIR AND MYSQL_LIBRARY)
    target_compile_definitions(dws_core PUBLIC DWS_HAVE_MYSQL=1)
    target_include_directories(dws_core PUBLIC ${MYSQL_INCLUDE_DIR})
    target_link_libraries(dws_core PUBLIC ${MYSQL_LIBRARY})
    message(STATUS "MySQL backend enabled: ${MYSQL_LIBRARY}")
  else()
    message(FATAL_ERROR "DWS_HAVE_MYSQL=ON but mysql client library not found")
  endif()
endif()

if(DWS_HAVE_REDIS)
  find_path(HIREDIS_INCLUDE_DIR NAMES hiredis/hiredis.h hiredis.h)
  find_library(HIREDIS_LIBRARY NAMES hiredis)
  if(HIREDIS_INCLUDE_DIR AND HIREDIS_LIBRARY)
    target_compile_definitions(dws_core PUBLIC DWS_HAVE_REDIS=1)
    target_include_directories(dws_core PUBLIC ${HIREDIS_INCLUDE_DIR})
    target_link_libraries(dws_core PUBLIC ${HIREDIS_LIBRARY})
    message(STATUS "Redis cache enabled: ${HIREDIS_LIBRARY}")
  else()
    message(FATAL_ERROR "DWS_HAVE_REDIS=ON but hiredis not found")
  endif()
endif()

if(DWS_HAVE_KAFKA)
  find_path(RDKAFKA_INCLUDE_DIR NAMES librdkafka/rdkafka.h rdkafka.h)
  find_library(RDKAFKA_LIBRARY NAMES rdkafka)
  if(RDKAFKA_INCLUDE_DIR AND RDKAFKA_LIBRARY)
    target_compile_definitions(dws_core PUBLIC DWS_HAVE_KAFKA=1)
    target_include_directories(dws_core PUBLIC ${RDKAFKA_INCLUDE_DIR})
    target_link_libraries(dws_core PUBLIC ${RDKAFKA_LIBRARY})
    message(STATUS "Kafka pipeline enabled: ${RDKAFKA_LIBRARY}")
  else()
    message(FATAL_ERROR "DWS_HAVE_KAFKA=ON but librdkafka not found")
  endif()
endif()

if(DWS_USE_POSTGRES)
  target_compile_definitions(dws_core PUBLIC DWS_USE_POSTGRES=1)
endif()

# ---------------------------------------------------------------- gRPC (P5 对照)
if(DWS_HAVE_GRPC)
  find_package(Protobuf REQUIRED)
  find_package(gRPC CONFIG REQUIRED)

  set(PROTO_FILE "${CMAKE_CURRENT_SOURCE_DIR}/proto/metadata.proto")
  set(PROTO_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen")
  file(MAKE_DIRECTORY "${PROTO_GEN_DIR}")

  add_custom_command(
    OUTPUT "${PROTO_GEN_DIR}/metadata.pb.cc" "${PROTO_GEN_DIR}/metadata.pb.h"
           "${PROTO_GEN_DIR}/metadata.grpc.pb.cc" "${PROTO_GEN_DIR}/metadata.grpc.pb.h"
    COMMAND protobuf::protoc
            --proto_path="${CMAKE_CURRENT_SOURCE_DIR}/proto"
            --cpp_out="${PROTO_GEN_DIR}"
            --grpc_out="${PROTO_GEN_DIR}"
            --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
            "${PROTO_FILE}"
    DEPENDS "${PROTO_FILE}"
    COMMENT "Generating gRPC stubs (metadata.proto)")

  add_library(dws_proto STATIC
    "${PROTO_GEN_DIR}/metadata.pb.cc"
    "${PROTO_GEN_DIR}/metadata.grpc.pb.cc")
  target_include_directories(dws_proto PUBLIC "${PROTO_GEN_DIR}")
  target_link_libraries(dws_proto PUBLIC gRPC::grpc++ protobuf::libprotobuf)

  add_executable(metadata-grpc-server src/rpc/metadata_grpc_server.cpp)
  target_link_libraries(metadata-grpc-server PRIVATE dws_core dws_proto)

  add_executable(grpc-bench tools/grpc_bench.cpp)
  target_link_libraries(grpc-bench PRIVATE dws_proto)

  message(STATUS "gRPC metadata service enabled (protoc: $<TARGET_FILE:protobuf::protoc>)")
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
