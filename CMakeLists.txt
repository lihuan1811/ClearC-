cmake_minimum_required(VERSION 3.24)

project(CDriveCleanerQt VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent)

qt_add_executable(CDriveCleanerQt
    src/main.cpp
    src/CleanupEngine.h
    src/CleanupEngine.cpp
    src/SystemCatalog.h
    src/SystemCatalog.cpp
    src/AccountStore.h
    src/AccountStore.cpp
    src/MainWindow.h
    src/MainWindow.cpp
)

target_link_libraries(CDriveCleanerQt
    PRIVATE
        Qt6::Widgets
        Qt6::Concurrent
)

if(WIN32)
    set_target_properties(CDriveCleanerQt PROPERTIES
        WIN32_EXECUTABLE TRUE
        OUTPUT_NAME "C DiskGlow"
    )
endif()
