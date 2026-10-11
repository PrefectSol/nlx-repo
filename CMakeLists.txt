cmake_minimum_required(VERSION 3.20)

project(supervisor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

set(SV_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/config.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/cpu.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/file.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/file_descriptor.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/system_error.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/time.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/logging/logger.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/logging/logger_node.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/logging/logger_registry.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/supervisor.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
)

add_executable(${PROJECT_NAME} ${SV_SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
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

endif()

target_link_libraries(${PROJECT_NAME} PRIVATE pthread)

if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.9)
    cmake_policy(SET CMP0069 NEW)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported)
    if(ipo_supported)
        set_property(TARGET ${PROJECT_NAME}
            PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endif()
