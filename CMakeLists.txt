if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
option(SANITIZE "debug sanitizers" OFF)
cmake_minimum_required(VERSION 3.16)
project(inventory CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PQXX REQUIRED libpqxx)
pkg_check_modules(JSON REQUIRED nlohmann_json)
pkg_check_modules(SODIUM REQUIRED libsodium)
pkg_check_modules(OPENSSL REQUIRED openssl)

add_executable(inventory src/main.cpp)

target_include_directories(inventory PRIVATE src
  ${PQXX_INCLUDE_DIRS} ${JSON_INCLUDE_DIRS} ${SODIUM_INCLUDE_DIRS} ${OPENSSL_INCLUDE_DIRS})
target_link_directories(inventory PRIVATE
  ${PQXX_LIBRARY_DIRS} ${SODIUM_LIBRARY_DIRS} ${OPENSSL_LIBRARY_DIRS})
target_link_libraries(inventory PRIVATE
  ${PQXX_LIBRARIES} ${SODIUM_LIBRARIES} ${OPENSSL_LIBRARIES})

target_compile_options(inventory PRIVATE -Wall -Wextra)
if(SANITIZE)
  target_compile_options(inventory PRIVATE -fsanitize=address,undefined)
  target_link_options(inventory PRIVATE -fsanitize=address,undefined)
endif()