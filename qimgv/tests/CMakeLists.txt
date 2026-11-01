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

# Engine stack integration: EngineBridge pump + DirectoryManager async wiring.
# Sources are listed explicitly (deliberately: keeps the test from dragging in
# the whole app until a proper object-library split exists).
add_executable(engine_stack_tests test_engine_stack.cpp
    ../components/directorymanager/directorymanager.cpp
    ../components/directorymanager/enginebridge.cpp
    ../components/directorymanager/watchers/directorywatcher.cpp
    ../components/directorymanager/watchers/dummywatcher.cpp
    ../components/directorymanager/watchers/watcherevent.cpp
    ../components/directorymanager/watchers/watcherworker.cpp
    ../sourcecontainers/fsentry.cpp
    ../settings.cpp
    ../themestore.cpp
    ../utils/stuff.cpp
    ../utils/script.cpp
)
if(UNIX AND NOT APPLE)
    target_sources(engine_stack_tests PRIVATE
        ../components/directorymanager/watchers/linux/linuxwatcher.cpp
        ../components/directorymanager/watchers/linux/linuxworker.cpp
        ../components/directorymanager/watchers/linux/linuxfsevent.cpp)
elseif(WIN32)
    target_sources(engine_stack_tests PRIVATE
        ../components/directorymanager/watchers/windows/windowswatcher.cpp
        ../components/directorymanager/watchers/windows/windowsworker.cpp)
endif()
target_include_directories(engine_stack_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/qimgv
    ${CMAKE_SOURCE_DIR}/engine/qimgv-engine-ffi/include)
target_link_libraries(engine_stack_tests PRIVATE Qt6::Test Qt6::Gui Qt6::Widgets qimgv_engine_ffi)
add_test(NAME ENGINE_STACK COMMAND engine_stack_tests)
