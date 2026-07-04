cmake_minimum_required(VERSION 3.16)
project(ittrack C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Optional SDL2 pixel backend for the editor on POSIX (Linux/macOS). When
# enabled and SDL2 is found, ited gets the authentic VGA window; otherwise
# the editor falls back to the no-deps terminal backend. itplay never uses it.
option(ITED_SDL "Build the ited SDL pixel backend (POSIX authentic window)" ON)

# Engine + driver + loader, shared by the player and the editor.
set(ENGINE_SOURCES
    src/it_music.c
    src/it_effects.c
    src/it_tables.c
    src/it_driver.c
    src/it_load.c
    src/it_pattern.c
    src/it_save.c
)

add_executable(itplay ${ENGINE_SOURCES} src/main.c)

set(EDITOR_SOURCES
    src/it_import.c
    src/it_ris.c
    src/it_screen.c
    src/it_vgadata.c
    src/it_editor.c
)
if(WIN32)
    list(APPEND EDITOR_SOURCES src/it_screen_win32.c)
endif()

# Discover SDL2 for the optional POSIX pixel backend.
set(ITED_HAVE_SDL FALSE)
if(ITED_SDL AND NOT WIN32)
    find_package(SDL2 QUIET)
    if(SDL2_FOUND)
        set(ITED_HAVE_SDL TRUE)
        list(APPEND EDITOR_SOURCES src/it_screen_sdl.c)
    else()
        message(STATUS
            "ited: SDL2 not found; building editor with the terminal backend only")
    endif()
endif()

add_executable(ited ${ENGINE_SOURCES} ${EDITOR_SOURCES})
if(WIN32)
    target_link_libraries(ited user32 gdi32)
endif()
if(ITED_HAVE_SDL)
    target_compile_definitions(ited PRIVATE HAVE_SDL)
    if(TARGET SDL2::SDL2)
        target_link_libraries(ited SDL2::SDL2)
    else()
        target_include_directories(ited PRIVATE ${SDL2_INCLUDE_DIRS})
        target_link_libraries(ited ${SDL2_LIBRARIES})
    endif()
endif()

add_executable(test_pattern ${ENGINE_SOURCES} tests/test_pattern.c)

foreach(tgt itplay ited test_pattern)
    if(MSVC)
        target_compile_options(${tgt} PRIVATE /W3 /D_CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${tgt} PRIVATE
            -Wall -Wextra -Wno-unused-parameter)
        target_link_libraries(${tgt} m)
        if(NOT WIN32)
            find_package(Threads REQUIRED)
            target_link_libraries(${tgt} Threads::Threads ${CMAKE_DL_LIBS})
        endif()
    endif()
endforeach()
