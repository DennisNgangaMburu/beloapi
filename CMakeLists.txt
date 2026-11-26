cmake_minimum_required(VERSION 3.16)
project(beloapi VERSION 1.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(beloapi server.cpp)

if(WIN32)
    # WinHTTP ships with Windows; no runtime dependencies once linked statically.
    target_link_libraries(beloapi PRIVATE winhttp ws2_32)
    target_compile_definitions(beloapi PRIVATE _WIN32_WINNT=0x0601)
    if(MINGW)
        # Produce a single portable .exe for the release artifact.
        target_link_options(beloapi PRIVATE -static)
    endif()
else()
    find_package(Threads REQUIRED)
    target_link_libraries(beloapi PRIVATE Threads::Threads)
endif()

if(MSVC)
    target_compile_options(beloapi PRIVATE /W4 /permissive-)
else()
    target_compile_options(beloapi PRIVATE -Wall -Wextra)
endif()

install(TARGETS beloapi RUNTIME DESTINATION bin)
