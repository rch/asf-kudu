cmake_minimum_required(VERSION 3.14)
project(kudu_caf_phases)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use CAF from Kudu thirdparty
set(CAF_ROOT "${CMAKE_SOURCE_DIR}/../../thirdparty/installed/uninstrumented")
set(CMAKE_PREFIX_PATH "${CAF_ROOT};${CAF_ROOT}/lib/cmake" ${CMAKE_PREFIX_PATH})

# Find required packages
find_package(CAF REQUIRED COMPONENTS core io)
find_package(OpenSSL REQUIRED)

# Try to find Aeron (from nixpkgs or system)
# Aeron doesn't always provide CMake config, so we'll find it manually
find_path(AERON_INCLUDE_DIR Aeron.h)
find_library(AERON_CLIENT_LIB aeron_client_shared)

if(AERON_INCLUDE_DIR AND AERON_CLIENT_LIB)
  message(STATUS "Found Aeron client: ${AERON_CLIENT_LIB}")
  message(STATUS "  Include dir: ${AERON_INCLUDE_DIR}")
  set(HAVE_AERON TRUE)
else()
  message(STATUS "Aeron not found - skipping Aeron examples")
  set(HAVE_AERON FALSE)
endif()

# Set Kudu paths
if(NOT DEFINED KUDU_BUILD_DIR)
  set(KUDU_BUILD_DIR "${CMAKE_SOURCE_DIR}/../../build/latest")
endif()

if(NOT DEFINED KUDU_SOURCE_DIR)
  set(KUDU_SOURCE_DIR "${CMAKE_SOURCE_DIR}/../..")
endif()

set(KUDU_CLIENT_INCLUDE_DIR "${KUDU_SOURCE_DIR}/src")
set(KUDU_CLIENT_LIB_DIR "${KUDU_BUILD_DIR}/lib")
set(KUDU_THIRDPARTY_DIR "${KUDU_SOURCE_DIR}/thirdparty/installed/uninstrumented")

# Detect build type by checking for shared vs static libraries
# Check directly for file existence to avoid find_library matching issues
if(EXISTS "${KUDU_CLIENT_LIB_DIR}/libkudu_client.so")
  set(KUDU_CLIENT_LIB "${KUDU_CLIENT_LIB_DIR}/libkudu_client.so")
  set(KUDU_LINK_MODE "DYNAMIC")
elseif(EXISTS "${KUDU_CLIENT_LIB_DIR}/libkudu_client.a")
  set(KUDU_CLIENT_LIB "${KUDU_CLIENT_LIB_DIR}/libkudu_client.a")
  set(KUDU_LINK_MODE "STATIC")
else()
  message(FATAL_ERROR "Could not find Kudu client library (neither shared nor static) in ${KUDU_CLIENT_LIB_DIR}")
endif()

# Find remaining libraries based on detected mode
if(KUDU_LINK_MODE STREQUAL "DYNAMIC")
  set(KUDU_COMMON_LIB "${KUDU_CLIENT_LIB_DIR}/libkudu_common.so")
  set(KUDU_UTIL_LIB "${KUDU_CLIENT_LIB_DIR}/libkudu_util.so")
  if(NOT EXISTS "${KUDU_COMMON_LIB}" OR NOT EXISTS "${KUDU_UTIL_LIB}")
    message(FATAL_ERROR "Could not find required Kudu shared libraries")
  endif()
  message(STATUS "Kudu linking mode: DYNAMIC (debug build)")
  message(STATUS "  kudu_client: ${KUDU_CLIENT_LIB}")
  message(STATUS "  kudu_common: ${KUDU_COMMON_LIB}")
  message(STATUS "  kudu_util: ${KUDU_UTIL_LIB}")
elseif(KUDU_LINK_MODE STREQUAL "STATIC")
  set(KUDU_COMMON_LIB "${KUDU_CLIENT_LIB_DIR}/libkudu_common.a")
  set(KUDU_UTIL_LIB "${KUDU_CLIENT_LIB_DIR}/libkudu_util.a")
  if(NOT EXISTS "${KUDU_COMMON_LIB}" OR NOT EXISTS "${KUDU_UTIL_LIB}")
    message(FATAL_ERROR "Could not find required Kudu static libraries")
  endif()
  message(STATUS "Kudu linking mode: STATIC (release build)")
  message(STATUS "  kudu_client: ${KUDU_CLIENT_LIB}")
  message(STATUS "  kudu_common: ${KUDU_COMMON_LIB}")
  message(STATUS "  kudu_util: ${KUDU_UTIL_LIB}")

  # For static linking, we need all transitive dependencies
  # First, find additional Kudu internal libraries
  set(KUDU_GUTIL_LIB "${KUDU_CLIENT_LIB_DIR}/libgutil.a")
  set(KUDU_KRPC_LIB "${KUDU_CLIENT_LIB_DIR}/libkrpc.a")
  set(KUDU_SECURITY_LIB "${KUDU_CLIENT_LIB_DIR}/libsecurity.a")

  # Find Kudu proto libraries
  file(GLOB KUDU_PROTO_LIBS "${KUDU_CLIENT_LIB_DIR}/lib*_proto.a")

  # Find thirdparty dependencies
  find_library(GLOG_LIB NAMES libglog.a
               PATHS ${KUDU_THIRDPARTY_DIR}/lib NO_DEFAULT_PATH REQUIRED)
  find_library(GFLAGS_LIB NAMES libgflags.a
               PATHS ${KUDU_THIRDPARTY_DIR}/lib NO_DEFAULT_PATH REQUIRED)
  find_library(PROTOBUF_LIB NAMES libprotobuf.a
               PATHS ${KUDU_THIRDPARTY_DIR}/lib NO_DEFAULT_PATH REQUIRED)
  find_library(UNWIND_LIB NAMES libunwind.a
               PATHS ${KUDU_THIRDPARTY_DIR}/lib NO_DEFAULT_PATH REQUIRED)
  find_library(ZLIB_LIB NAMES libz.a
               PATHS ${KUDU_THIRDPARTY_DIR}/lib NO_DEFAULT_PATH REQUIRED)
  find_library(LZ4_LIB NAMES liblz4.a
               PATHS ${KUDU_THIRDPARTY_DIR}/lib NO_DEFAULT_PATH REQUIRED)
  find_library(SNAPPY_LIB NAMES libsnappy.a
               PATHS ${KUDU_THIRDPARTY_DIR}/lib NO_DEFAULT_PATH REQUIRED)
  find_library(CRCUTIL_LIB NAMES libcrcutil.a
               PATHS ${KUDU_THIRDPARTY_DIR}/lib NO_DEFAULT_PATH REQUIRED)
  find_library(YAMLCPP_LIB NAMES libyaml-cpp.a
               PATHS ${KUDU_THIRDPARTY_DIR}/lib NO_DEFAULT_PATH REQUIRED)
  find_library(EV_LIB NAMES libev.a
               PATHS ${KUDU_THIRDPARTY_DIR}/lib NO_DEFAULT_PATH REQUIRED)
  find_library(TCMALLOC_LIB NAMES libtcmalloc.a
               PATHS ${KUDU_THIRDPARTY_DIR}/lib NO_DEFAULT_PATH)  # Optional

  set(KUDU_STATIC_DEPS
    ${KUDU_GUTIL_LIB}
    ${KUDU_KRPC_LIB}
    ${KUDU_SECURITY_LIB}
    ${KUDU_PROTO_LIBS}
    ${GLOG_LIB}
    ${GFLAGS_LIB}
    ${PROTOBUF_LIB}
    ${CRCUTIL_LIB}
    ${YAMLCPP_LIB}
    ${EV_LIB}
    ${UNWIND_LIB}
    ${ZLIB_LIB}
    ${LZ4_LIB}
    ${SNAPPY_LIB}
  )
  if(TCMALLOC_LIB)
    list(APPEND KUDU_STATIC_DEPS ${TCMALLOC_LIB})
  endif()

  # Find breakpad library from thirdparty
  find_library(BREAKPAD_LIB NAMES libbreakpad_client.a
               PATHS ${KUDU_THIRDPARTY_DIR}/lib NO_DEFAULT_PATH)
  if(BREAKPAD_LIB)
    list(APPEND KUDU_STATIC_DEPS ${BREAKPAD_LIB})
  endif()

  message(STATUS "Static dependencies:")
  message(STATUS "  gutil: ${KUDU_GUTIL_LIB}")
  message(STATUS "  krpc: ${KUDU_KRPC_LIB}")
  message(STATUS "  security: ${KUDU_SECURITY_LIB}")
  message(STATUS "  proto libraries: ${KUDU_PROTO_LIBS}")
  message(STATUS "  glog: ${GLOG_LIB}")
  message(STATUS "  gflags: ${GFLAGS_LIB}")
  message(STATUS "  protobuf: ${PROTOBUF_LIB}")
  message(STATUS "  crcutil: ${CRCUTIL_LIB}")
  message(STATUS "  yaml-cpp: ${YAMLCPP_LIB}")
  message(STATUS "  ev: ${EV_LIB}")
  if(BREAKPAD_LIB)
    message(STATUS "  breakpad: ${BREAKPAD_LIB}")
  endif()
endif()

# Compiler flags
add_compile_options(-Wall -Wextra)

# Fix for CAF/Kudu static linking TLS conflicts
add_compile_definitions(_GLIBCXX_GTHREAD_USE_WEAK=0)

# Prepare Kudu link libraries based on mode
if(KUDU_LINK_MODE STREQUAL "STATIC")
  # For static linking, use linker groups to handle circular dependencies
  set(KUDU_LINK_LIBS
    -Wl,--start-group
    ${KUDU_CLIENT_LIB}
    ${KUDU_COMMON_LIB}
    ${KUDU_UTIL_LIB}
    ${KUDU_STATIC_DEPS}
    -Wl,--end-group
  )
  # System libraries required for static linking
  set(KUDU_SYSTEM_LIBS
    krb5
    krb5support
    k5crypto
    com_err
    gssapi_krb5
    sasl2
  )
else()
  set(KUDU_LINK_LIBS
    ${KUDU_CLIENT_LIB}
    ${KUDU_COMMON_LIB}
    ${KUDU_UTIL_LIB}
  )
  set(KUDU_SYSTEM_LIBS "")
endif()

# Phase 1: CAF Hello World (no Kudu dependency)
add_executable(phase1_hello_world phase1_hello_world.cc)
target_link_libraries(phase1_hello_world
  CAF::core
  CAF::io
)

# Phase 2: Kudu Only (no CAF dependency)
add_executable(phase2_kudu_only phase2_kudu_only.cc)
target_include_directories(phase2_kudu_only PRIVATE
  ${KUDU_CLIENT_INCLUDE_DIR}
  ${KUDU_BUILD_DIR}/src
)
target_link_libraries(phase2_kudu_only
  ${KUDU_LINK_LIBS}
  ${OPENSSL_LIBRARIES}
  ${KUDU_SYSTEM_LIBS}
  pthread
  dl
  rt
)

# Phase 3: CAF + Kudu Integration (CRITICAL TEST)
add_executable(phase3_actor_kudu phase3_actor_kudu.cc)
target_include_directories(phase3_actor_kudu PRIVATE
  ${KUDU_CLIENT_INCLUDE_DIR}
  ${KUDU_BUILD_DIR}/src
)
target_link_libraries(phase3_actor_kudu
  CAF::core
  CAF::io
  ${KUDU_LINK_LIBS}
  ${OPENSSL_LIBRARIES}
  ${KUDU_SYSTEM_LIBS}
  pthread
  dl
  rt
)

# Phase 4: Process Separation Solution
# Service process - has Kudu client, uses CAF I/O for networking
add_executable(phase4_kudu_service phase4_kudu_service.cc)
target_include_directories(phase4_kudu_service PRIVATE
  ${KUDU_CLIENT_INCLUDE_DIR}
  ${KUDU_BUILD_DIR}/src
)
target_link_libraries(phase4_kudu_service
  CAF::core
  CAF::io
  ${KUDU_LINK_LIBS}
  ${OPENSSL_LIBRARIES}
  ${KUDU_SYSTEM_LIBS}
  pthread
  dl
  rt
)

# Client process - CAF actors only, no Kudu code
add_executable(phase4_caf_client phase4_caf_client.cc)
target_link_libraries(phase4_caf_client
  CAF::core
  CAF::io
)

# Client V2 - simpler version without .then() continuations
add_executable(phase4_caf_client_v2 phase4_caf_client_v2.cc)
target_link_libraries(phase4_caf_client_v2
  CAF::core
  CAF::io
)

# Aeron IPC Examples (if Aeron is available)
if(HAVE_AERON)
  add_executable(aeron_publisher aeron_publisher.cpp)
  target_include_directories(aeron_publisher PRIVATE ${AERON_INCLUDE_DIR})
  target_link_libraries(aeron_publisher
    ${AERON_CLIENT_LIB}
    pthread
  )

  add_executable(aeron_subscriber aeron_subscriber.cpp)
  target_include_directories(aeron_subscriber PRIVATE ${AERON_INCLUDE_DIR})
  target_link_libraries(aeron_subscriber
    ${AERON_CLIENT_LIB}
    pthread
  )

  # CAF + Aeron Integration Examples
  add_executable(caf_aeron_service caf_aeron_service.cpp)
  target_include_directories(caf_aeron_service PRIVATE ${AERON_INCLUDE_DIR})
  target_link_libraries(caf_aeron_service
    CAF::core
    ${AERON_CLIENT_LIB}
    pthread
  )

  add_executable(caf_aeron_client caf_aeron_client.cpp)
  target_include_directories(caf_aeron_client PRIVATE ${AERON_INCLUDE_DIR})
  target_link_libraries(caf_aeron_client
    CAF::core
    ${AERON_CLIENT_LIB}
    pthread
  )

  # Phase 1: Kudu Service (Aeron + Kudu, NO CAF)
  add_executable(kudu_service_simple kudu_service_simple.cpp)
  target_include_directories(kudu_service_simple PRIVATE
    ${AERON_INCLUDE_DIR}
    ${KUDU_CLIENT_INCLUDE_DIR}
    ${KUDU_BUILD_DIR}/src
  )
  target_link_libraries(kudu_service_simple
    ${AERON_CLIENT_LIB}
    ${KUDU_LINK_LIBS}
    ${OPENSSL_LIBRARIES}
    ${KUDU_SYSTEM_LIBS}
    pthread
    dl
    rt
  )

  # Phase 1 Test Client
  add_executable(test_kudu_service test_kudu_service.cpp)
  target_include_directories(test_kudu_service PRIVATE ${AERON_INCLUDE_DIR})
  target_link_libraries(test_kudu_service
    ${AERON_CLIENT_LIB}
    pthread
  )

  # Phase 2: CAF Event Coordinator (CAF + Aeron, NO Kudu)
  add_executable(caf_event_coordinator caf_event_coordinator.cpp)
  target_include_directories(caf_event_coordinator PRIVATE ${AERON_INCLUDE_DIR})
  target_link_libraries(caf_event_coordinator
    CAF::core
    ${AERON_CLIENT_LIB}
    pthread
  )

  # Phase 3: Event Sourcing Components (CAF + Aeron, event-sourced actors)
  # Event sourcing library (shared by all event-sourced actors)
  add_library(event_sourcing STATIC
    event_sourcing.cpp
    equipment_health_actor.cpp
    recovery_coordinator.cpp
  )
  target_include_directories(event_sourcing PRIVATE ${AERON_INCLUDE_DIR})
  target_link_libraries(event_sourcing
    CAF::core
    ${AERON_CLIENT_LIB}
    pthread
  )

  # Event Sourcing Test - Recovery coordinator demo
  add_executable(test_event_sourcing test_event_sourcing.cpp)
  target_include_directories(test_event_sourcing PRIVATE ${AERON_INCLUDE_DIR})
  target_link_libraries(test_event_sourcing
    event_sourcing
    CAF::core
    ${AERON_CLIENT_LIB}
    pthread
  )

  # Manufacturing Event Generator - Continuous simulation
  add_executable(manufacturing_event_generator manufacturing_event_generator.cpp)
  target_include_directories(manufacturing_event_generator PRIVATE ${AERON_INCLUDE_DIR})
  target_link_libraries(manufacturing_event_generator
    event_sourcing
    CAF::core
    ${AERON_CLIENT_LIB}
    pthread
  )
endif()

message(STATUS "CAF Phases configured:")
message(STATUS "  phase1_hello_world - CAF only")
message(STATUS "  phase2_kudu_only - Kudu only")
message(STATUS "  phase3_actor_kudu - CAF + Kudu integration test (crashes)")
message(STATUS "  phase4_kudu_service - Process separation: Kudu service")
message(STATUS "  phase4_caf_client - Process separation: CAF client")

if(HAVE_AERON)
  message(STATUS "Aeron examples:")
  message(STATUS "  aeron_publisher - IPC publisher")
  message(STATUS "  aeron_subscriber - IPC subscriber")
  message(STATUS "CAF + Aeron integration:")
  message(STATUS "  caf_aeron_service - Service with CAF actors + Aeron IPC")
  message(STATUS "  caf_aeron_client - Client with CAF actors + Aeron IPC")
  message(STATUS "Event Sourcing - Phase 1 (Kudu + Aeron, NO CAF):")
  message(STATUS "  kudu_service_simple - Kudu persistence service")
  message(STATUS "  test_kudu_service - Test client for Kudu service")
endif()
