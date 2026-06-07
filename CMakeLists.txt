cmake_minimum_required(VERSION 3.20)
project(xmodelconverteriw3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(xmodelconverteriw3
    main.cpp
    model.cpp
    parts.cpp
    surface.cpp
    animation.cpp
)

# GLM is header-only; just point at the submodule root so
# #include <glm/glm.hpp> resolves correctly.
target_compile_definitions(xmodelconverteriw3 PRIVATE GLM_ENABLE_EXPERIMENTAL)

target_include_directories(xmodelconverteriw3 PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/glm
)

# Commdlg (file-open dialog) on Windows
if(WIN32)
    target_link_libraries(xmodelconverteriw3 PRIVATE comdlg32)
endif()