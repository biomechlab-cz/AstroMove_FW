cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME AstroMoveCubeMX)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Generate recording_key.h from recording.key — one key file shared by
# the firmware and decode_emgx.py. Editing recording.key re-runs configure.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/recording.key" REC_KEY_FILE)
string(REGEX MATCH "key[ \t]*=[ \t]*([0-9a-fA-F]+)" _ "${REC_KEY_FILE}")
set(REC_KEY_HEX "${CMAKE_MATCH_1}")
string(LENGTH "${REC_KEY_HEX}" REC_KEY_LEN)
if(NOT REC_KEY_LEN EQUAL 64)
    message(FATAL_ERROR "recording.key: 'key =' must be 64 hex chars (AES-256), got ${REC_KEY_LEN}")
endif()
string(REGEX MATCH "id[ \t]*=[ \t]*([0-9]+)" _ "${REC_KEY_FILE}")
set(REC_KEY_ID "${CMAKE_MATCH_1}")
string(REGEX MATCH "version[ \t]*=[ \t]*([0-9]+)" _ "${REC_KEY_FILE}")
set(REC_KEY_VERSION "${CMAKE_MATCH_1}")
string(REGEX REPLACE "(..)" "0x\\1, " REC_KEY_BYTES "${REC_KEY_HEX}")
configure_file(cmake/recording_key.h.in generated/recording_key.h @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS recording.key)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    Core/Src/i2c_sensors.c
    Core/Src/ads1292.c
    Core/Src/acquisition.c
    Core/Src/recording.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    Core/Inc
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
