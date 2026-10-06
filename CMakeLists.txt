cmake_minimum_required(VERSION 3.25)
project(pipewire-waybar LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Wno-missing-field-initializers)

include(GNUInstallDirs)
find_package(PkgConfig REQUIRED)
find_package(nlohmann_json 3.10 REQUIRED)
find_package(sdbus-c++ 2.0 REQUIRED)

pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

option(BUILD_PICKER "Build GTK4 layer-shell sink picker" ON)
if(BUILD_PICKER)
    pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
    pkg_check_modules(GTK4_LAYER_SHELL REQUIRED IMPORTED_TARGET gtk4-layer-shell-0)
endif()

# ---------- common static library ----------
add_library(phc_common STATIC
    src/common/config.cpp
    src/common/state.cpp
    src/common/log.cpp
)
target_include_directories(phc_common PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(phc_common PUBLIC
    nlohmann_json::nlohmann_json
)

# ---------- daemon ----------
add_executable(pipewire-waybard
    src/daemon/main.cpp
    src/daemon/pw_client.cpp
    src/daemon/router.cpp
    src/daemon/combine_manager.cpp
    src/daemon/dbus_server.cpp
    src/daemon/config_watcher.cpp
)
target_link_libraries(pipewire-waybard PRIVATE
    phc_common
    PkgConfig::PIPEWIRE
    SDBusCpp::sdbus-c++
)

# ---------- cli ----------
add_executable(pipewire-waybar
    src/cli/main.cpp
    src/cli/genconfig.cpp
)
target_link_libraries(pipewire-waybar PRIVATE
    phc_common
    PkgConfig::PIPEWIRE
    SDBusCpp::sdbus-c++
)

# ---------- picker ----------
if(BUILD_PICKER)
    add_executable(pipewire-waybar-picker
        src/picker/main.cpp
    )
    target_link_libraries(pipewire-waybar-picker PRIVATE
        phc_common
        SDBusCpp::sdbus-c++
        PkgConfig::GTK4
        PkgConfig::GTK4_LAYER_SHELL
    )
endif()

# ---------- install ----------
install(TARGETS pipewire-waybard pipewire-waybar
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(BUILD_PICKER)
    install(TARGETS pipewire-waybar-picker
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
configure_file(
    ${CMAKE_SOURCE_DIR}/contrib/pipewire-waybar.service.in
    ${CMAKE_BINARY_DIR}/pipewire-waybar.service
    @ONLY)
install(FILES ${CMAKE_BINARY_DIR}/pipewire-waybar.service
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/user)
install(FILES config.example.json
        DESTINATION ${CMAKE_INSTALL_DATADIR}/pipewire-waybar)
