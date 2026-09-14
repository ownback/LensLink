cmake_minimum_required(VERSION 3.24)
project(lenslink-desktop C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT DEFINED LENSLINK_VERSION)
	set(LENSLINK_VERSION "dev")
endif()
add_compile_definitions(LENSLINK_VERSION="${LENSLINK_VERSION}")

find_package(PkgConfig REQUIRED)
pkg_check_modules(AVCODEC REQUIRED libavcodec libavutil)
pkg_check_modules(PIPEWIRE REQUIRED libpipewire-0.3)
pkg_check_modules(USBMUXD REQUIRED libusbmuxd-2.0)

find_package(Threads REQUIRED)

# Core library shared by the daemon (and linked once; the tray talks over
# IPC instead of linking it).
add_library(llcore STATIC
	src/json.c
	src/config.c
	src/decode.c
	src/vcam.c
	src/vmic.c
	src/session.c
	src/daemon.c
	src/ipc.c
	../obs-plugin/src/usbmux.c
	../obs-plugin/src/mdns.c
)
target_include_directories(llcore PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}/src
	${CMAKE_CURRENT_SOURCE_DIR}/../obs-plugin/src
	${CMAKE_CURRENT_SOURCE_DIR}/src/obs-shim
)
target_include_directories(llcore SYSTEM PRIVATE
	${AVCODEC_INCLUDE_DIRS} ${PIPEWIRE_INCLUDE_DIRS} ${USBMUXD_INCLUDE_DIRS}
)
target_link_libraries(llcore PUBLIC
	${AVCODEC_LIBRARIES} ${PIPEWIRE_LIBRARIES} ${USBMUXD_LIBRARIES}
	Threads::Threads
)
target_compile_options(llcore PRIVATE -Wall -Wextra -Werror)

add_executable(lenslinkd src/main.c)
target_link_libraries(lenslinkd PRIVATE llcore)
target_compile_options(lenslinkd PRIVATE -Wall -Wextra -Werror)

find_package(Qt6 COMPONENTS Widgets QUIET)
if(Qt6_FOUND)
	add_executable(lenslink-tray tray/main.cpp src/ipc.c)
	target_link_libraries(lenslink-tray PRIVATE Qt6::Widgets)
	target_compile_options(lenslink-tray PRIVATE -Wall -Wextra -Werror)
	message(STATUS "Qt6 found — building lenslink-tray")
else()
	message(STATUS "Qt6 not found — lenslink-tray will not be built")
endif()

install(TARGETS lenslinkd RUNTIME DESTINATION bin)
if(Qt6_FOUND)
	install(TARGETS lenslink-tray RUNTIME DESTINATION bin)
endif()
