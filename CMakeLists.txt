cmake_minimum_required(VERSION 3.24)

project(CDriveCleanerQt VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
include(FetchContent)

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
    src/NvidiaNvapiBridge.h
    src/NvidiaNvapiBridge.cpp
    src/AmdAdlxBridge.h
    src/AmdAdlxBridge.cpp
    src/SoftwareUninstallEngine.h
    src/SoftwareUninstallEngine.cpp
    src/SystemCatalog.h
    src/SystemCatalog.cpp
    src/MainWindow.h
    src/MainWindow.cpp
)

if(WIN32)
    FetchContent_Declare(
        nvapi
        GIT_REPOSITORY https://github.com/NVIDIA/nvapi.git
        GIT_TAG cd6918f60b3c9a0476fdfe7e89bb32330602049d
        GIT_SHALLOW FALSE
    )
    FetchContent_Declare(
        adlx
        GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/ADLX.git
        GIT_TAG v1.5
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(nvapi adlx)
    list(APPEND CDRIVE_CLEANER_SOURCES
        ${adlx_SOURCE_DIR}/SDK/ADLXHelper/Windows/Cpp/ADLXHelper.cpp
        ${adlx_SOURCE_DIR}/SDK/Platform/Windows/WinAPIs.cpp
    )
endif()

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

if(MSVC)
    target_compile_options(CDriveCleanerQt PRIVATE /utf-8)
endif()

if(WIN32)
    target_include_directories(CDriveCleanerQt PRIVATE ${nvapi_SOURCE_DIR} ${adlx_SOURCE_DIR})
    target_link_libraries(CDriveCleanerQt PRIVATE shell32 advapi32 PowrProf)
    set_target_properties(CDriveCleanerQt PROPERTIES
        WIN32_EXECUTABLE TRUE
        OUTPUT_NAME "C DiskGlow"
    )
endif()

if(BUILD_TESTING)
    add_executable(CDriveCleanerEngineTests
        tests/EngineBehaviorTests.cpp
        src/CleanupEngine.h
        src/CleanupEngine.cpp
        src/FileManagementEngine.h
        src/FileManagementEngine.cpp
        src/SoftwareUninstallEngine.h
        src/SoftwareUninstallEngine.cpp
        src/SystemCatalog.h
        src/SystemCatalog.cpp
    )
    target_include_directories(CDriveCleanerEngineTests PRIVATE src)
    target_link_libraries(CDriveCleanerEngineTests
        PRIVATE
            Qt${QT_VERSION_MAJOR}::Widgets
    )
    if(MSVC)
        target_compile_options(CDriveCleanerEngineTests PRIVATE /utf-8)
    endif()
    if(WIN32)
        target_link_libraries(CDriveCleanerEngineTests PRIVATE shell32 advapi32 PowrProf)
    endif()
    add_test(NAME CDriveCleanerEngineTests COMMAND CDriveCleanerEngineTests)
endif()
