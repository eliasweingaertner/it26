cmake_minimum_required(VERSION 3.16)
project(itplay C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Engine + driver + loader, shared by the player and the editor.
set(ENGINE_SOURCES
    src/it_music.c
    src/it_effects.c
    src/it_tables.c
    src/it_driver.c
    src/it_load.c
    src/it_pattern.c
)

add_executable(itplay ${ENGINE_SOURCES} src/main.c)
add_executable(ited   ${ENGINE_SOURCES} src/it_screen.c src/it_editor.c)

foreach(tgt itplay ited)
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
