enable_testing()
find_package(Qt6 REQUIRED COMPONENTS Test Widgets)

include_directories(${CMAKE_SOURCE_DIR})

# Legacy test: currently unbuildable (stale include path predating the gui/
# tree reorganization). Kept for reference; flip on to attempt revival.
option(QIMGV_LEGACY_TESTS "Build legacy MapOverlay test (currently broken)" OFF)
if(QIMGV_LEGACY_TESTS)
    add_executable(unit_tests test_mapoverlay.cpp)
    target_link_libraries(unit_tests PRIVATE Qt6::Test Qt6::Widgets)
    add_test(NAME QUI_TEST COMMAND unit_tests)
endif()

# Rust engine contract smoke test (pure C ABI, no Qt needed)
add_executable(engine_contract_tests test_engine_contract.cpp)
target_link_libraries(engine_contract_tests PRIVATE qimgv_engine_ffi)
target_include_directories(engine_contract_tests PRIVATE ${CMAKE_SOURCE_DIR}/engine/qimgv-engine-ffi/include)
add_test(NAME ENGINE_CONTRACT COMMAND engine_contract_tests)
