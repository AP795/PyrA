cmake_minimum_required(VERSION 3.20)
project(PyraInstalledConsumer LANGUAGES C)

find_package(Pyra CONFIG REQUIRED)
add_executable(pyra_installed_consumer ../c_header_test.c)
target_link_libraries(pyra_installed_consumer PRIVATE Pyra::pyra)
