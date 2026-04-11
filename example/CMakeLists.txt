cmake_minimum_required(VERSION 3.31)
project(example)

set(CMAKE_CXX_STANDARD 20)

include_directories(src)
include_directories(third_party)

#Crée l'exécutable
add_executable(example
        main.cpp
        src/example.cpp
)

#Déclare libhrl.a comme une bibliothèque IMPORTED
add_library(hrl STATIC IMPORTED)
set_target_properties(hrl PROPERTIES IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/lib/libhrldll.dll.a")

#Déclare libglfw3.a aussi si besoin
add_library(glfw3 STATIC IMPORTED)
set_target_properties(glfw3 PROPERTIES IMPORTED_LOCATION "${CMAKE_SOURCE_DIR}/lib/libglfw3.a")


#Lie les libs proprement
target_link_libraries(example PRIVATE
        -Wl,--whole-archive hrl -Wl,--no-whole-archive
        glfw3
        opengl32
)
