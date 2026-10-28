cmake_minimum_required(VERSION 3.20)
project(Try2Create LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
    # Static CRT so the exe carries no MSVC redistributable dependency.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/W3 /utf-8 /EHsc /MP)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/vendor/imgui)

add_executable(Try2Create WIN32
    src/main.cpp
    src/metadata.cpp
    src/metadata_png.cpp
    src/metadata_jpeg.cpp
    src/exif_util.cpp
    src/metadata_parsers.cpp
    src/image_texture.cpp
    src/scrape.cpp
    src/app_state.cpp
    src/win_dialogs.cpp
    src/disk_cache.cpp
    src/tag_generator.cpp
    src/tag_import.cpp
    src/nsfw_classifier.cpp
    src/organizer.cpp
    src/vision_prompt.cpp
    src/ui_theme.cpp
    src/image_attributes.cpp
    src/image_hash.cpp
    src/person_detector.cpp
    src/folder_watcher.cpp
    src/metadata_strip.cpp
    src/rename_template.cpp
    src/ui_extra.cpp
    src/workflow.cpp
    src/folder_tree.cpp
    src/lib_sort.cpp
    src/clipboard_image.cpp
    src/aesthetic_scorer.cpp
    src/video_util.cpp
    src/video_player.cpp
    src/orbit_view.cpp
    src/layout_ui.cpp
    src/generate_ui.cpp
    src/character_story.cpp
    src/lora_sort.cpp
    src/lora_browser.cpp
    src/apps_ui.cpp
    src/video_tools.cpp
    src/workflow_ui.cpp
    src/board.cpp
    src/board_ui.cpp
    src/svg_icons.cpp
    src/preset_art.cpp
    src/setup_ui.cpp
    src/comfy.cpp
    src/comfy_ui.cpp
    src/characters_ui.cpp
    src/board_recast.cpp
    src/edits_ui.cpp
    src/upscale_ui.cpp
    src/pixel_art.cpp
    src/pf_util.cpp
    src/pf_detect.cpp
    src/pf_full.cpp
    src/pixel_ui.cpp
    src/design_ui.cpp
    src/video_ui.cpp
    src/story_ui.cpp
    src/music_ui.cpp
    src/music_lyrics.cpp
    src/cover_templates.cpp
    src/drama_ui.cpp
    src/character_creator.cpp
    src/notify.cpp
    src/director.cpp
    src/clef.cpp
    src/clef_grades.cpp
    src/clef_tags.cpp
    src/relation_import.cpp
    src/embed_index.cpp
    src/clef_ui.cpp
    src/crash_log.cpp
    src/resources.rc

    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp
)

target_include_directories(Try2Create PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${CMAKE_SOURCE_DIR}/vendor/stb
    ${CMAKE_SOURCE_DIR}/vendor/json
    ${CMAKE_SOURCE_DIR}/vendor/onnxruntime/include
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(Try2Create PRIVATE
    d3d11 dxgi d3dcompiler dwmapi shlwapi comctl32 ole32 uuid shell32 windowscodecs winhttp dbghelp comdlg32 iphlpapi ws2_32
)

set_target_properties(Try2Create PROPERTIES
    OUTPUT_NAME "Try2Create"
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

if(MSVC)
    # The application manifest (DPI awareness, long paths, visual styles) is
    # embedded through resources.rc, so the linker must not generate its own.
    target_link_options(Try2Create PRIVATE /MANIFEST:NO /DEBUG /OPT:REF /OPT:ICF)
    target_compile_options(Try2Create PRIVATE /Zi) # a .pdb next to the program lets the crash logger name the function and line
endif()

# ONNX Runtime (DirectML build) for the NSFW classifier. Not linked: it's
# LoadLibrary'd at runtime so the app still runs without it, but the DLLs
# must sit next to the exe. The model itself lives in models/nsfw.onnx.
file(GLOB ORT_DLLS ${CMAKE_SOURCE_DIR}/vendor/onnxruntime/bin/*.dll)
add_custom_command(TARGET Try2Create POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ORT_DLLS} $<TARGET_FILE_DIR:Try2Create>
)
