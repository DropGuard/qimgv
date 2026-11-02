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
#include <vector>

// Layout pinning — must match #[repr(C)] QimgvEntryMeta (u64,u64,i64,c_int).
static_assert(sizeof(qimgv_entry_meta) == 32, "qimgv_entry_meta layout drift");
static_assert(offsetof(qimgv_entry_meta, handle) == 0, "layout drift");
static_assert(offsetof(qimgv_entry_meta, size) == 8, "layout drift");
static_assert(offsetof(qimgv_entry_meta, mtime_ms) == 16, "layout drift");
static_assert(offsetof(qimgv_entry_meta, is_dir) == 24, "layout drift");

// Layout pinning — must match #[repr(C)] QimgvEvent (i32,i32,u32).
static_assert(sizeof(qimgv_event) == 12, "qimgv_event layout drift");
static_assert(offsetof(qimgv_event, epoch) == 8, "layout drift");

// Layout pinning — must match #[repr(C)] QimgvProbeResult (5 × int32).
static_assert(sizeof(qimgv_probe_result) == 20, "qimgv_probe_result layout drift");
static_assert(offsetof(qimgv_probe_result, format) == 0, "layout drift");
static_assert(offsetof(qimgv_probe_result, animated) == 4, "layout drift");
static_assert(offsetof(qimgv_probe_result, orientation) == 8, "layout drift");
static_assert(offsetof(qimgv_probe_result, width) == 12, "layout drift");
static_assert(offsetof(qimgv_probe_result, height) == 16, "layout drift");

// Layout pinning — must match #[repr(C)] QimgvMutation (i32,i32,u64,i64).
static_assert(sizeof(qimgv_mutation) == 24, "qimgv_mutation layout drift");
static_assert(offsetof(qimgv_mutation, kind) == 0, "layout drift");
static_assert(offsetof(qimgv_mutation, is_dir) == 4, "layout drift");
static_assert(offsetof(qimgv_mutation, size) == 8, "layout drift");
static_assert(offsetof(qimgv_mutation, mtime_ms) == 16, "layout drift");

// Layout pinning — must match #[repr(C)] QimgvThumbReq (u32,i32).
static_assert(sizeof(qimgv_thumb_req) == 8, "qimgv_thumb_req layout drift");
static_assert(offsetof(qimgv_thumb_req, size) == 0, "layout drift");
static_assert(offsetof(qimgv_thumb_req, crop) == 4, "layout drift");

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
    fs::create_directory(dir / "subdir");
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
    qimgv_mutation mut{};
    char dirbuf[512];
    CHECK(qimgv_engine_take_event(engine, &ev, dirbuf, sizeof(dirbuf), &mut, nullptr) == 0);

    // error paths leave a readable diagnostic
    CHECK(qimgv_engine_set_sort(engine, 99) == QIMGV_ERR_BAD_SORT);
    CHECK(qimgv_last_error() != nullptr);
    CHECK(qimgv_engine_open_dir(engine, nullptr, 0, nullptr) == QIMGV_ERR_INVALID);

    // async open on a real directory
    std::string fixture = makeFixture();
    uint32_t openEpoch = 0;
    CHECK(qimgv_engine_open_dir(engine, fixture.c_str(), 0, &openEpoch) == QIMGV_OK);
    CHECK(openEpoch > 0);

    int got = 0;
    for (int i = 0; i < 1000; ++i) {
        got = qimgv_engine_take_event(engine, &ev, dirbuf, sizeof(dirbuf), &mut, nullptr);
        if (got != 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(got == 1);
    CHECK(ev.kind == QIMGV_EVENT_DIR_LISTED);
    CHECK(ev.code == QIMGV_OK);
    CHECK(ev.epoch == openEpoch);
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

    // --- remaining exported symbols ---------------------------------------
    // A symbol nothing calls here drops out of the link check entirely, which
    // is the one thing this file exists to verify. The semantics behind each
    // are owned by engine/qimgv-engine-ffi/tests/ffi_contract.rs.
    CHECK(qimgv_engine_dir_count(engine) == 1);
    qimgv_entry_meta dmeta{};
    CHECK(qimgv_engine_dir_entry_at(engine, 0, &dmeta, name, sizeof(name), path, sizeof(path)) == QIMGV_OK);
    CHECK(std::strcmp(name, "subdir") == 0);
    CHECK(qimgv_engine_next_dir(engine, dmeta.handle, &next) == QIMGV_ERR_EMPTY);
    CHECK(qimgv_engine_prev_dir(engine, dmeta.handle, &next) == QIMGV_ERR_EMPTY);

    CHECK(qimgv_engine_set_show_hidden(engine, 1) == QIMGV_OK);
    CHECK(qimgv_engine_set_show_hidden(engine, 0) == QIMGV_OK);
    CHECK(qimgv_engine_include_path(engine, (fixture + "/subdir").c_str()) == QIMGV_OK);
    CHECK(qimgv_engine_stop_watch(engine) == QIMGV_OK);

    CHECK(qimgv_engine_thumb_config(engine, 2) == QIMGV_OK);
    CHECK(qimgv_engine_thumb_request(engine, (fixture + "/a.jpg").c_str(), 128, 1, 1) == QIMGV_OK);
    CHECK(qimgv_engine_thumb_done(engine, (fixture + "/a.jpg").c_str(), 128, 1) == QIMGV_OK);
    CHECK(qimgv_engine_thumb_clear(engine) == QIMGV_OK);

    // bulk export: the cap=0 probe must report the size, then fit exactly
    uint32_t needed = 0, fc = 0, dc = 0;
    CHECK(qimgv_engine_export_listing(engine, nullptr, 0, &needed, &fc, &dc) == QIMGV_ERR_TRUNCATED);
    CHECK(needed > 8);
    std::vector<unsigned char> arena(needed);
    CHECK(qimgv_engine_export_listing(engine, arena.data(), arena.size(), &needed, &fc, &dc) == QIMGV_OK);
    CHECK(fc == 2);
    CHECK(dc == 1);

    qimgv_engine_free(engine);

    // --- probe: pure function, no engine instance needed -------------------
    qimgv_probe_result pr{};
    // CRC-free PNG is fine — the header parsers don't validate chunk CRCs
    auto chunk = [](const char tag[4], const unsigned char *data, uint32_t len) {
        std::string c;
        for (int i = 3; i >= 0; --i) c.push_back(char((len >> (8 * i)) & 0xff));
        c.append(tag, 4);
        c.append(reinterpret_cast<const char *>(data), len);
        return c;
    };
    unsigned char ihdr[13] = {0, 0, 0, 40, 0, 0, 0, 20, 8, 2, 0, 0, 0}; // 40x20 RGB
    std::string png = "\x89PNG\r\n\x1a\n";
    png += chunk("IHDR", ihdr, 13);
    png += chunk("IDAT", reinterpret_cast<const unsigned char *>(""), 0);
    png += chunk("IEND", reinterpret_cast<const unsigned char *>(""), 0);
    {
        std::filesystem::path pp = std::filesystem::temp_directory_path() / "qimgv_contract_probe.png";
        std::ofstream(pp, std::ios::binary).write(png.data(), png.size());
        CHECK(qimgv_engine_probe(pp.string().c_str(), &pr) == QIMGV_OK);
        CHECK(pr.format == QIMGV_FMT_PNG);
        CHECK(pr.width == 40 && pr.height == 20);
        CHECK(pr.animated == 0);
        CHECK(pr.orientation >= 1 && pr.orientation <= 8);
        CHECK(qimgv_engine_probe(nullptr, &pr) == QIMGV_ERR_INVALID);
    }

    // watcher support query must answer consistently with mutation events
    CHECK(qimgv_engine_supports_watching() == 1);

    std::printf(failures == 0 ? "engine contract: OK\n" : "engine contract: %d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
