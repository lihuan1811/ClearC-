cmake_minimum_required(VERSION 3.24)

project(CDriveCleanerQt VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Concurrent)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Concurrent)

set(CDRIVE_CLEANER_SOURCES
    src/main.cpp
    src/CleanupEngine.h
    src/CleanupEngine.cpp
    src/FileManagementEngine.h
    src/FileManagementEngine.cpp
    src/GpuOptimizationEngine.h
    src/GpuOptimizationEngine.cpp
    src/SoftwareUninstallEngine.h
    src/SoftwareUninstallEngine.cpp
    src/SystemCatalog.h
    src/SystemCatalog.cpp
    src/MainWindow.h
    src/MainWindow.cpp
)

if(QT_VERSION_MAJOR GREATER_EQUAL 6)
    qt_add_executable(CDriveCleanerQt ${CDRIVE_CLEANER_SOURCES})
else()
    add_executable(CDriveCleanerQt ${CDRIVE_CLEANER_SOURCES})
endif()

target_link_libraries(CDriveCleanerQt
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Widgets
        Qt${QT_VERSION_MAJOR}::Concurrent
)

if(WIN32)
    target_link_libraries(CDriveCleanerQt PRIVATE shell32 advapi32)
    set_target_properties(CDriveCleanerQt PROPERTIES
        WIN32_EXECUTABLE TRUE
        OUTPUT_NAME "C DiskGlow"
    )
endif()
