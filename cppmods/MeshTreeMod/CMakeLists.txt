cmake_minimum_required(VERSION 3.22)
set(TARGET MeshTreeMod)
project(${TARGET})

add_library(${TARGET} SHARED
        src/dllmain.cpp
        src/MeshTreeMod.cpp
)

target_include_directories(${TARGET} PRIVATE "include")
target_compile_definitions(${TARGET} PRIVATE _WIN32_WINNT=0x0A00 WINVER=0x0A00)
target_link_libraries(${TARGET} PRIVATE ImGui)
target_link_libraries(${TARGET} PUBLIC UE4SS)
