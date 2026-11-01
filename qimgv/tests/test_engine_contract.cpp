// Contract smoke test for the Rust engine, run from the C++ consumption side.
//
// Ownership rule (agreed): this file owns what only a C++ consumer can verify —
// that the header matches the Rust #[repr(C)] structs (compile-time below),
// that every exported symbol links, and the basic async lifecycle works with
// plain C strings. Business semantics are asserted in Rust:
// engine/qimgv-engine-ffi/tests/ffi_contract.rs. One behavior, one owner.

#include "qimgv_engine.h"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

// Layout pinning — must match #[repr(C)] QimgvEntryMeta (u64,u64,i64,c_int).
static_assert(sizeof(qimgv_entry_meta) == 32, "qimgv_entry_meta layout drift");
static_assert(offsetof(qimgv_entry_meta, handle) == 0, "layout drift");
static_assert(offsetof(qimgv_entry_meta, size) == 8, "layout drift");
static_assert(offsetof(qimgv_entry_meta, mtime_ms) == 16, "layout drift");
static_assert(offsetof(qimgv_entry_meta, is_dir) == 24, "layout drift");

static int failures = 0;
#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++failures;                                                    \
        }                                                                  \
    } while (0)

static std::string makeFixture() {
    namespace fs = std::filesystem;
    auto dir = fs::temp_directory_path() / "qimgv_engine_contract_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::ofstream(dir / "a.jpg") << "x";
    std::ofstream(dir / "b.jpg") << "x";
    std::ofstream(dir / "skip.txt") << "x";
    return dir.string();
}

int main() {
    CHECK(qimgv_engine_version() != nullptr);
    CHECK(std::strlen(qimgv_engine_version()) > 0);

    qimgv_engine *engine = qimgv_engine_new(nullptr, nullptr, 1, "jpg");
    CHECK(engine != nullptr);
    if (!engine)
        return 1;

    // fresh state
    CHECK(qimgv_engine_file_count(engine) == 0);
    CHECK(qimgv_engine_dir_count(engine) == 0);
    qimgv_event ev{};
    char dirbuf[512];
    CHECK(qimgv_engine_take_event(engine, &ev, dirbuf, sizeof(dirbuf)) == 0);

    // error paths leave a readable diagnostic
    CHECK(qimgv_engine_set_sort(engine, 99) == QIMGV_ERR_BAD_SORT);
    CHECK(qimgv_last_error() != nullptr);
    CHECK(qimgv_engine_open_dir(engine, nullptr) == QIMGV_ERR_INVALID);

    // async open on a real directory
    std::string fixture = makeFixture();
    CHECK(qimgv_engine_open_dir(engine, fixture.c_str()) == QIMGV_OK);

    int got = 0;
    for (int i = 0; i < 1000; ++i) {
        got = qimgv_engine_take_event(engine, &ev, dirbuf, sizeof(dirbuf));
        if (got != 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(got == 1);
    CHECK(ev.kind == QIMGV_EVENT_DIR_LISTED);
    CHECK(ev.code == QIMGV_OK);
    CHECK(std::strcmp(dirbuf, fixture.c_str()) == 0);

    CHECK(qimgv_engine_file_count(engine) == 2);
    qimgv_entry_meta meta{};
    char name[64], path[512];
    CHECK(qimgv_engine_file_entry_at(engine, 0, &meta, name, sizeof(name), path, sizeof(path)) == QIMGV_OK);
    CHECK(std::strcmp(name, "a.jpg") == 0);
    uint64_t h0 = meta.handle;
    CHECK(qimgv_engine_file_entry_at(engine, 1, &meta, name, sizeof(name), path, sizeof(path)) == QIMGV_OK);
    uint64_t h1 = meta.handle;
    uint64_t next = 0;
    CHECK(qimgv_engine_next_file(engine, h0, &next) == QIMGV_OK);
    CHECK(next == h1);

    // undersized name buffer ("a.jpg\0" needs 6 bytes, 1 given)
    CHECK(qimgv_engine_file_entry_at(engine, 0, &meta, name, 1, path, sizeof(path)) == QIMGV_ERR_TRUNCATED);

    qimgv_engine_free(engine);

    std::printf(failures == 0 ? "engine contract: OK\n" : "engine contract: %d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
