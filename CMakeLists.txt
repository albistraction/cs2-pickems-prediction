cmake_minimum_required(VERSION 3.16)
project(pickems_prediction CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)  # -std=c++17 em vez de gnu++17 (sem macros como "unix")
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)
find_package(OpenSSL)
find_package(ZLIB)

add_executable(pickems-prediction
  src/main.cpp
  src/hltv.cpp
  src/curation.cpp
  src/model.cpp)

target_include_directories(pickems-prediction PRIVATE src third_party)
target_link_libraries(pickems-prediction PRIVATE Threads::Threads)

if(OPENSSL_FOUND)
  target_compile_definitions(pickems-prediction PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
  target_link_libraries(pickems-prediction PRIVATE OpenSSL::SSL OpenSSL::Crypto)
else()
  message(WARNING "OpenSSL nao encontrado: a coleta da HLTV (HTTPS) ficara desativada")
endif()

if(ZLIB_FOUND)
  target_compile_definitions(pickems-prediction PRIVATE CPPHTTPLIB_ZLIB_SUPPORT)
  target_link_libraries(pickems-prediction PRIVATE ZLIB::ZLIB)
endif()

if(WIN32)
  target_link_libraries(pickems-prediction PRIVATE ws2_32 crypt32)
endif()
