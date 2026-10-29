cmake_minimum_required(VERSION 3.16)
project(CheetahTrajCpp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(cheetahtraj
    src/io.cpp
    src/projection.cpp
    src/vqgs.cpp
    src/quadtree.cpp
    src/invquad.cpp
    src/temporal.cpp
    src/temporal_invquad.cpp
)

target_include_directories(cheetahtraj PUBLIC include)

if(MSVC)
    target_compile_options(cheetahtraj PRIVATE /W4 /permissive-)
else()
    target_compile_options(cheetahtraj PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(cheetahtraj-cli src/main.cpp)
target_link_libraries(cheetahtraj-cli PRIVATE cheetahtraj)

add_executable(cheetahtraj-temporal src/temporal_main.cpp)
target_link_libraries(cheetahtraj-temporal PRIVATE cheetahtraj)

add_executable(cheetahtraj-spatiotemporal src/spatiotemporal_main.cpp)
target_link_libraries(cheetahtraj-spatiotemporal PRIVATE cheetahtraj)

if(WIN32)
    add_executable(cheetahtraj-viewer src/viewer_win32.cpp)
    target_link_libraries(cheetahtraj-viewer PRIVATE cheetahtraj gdi32 user32)

    add_executable(cheetahtraj-temporal-visual-color src/temporal_visual_main.cpp)
    target_link_libraries(cheetahtraj-temporal-visual-color PRIVATE cheetahtraj gdi32 user32)
endif()

enable_testing()
add_executable(cheetahtraj-tests tests/test_core.cpp)
target_link_libraries(cheetahtraj-tests PRIVATE cheetahtraj)
add_test(NAME core-tests COMMAND cheetahtraj-tests)

add_executable(cheetahtraj-temporal-tests tests/test_temporal.cpp)
target_link_libraries(cheetahtraj-temporal-tests PRIVATE cheetahtraj)
add_test(NAME temporal-tests COMMAND cheetahtraj-temporal-tests)
