cmake_minimum_required(VERSION 3.5)
project(cortex.llamacpp)
SET(TARGET engine)

if(UNIX AND NOT APPLE)
  set(CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS} -fPIC")
  add_compile_options(-fPIC)
endif()

set(THIRD_PARTY_PATH ${CMAKE_CURRENT_SOURCE_DIR}/build_deps/_install)

if(UNIX AND NOT APPLE)
  add_compile_options(-fPIC)
endif()

include(CheckIncludeFileCXX)
# CPP version
check_include_file_cxx(any HAS_ANY)
check_include_file_cxx(string_view HAS_STRING_VIEW)
check_include_file_cxx(coroutine HAS_COROUTINE)

set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

SET(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)

if(CORTEXLLAMA_VERSION)
  add_compile_definitions(CORTEXLLAMA_VERSION="${CORTEXLLAMA_VERSION}")
endif()

add_subdirectory(whisper.cpp)

add_library(${TARGET} SHARED 
    src/audio_engine.cc
    src/whisper_server_context.cc
)

find_library(JSONCPP
    NAMES jsoncpp
    HINTS "${THIRD_PARTY_PATH}/lib"
)

find_library(TRANTOR
    NAMES trantor
    HINTS "${THIRD_PARTY_PATH}/lib"
)

target_link_libraries(${TARGET} PRIVATE whisper ${JSONCPP} ${TRANTOR}
                                              ${CMAKE_THREAD_LIBS_INIT})
target_include_directories(${TARGET} PRIVATE 
            ${CMAKE_CURRENT_SOURCE_DIR}/base
            ${CMAKE_CURRENT_SOURCE_DIR}/whisper.cpp 
            ${THIRD_PARTY_PATH}/include)

target_compile_features(${TARGET} PUBLIC cxx_std_17)