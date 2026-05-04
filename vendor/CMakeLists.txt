# ── GLAD ─────────────────────────────────────────────────────────────────────
add_library(glad STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/glad/src/glad.c
)
target_include_directories(glad PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/glad/include
)
target_link_libraries(glad PUBLIC ${CMAKE_DL_LIBS})

# ── Dear ImGui ────────────────────────────────────────────────────────────────
find_package(SDL2 CONFIG REQUIRED)

add_library(imgui STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/imgui.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/imgui_draw.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/imgui_tables.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/imgui_widgets.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/imgui_demo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends/imgui_impl_sdl2.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/imgui/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/glad/include
)
target_link_libraries(imgui PUBLIC SDL2::SDL2 glad)
target_compile_definitions(imgui PUBLIC IMGUI_IMPL_OPENGL_LOADER_GLAD)
message(STATUS "GLAD source path: ${CMAKE_CURRENT_SOURCE_DIR}/glad/src/glad.c")
message(STATUS "File exists: ${CMAKE_CURRENT_SOURCE_DIR}/glad/src/glad.c")

# ── ImGuizmo (optional) ───────────────────────────────────────────────────────
# To enable: copy src/ImGuizmo.h and src/ImGuizmo.cpp from
#   https://github.com/CedricGuillemet/ImGuizmo  into vendor/imguizmo/
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/imguizmo/ImGuizmo.h)
    add_library(imguizmo STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/imguizmo/ImGuizmo.cpp
    )
    target_include_directories(imguizmo PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/imguizmo
    )
    target_link_libraries(imguizmo PUBLIC imgui)
    message(STATUS "ImGuizmo: found — gizmo enabled")
else()
    message(STATUS "ImGuizmo: not found in vendor/imguizmo/ — gizmo disabled")
endif()
