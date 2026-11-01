cmake_minimum_required(VERSION 3.20)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

project(supervisor LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

set(THIRDPARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/3rdparty")
set(LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib")

set(CURL_LIB "${LIB_DIR}/curl/lib/libcurl.a")
set(SSL_LIB "${LIB_DIR}/openssl/lib64/libssl.a")
set(CRYPTO_LIB "${LIB_DIR}/openssl/lib64/libcrypto.a")
set(ZLIBNG_LIB "${LIB_DIR}/zlib-ng/lib/libz-ng.a")
set(SIMDJSON_LIB "${LIB_DIR}/simdjson/lib/libsimdjson.a")

add_library(dep_curl STATIC IMPORTED GLOBAL)
set_target_properties(dep_curl PROPERTIES
    IMPORTED_LOCATION "${CURL_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${LIB_DIR}/curl/include"
)

add_library(dep_ssl STATIC IMPORTED GLOBAL)
set_target_properties(dep_ssl PROPERTIES
    IMPORTED_LOCATION "${SSL_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${LIB_DIR}/openssl/include"
)

add_library(dep_crypto STATIC IMPORTED GLOBAL)
set_target_properties(dep_crypto PROPERTIES
    IMPORTED_LOCATION "${CRYPTO_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${LIB_DIR}/openssl/include"
)

add_library(dep_zlibng STATIC IMPORTED GLOBAL)
set_target_properties(dep_zlibng PROPERTIES
    IMPORTED_LOCATION "${ZLIBNG_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${LIB_DIR}/zlib-ng/include"
)

add_library(dep_simdjson STATIC IMPORTED GLOBAL)
set_target_properties(dep_simdjson PROPERTIES
    IMPORTED_LOCATION "${SIMDJSON_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${LIB_DIR}/simdjson/include"
)

set(SV_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/config.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/credentials.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/crypto.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/file.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/file_descriptor.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/system_error.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/thread.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/time.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/exchange/okx_http_client.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/exchange/okx_rate_limiter.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/exchange/okx_streamer.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/logging/logger.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/logging/logger_node.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/logging/logger_registry.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/network/http_client.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/network/websocket_client.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/parsing/env_reader.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/parsing/json_reader.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/service/screener.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/service/strategy.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/supervisor.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
)

add_executable(${PROJECT_NAME} ${SV_SOURCES})

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)

target_include_directories(${PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE
    "${THIRDPARTY_DIR}/websocketclient-cpp/include"
    "${THIRDPARTY_DIR}/websocketclient-cpp"
    "${LIB_DIR}/openssl/include"
    "${LIB_DIR}/simdjson/include"
    "${LIB_DIR}/curl/include"
    "${LIB_DIR}/zlib-ng/include"
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    CURL_STATICLIB
    WS_CLIENT_USE_ZLIB_NG
    WS_CLIENT_VALIDATE_UTF8=0
    WS_CLIENT_LOG_DNS=0
    WS_CLIENT_LOG_TCP=0
    WS_CLIENT_LOG_SSL=0
    WS_CLIENT_LOG_HANDSHAKE=0
    WS_CLIENT_LOG_COMPRESSION=0
    WS_CLIENT_LOG_SEND_FRAME=0
    WS_CLIENT_LOG_SEND_FRAME_PAYLOAD=0
    WS_CLIENT_LOG_RECV_FRAME=0
    WS_CLIENT_LOG_RECV_FRAME_PAYLOAD=0
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    CA_CERT_PATH="certs/cacert.pem"
)

target_compile_options(${PROJECT_NAME} PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Wconversion
    -Wshadow
    -Wno-unused-parameter
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Woverloaded-virtual
    -Wnull-dereference
    -Wdouble-promotion
    -Wformat=2
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    dep_curl
    dep_ssl
    dep_crypto
    dep_zlibng
    dep_simdjson
    pthread
    dl
)

cmake_policy(SET CMP0069 NEW)
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo_supported)
if(ipo_supported)
    set_property(TARGET ${PROJECT_NAME}
        PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()
