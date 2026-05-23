add_executable(gpur-daemon
    main.cpp
    ipc_server.cpp
    hotkey.cpp
    overlay/layered_window.cpp
    overlay/d3d11_hook.cpp
    overlay/imgui_overlay.cpp
)

target_link_libraries(gpur-daemon
    PRIVATE
        gpur_core
        gpur_shared
)

target_include_directories(gpur-daemon
    PRIVATE
        ${CMAKE_SOURCE_DIR}
)

set_target_properties(gpur-daemon PROPERTIES
    WIN32_EXECUTABLE OFF      # console subsystem for Phase 0 CLI
)
