file(GLOB_RECURSE EDITOR_SRC CONFIGURE_DEPENDS src/*.cpp src/**/*.cpp)

add_executable(psx_editor ${EDITOR_SRC})

target_include_directories(psx_editor PRIVATE
    ${CMAKE_SOURCE_DIR}/vendor/imgui
    ${CMAKE_SOURCE_DIR}/vendor/imgui/backends
    ${CMAKE_SOURCE_DIR}/vendor/glad/include
)

target_link_libraries(psx_editor PRIVATE
    psx_shared
    imgui
    glad
    SDL2::SDL2
    SDL2::SDL2main
    glm::glm
    nlohmann_json::nlohmann_json
    EnTT::EnTT
    ${CMAKE_DL_LIBS}
)

# Wire in ImGuizmo if it was found by vendor/CMakeLists.txt
if(TARGET imguizmo)
    target_link_libraries(psx_editor PRIVATE imguizmo)
    target_include_directories(psx_editor PRIVATE
        ${CMAKE_SOURCE_DIR}/vendor/imguizmo
    )
    target_compile_definitions(psx_editor PRIVATE HAVE_IMGUIZMO)
endif()

add_custom_command(TARGET psx_editor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/editor/shaders
        $<TARGET_FILE_DIR:psx_editor>/shaders
    COMMENT "Copying editor shaders"
)

target_compile_options(psx_editor PRIVATE
    $<$<CONFIG:Release>:-O3 -march=native -funroll-loops>
)
