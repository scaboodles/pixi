cmake_minimum_required(VERSION 3.10)
project(pixi C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(JPEG REQUIRED libjpeg)
pkg_check_modules(NCURSES REQUIRED ncurses)
pkg_check_modules(LIBAV REQUIRED libavformat libavcodec libavutil libswscale)

add_executable(pixi pixi.c pixiv.c)

target_include_directories(pixi PRIVATE
    ${JPEG_INCLUDE_DIRS}
    ${NCURSES_INCLUDE_DIRS}
    ${LIBAV_INCLUDE_DIRS}
)

target_link_directories(pixi PRIVATE
    ${JPEG_LIBRARY_DIRS}
    ${NCURSES_LIBRARY_DIRS}
    ${LIBAV_LIBRARY_DIRS}
)

target_link_libraries(pixi
    ${JPEG_LIBRARIES}
    ${NCURSES_LIBRARIES}
    ${LIBAV_LIBRARIES}
)
