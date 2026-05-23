add_library(gpur_shared STATIC
    log.cpp
)

target_include_directories(gpur_shared
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(gpur_shared
    PUBLIC
        spdlog::spdlog
)

target_compile_features(gpur_shared PUBLIC cxx_std_23)
