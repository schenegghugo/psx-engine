file(GLOB_RECURSE ENGINE_SRC CONFIGURE_DEPENDS src/*.cpp src/**/*.cpp)

add_executable(psx_engine ${ENGINE_SRC})

target_include_directories(psx_engine PRIVATE
    ${CMAKE_SOURCE_DIR}/vendor/imgui
    ${CMAKE_SOURCE_DIR}/vendor/imgui/backends
    ${CMAKE_SOURCE_DIR}/vendor/glad/include
)

target_link_libraries(psx_engine PRIVATE
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

add_custom_command(TARGET psx_engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/engine/shaders
        $<TARGET_FILE_DIR:psx_engine>/shaders
    COMMENT "Copying engine shaders"
)

add_custom_command(TARGET psx_engine POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_SOURCE_DIR}/assets
        $<TARGET_FILE_DIR:psx_engine>/assets
    COMMENT "Copying assets"
)

target_compile_options(psx_engine PRIVATE
    $<$<CONFIG:Release>:-O3 -march=native -funroll-loops>
)
