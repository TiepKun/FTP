cmake_minimum_required(VERSION 3.16)
project(ftp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

pkg_check_modules(GTKMM QUIET IMPORTED_TARGET gtkmm-3.0)
if(NOT GTKMM_FOUND)
    message(FATAL_ERROR
        "gtkmm-3.0 not found. Install the development package "
        "(macOS: brew install gtkmm3; Ubuntu/Debian: sudo apt-get install libgtkmm-3.0-dev).")
endif()

add_library(common STATIC
    common/Utils.cpp
    common/Protocol.cpp
)
target_include_directories(common PUBLIC ${PROJECT_SOURCE_DIR}/common)

add_executable(fileshare_server
    server/main.cpp
    server/FileServer.cpp
    server/ClientSession.cpp
    server/Logger.cpp
    server/QuotaManager.cpp
    server/DbSqlite.cpp
)
target_include_directories(fileshare_server PRIVATE
    ${PROJECT_SOURCE_DIR}/server
    ${PROJECT_SOURCE_DIR}/common
)
target_link_libraries(fileshare_server PRIVATE
    common
    SQLite::SQLite3
    Threads::Threads
)

add_executable(fileshare_client
    client/main.cpp
    client/LoginWindow.cpp
    client/MainWindow.cpp
    client/NetworkClient.cpp
)
target_include_directories(fileshare_client PRIVATE
    ${PROJECT_SOURCE_DIR}/client
    ${PROJECT_SOURCE_DIR}/common
)
target_link_libraries(fileshare_client PRIVATE
    common
    PkgConfig::GTKMM
)
