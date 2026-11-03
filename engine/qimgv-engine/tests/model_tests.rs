use std::time::{Duration, SystemTime};

use qimgv_engine::{key_from_ffi, key_to_ffi, DirectoryState, EntryData, SortMode};

fn entry(name: &str, size: u64, age_secs: u64) -> EntryData {
    EntryData {
        name: name.to_owned(),
        path: format!("/tmp/{name}"),
        is_dir: false,
        size,
        mtime: SystemTime::now() - Duration::from_secs(age_secs),
    }
}

#[test]
fn natural_name_order() {
    let mut st = DirectoryState::new(SortMode::Name);
    st.replace_all(
        "/tmp".into(),
        vec![entry("img10.jpg", 1, 0), entry("img2.jpg", 1, 0), entry("img1.jpg", 1, 0)],
        vec![],
    );
    let names: Vec<&str> = (0..3).map(|i| st.file_at(i).unwrap().data.name.as_str()).collect();
    assert_eq!(names, ["img1.jpg", "img2.jpg", "img10.jpg"]);
}

#[test]
fn name_desc_reverses() {
    let mut st = DirectoryState::new(SortMode::Name);
    st.replace_all("/tmp".into(), vec![entry("a.jpg", 1, 0), entry("b.jpg", 1, 0)], vec![]);
    st.resort(SortMode::NameDesc);
    let names: Vec<&str> = (0..2).map(|i| st.file_at(i).unwrap().data.name.as_str()).collect();
    assert_eq!(names, ["b.jpg", "a.jpg"]);
}

#[test]
fn size_and_time_sorts() {
    let mut st = DirectoryState::new(SortMode::Size);
    st.replace_all("/tmp".into(), vec![entry("big.jpg", 100, 0), entry("small.jpg", 1, 0)], vec![]);
    assert_eq!(st.file_at(0).unwrap().data.name, "small.jpg");
    st.resort(SortMode::SizeDesc);
    assert_eq!(st.file_at(0).unwrap().data.name, "big.jpg");

    st.resort(SortMode::Time);
    // entry() ages newer files less; Time ascending = oldest first
    let mut st2 = DirectoryState::new(SortMode::Time);
    st2.replace_all("/tmp".into(), vec![entry("new.jpg", 1, 10), entry("old.jpg", 1, 100)], vec![]);
    assert_eq!(st2.file_at(0).unwrap().data.name, "old.jpg");
}

#[test]
fn next_prev_and_index() {
    let mut st = DirectoryState::new(SortMode::Name);
    st.replace_all(
        "/tmp".into(),
        vec![entry("a.jpg", 1, 0), entry("b.jpg", 1, 0), entry("c.jpg", 1, 0)],
        vec![],
    );
    let b = st.file_at(1).unwrap().handle;
    assert_eq!(st.index_of_file(b), Some(1));
    assert_eq!(st.next_file(b), Some(st.file_at(2).unwrap().handle));
    assert_eq!(st.prev_file(b), Some(st.file_at(0).unwrap().handle));

    let first = st.file_at(0).unwrap().handle;
    assert_eq!(st.prev_file(first), None);
}

#[test]
fn handles_survive_resort_die_on_replace() {
    let mut st = DirectoryState::new(SortMode::Name);
    st.replace_all("/tmp".into(), vec![entry("a.jpg", 1, 0), entry("b.jpg", 1, 0)], vec![]);
    let b = st.file_at(1).unwrap().handle;

    st.resort(SortMode::NameDesc);
    assert_eq!(st.index_of_file(b), Some(0), "handle follows its entry across re-sort");

    st.replace_all("/tmp2".into(), vec![entry("x.jpg", 1, 0)], vec![]);
    assert_eq!(st.index_of_file(b), None);
    assert_eq!(st.next_file(b), None);
}

#[test]
fn ffi_key_roundtrip_and_null_rejection() {
    let mut st = DirectoryState::new(SortMode::Name);
    st.replace_all("/tmp".into(), vec![entry("a.jpg", 1, 0)], vec![]);
    let key = st.file_at(0).unwrap().handle;
    let bits = key_to_ffi(key);
    assert_eq!(key_from_ffi(bits), Some(key));
    assert_eq!(key_from_ffi(0), None, "all-zero bits are the null key");
}

#[test]
fn insert_entry_reports_the_position_it_chose() {
    let mut st = DirectoryState::new(SortMode::Name);
    st.replace_all(
        "/tmp".into(),
        vec![entry("a.jpg", 1, 0), entry("c.jpg", 1, 0)],
        vec![],
    );
    let (_, pos) = st.insert_entry(entry("b.jpg", 1, 0)).unwrap();
    assert_eq!(pos, 1);
    let names: Vec<&str> = (0..3).map(|i| st.file_at(i).unwrap().data.name.as_str()).collect();
    assert_eq!(names, ["a.jpg", "b.jpg", "c.jpg"]);
}

#[test]
fn meta_update_repositions_under_size_sort() {
    // without the re-seating, a size change under SIZE sort leaves the order
    // vectors unsorted — the export itself stops agreeing with the comparator
    let mut st = DirectoryState::new(SortMode::Size);
    st.replace_all(
        "/tmp".into(),
        vec![entry("big.jpg", 100, 0), entry("small.jpg", 1, 0)],
        vec![],
    );
    let mtime = SystemTime::now();
    let pos = st.update_entry_meta("/tmp/small.jpg", 500, mtime).unwrap();
    assert_eq!(pos, 1);
    assert_eq!(st.file_at(0).unwrap().data.name, "big.jpg");
    assert_eq!(st.file_at(1).unwrap().data.name, "small.jpg");
}

#[test]
fn meta_update_keeps_the_position_under_name_sort() {
    let mut st = DirectoryState::new(SortMode::Name);
    st.replace_all("/tmp".into(), vec![entry("a.jpg", 1, 0)], vec![]);
    let pos = st.update_entry_meta("/tmp/a.jpg", 9, SystemTime::now()).unwrap();
    assert_eq!(pos, 0);
}
