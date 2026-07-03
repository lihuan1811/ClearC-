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
    src/DismRuleScanner.h
    src/DismRuleScanner.cpp
    src/FileManagementEngine.h
    src/FileManagementEngine.cpp
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

set(DISMPP_RULE_FILE "${CMAKE_SOURCE_DIR}/rules/dismpp/Data.xml")
if(NOT EXISTS "${DISMPP_RULE_FILE}")
    message(WARNING "Dism++ rules/dismpp/Data.xml was not found; professional cleanup rules will be limited.")
endif()

if(WIN32)
    set_target_properties(CDriveCleanerQt PROPERTIES
        WIN32_EXECUTABLE TRUE
        OUTPUT_NAME "C DiskGlow"
    )
endif()

add_custom_command(TARGET CDriveCleanerQt POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/rules/dismpp"
        "$<TARGET_FILE_DIR:CDriveCleanerQt>/rules/dismpp"
)
