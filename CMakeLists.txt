cmake_minimum_required(VERSION 3.2)
project(proxy_server)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -pthread -Wall -Wextra")

set(SOURCE_FILES main.cpp util/header_parser.cpp
        util/header_parser.h proxy/proxy_server.cpp proxy/proxy_server.h proxy/resolver.h util/util.cpp
        util/util.h util/wraps.cpp util/wraps.h util/buffered_message.h util/buffered_message.cpp)

add_executable(proxy_server ${SOURCE_FILES})
