add_executable(gpur_core_tests
    test_result.cpp
    test_d3d_context.cpp
)

target_link_libraries(gpur_core_tests
    PRIVATE
        gpur_core
        Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(gpur_core_tests)
