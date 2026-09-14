cmake_minimum_required(VERSION 3.25)
project(ThunderboltTestPackage LANGUAGES CXX)

find_package(Thunderbolt REQUIRED)

add_executable(test_package test_package.cpp)
target_link_libraries(test_package PRIVATE thunderbolt::thunderbolt)
