find_package(Qt6 REQUIRED COMPONENTS Test Widgets)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

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

# Engine stack integration: EngineBridge pump + DirectoryManager async wiring.
# Links qimgv_nongui — the same objects the executable is built from — so a test
# that touches another component costs nothing to add.
# One class per domain, one binary. 49 slots in a single 1371-line TU meant every
# edit recompiled the whole suite, and the fifteen "layer" comments in it were
# file boundaries that never became files.
add_executable(engine_stack_tests
    engine_stack_main.cpp
    listing_tests.cpp
    wire_format_tests.cpp
    folder_boundary_tests.cpp
    decode_tests.cpp
    settings_tests.cpp
    filelog_tests.cpp
    machinery_tests.cpp
    workerpool_tests.cpp
    image_ops_tests.cpp
    infra_tests.cpp)
target_link_libraries(engine_stack_tests PRIVATE Qt6::Test qimgv_nongui)
add_test(NAME ENGINE_STACK COMMAND engine_stack_tests)

# Core-level navigation: links the widget tree too, so a real Core can be built
# and driven under the offscreen platform. OBJECT libraries do not re-export each
# other's objects, so both are listed — the same pair the executable links.
add_executable(core_nav_tests core_nav_tests.cpp)
target_link_libraries(core_nav_tests PRIVATE Qt6::Test qimgv_gui qimgv_nongui)
add_test(NAME CORE_NAV COMMAND core_nav_tests)

# executable architecture rules (ArchUnit-style): gui isolation, FFI speaker
# allowlist, rust internal seal
add_test(NAME ARCH_RULES
         COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/qimgv/tests/architecture_rules.py)
set_tests_properties(ARCH_RULES PROPERTIES ENVIRONMENT "PYTHONPATH=")
