cmake_minimum_required(VERSION 3.20)

project(kio-chomikuj VERSION 0.1.0 LANGUAGES CXX)

set(QT_MIN_VERSION 6.5.0)
set(KF_MIN_VERSION 6.3.0)

find_package(ECM ${KF_MIN_VERSION} REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH} ${CMAKE_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(CTest)

find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Core Network Widgets)
if (BUILD_TESTING)
    find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Test)
endif ()
find_package(KF6 ${KF_MIN_VERSION} REQUIRED COMPONENTS Config DocTools I18n KCMUtils KIO)
find_package(ZLIB REQUIRED)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
add_compile_options(-fexceptions)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_definitions(-DQT_NO_URL_CAST_FROM_STRING)
add_definitions(-DQT_STRICT_ITERATORS)

add_library(chomikujcore STATIC
    src/core/chomikujtypes.h
    src/core/chomikujtypes.cpp
    src/core/chomikujerror.h
    src/core/chomikujerror.cpp
    src/core/signing.h
    src/core/signing.cpp
    src/core/httptransport.h
    src/core/httptransport.cpp
    src/core/qthttptransport.h
    src/core/qthttptransport.cpp
    src/core/retry.h
    src/core/retry.cpp
    src/core/chomikujclient.h
    src/core/chomikujclient.cpp
    src/core/chomikujfilesystem.h
    src/core/chomikujfilesystem.cpp
    src/core/uploader.h
    src/core/uploader.cpp
)
target_include_directories(chomikujcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(chomikujcore
    PUBLIC
        Qt6::Core
        Qt6::Network
        KF6::KIOCore
    PRIVATE
        ZLIB::ZLIB
)
target_compile_definitions(chomikujcore PRIVATE QT_NO_CAST_FROM_ASCII)

kcoreaddons_add_plugin(kio_chomikuj
    SOURCES
        src/worker/chomikujworker.cpp
        src/worker/chomikuj.json
    INSTALL_NAMESPACE kf6/kio
)
target_link_libraries(kio_chomikuj
    Qt6::Core
    Qt6::Network
    KF6::KIOCore
    KF6::I18n
    KF6::ConfigCore
    chomikujcore
)
set_target_properties(kio_chomikuj PROPERTIES OUTPUT_NAME "chomikuj")

add_subdirectory(src/kcm)
add_subdirectory(doc)

find_package(Gettext REQUIRED)
ki18n_install(po)

if (BUILD_TESTING)
    add_subdirectory(autotests)
endif ()
