//! The sanitizer runs' arming test.
//!
//! A green run says nothing about an instrument that is not pointed at
//! anything — so the runs that are supposed to see races get one planted
//! deliberately. This test contains a textbook data race (two threads
//! incrementing one non-atomic counter); a correct sanitizer run MUST report
//! it at `COUNTER += 1`'s own line. It is `#[ignore]`d because its expected
//! result IS the report — a passing execution would mean the instrument is
//! dead, and the assert below is deliberately beside the point.
//!
//! Run explicitly:
//!   cargo +nightly miri test --test race_arm -- --ignored
//! (expected: "Data race detected" naming this file; nonzero exit IS the pass)
//!
//! History: the mixed-tree TSan run was armed the same way (deliberately
//! unsynchronised counter, reported at its own source line) before the tree
//! was demoted; see the QIMGV_SANITIZERS block in the root CMakeLists.txt.

#[test]
#[ignore = "its expected result IS a sanitizer report — run explicitly, see module docs"]
fn a_deliberately_unsynchronised_counter_is_reported() {
    static mut COUNTER: u64 = 0;
    let h1 = std::thread::spawn(|| {
        for _ in 0..1000 {
            unsafe { COUNTER += 1 };
        }
    });
    let h2 = std::thread::spawn(|| {
        for _ in 0..1000 {
            unsafe { COUNTER += 1 };
        }
    });
    h1.join().unwrap();
    h2.join().unwrap();
    // the value is beside the point; the race report is the deliverable
    assert!(unsafe { COUNTER } >= 1000);
}
