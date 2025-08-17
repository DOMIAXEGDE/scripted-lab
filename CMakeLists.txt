cmake_minimum_required(VERSION 3.20)
project(scripted LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)

add_executable(scripted_cli scripted.cpp)
target_include_directories(scripted_cli PRIVATE .)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()

add_executable(scripted_qt qt_view.cpp)
target_include_directories(scripted_qt PRIVATE .)
target_link_libraries(scripted_qt PRIVATE Qt6::Widgets)