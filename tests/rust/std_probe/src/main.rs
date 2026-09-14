mod abi_facts;

use std::collections::{BTreeMap, HashMap};
use std::ffi::CStr;
use std::fs;
use std::io::{Read, Seek, SeekFrom, Write};
use std::os::raw::{c_char, c_ulong};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

#[repr(C)]
struct AbiFact {
    name: *const c_char,
    value: c_ulong,
}

unsafe extern "C" {
    fn leanos_abi_fact_count() -> c_ulong;
    fn leanos_abi_facts() -> *const AbiFact;
}

fn c_side() -> BTreeMap<String, u64> {
    let mut out = BTreeMap::new();
    unsafe {
        let count = leanos_abi_fact_count() as usize;
        let base = leanos_abi_facts();
        for index in 0..count {
            let fact = &*base.add(index);
            let name = CStr::from_ptr(fact.name).to_string_lossy().into_owned();
            out.insert(name, fact.value as u64);
        }
    }
    out
}

fn abi(failures: &mut usize) {
    let theirs = c_side();
    let mut disagreements = Vec::new();
    for (name, ours) in abi_facts::FACTS {
        match theirs.get(*name) {
            None => disagreements.push(format!("{name}: the C side never named it")),
            Some(value) if value != ours => {
                disagreements.push(format!("{name}: C says {value}, Rust says {ours}"))
            }
            Some(_) => {}
        }
    }
    if theirs.len() != abi_facts::FACTS.len() {
        disagreements.push(format!(
            "the two tables are different sizes: C {}, Rust {}",
            theirs.len(),
            abi_facts::FACTS.len()
        ));
    }
    if disagreements.is_empty() {
        println!(
            "ruststd: {} ABI facts agree between this libc's headers and the Rust libc crate",
            abi_facts::FACTS.len()
        );
    } else {
        for line in &disagreements {
            println!("ruststd: FAIL {line}");
        }
        *failures += 1;
    }
}

fn files(failures: &mut usize) {
    let directory = "/tmp/m138";
    let _ = fs::remove_dir_all(directory);
    if let Err(error) = fs::create_dir_all(directory) {
        println!("ruststd: FAIL create_dir_all {error}");
        *failures += 1;
        return;
    }
    let path = format!("{directory}/written-by-rust");
    let body = "std::fs reached leanfs\n".repeat(64);
    if let Err(error) = fs::write(&path, &body) {
        println!("ruststd: FAIL write {error}");
        *failures += 1;
        return;
    }
    match fs::read_to_string(&path) {
        Ok(back) if back == body => {}
        Ok(back) => {
            println!("ruststd: FAIL read back {} bytes of {}", back.len(), body.len());
            *failures += 1;
            return;
        }
        Err(error) => {
            println!("ruststd: FAIL read {error}");
            *failures += 1;
            return;
        }
    }

    let metadata = match fs::metadata(&path) {
        Ok(metadata) => metadata,
        Err(error) => {
            println!("ruststd: FAIL metadata {error}");
            *failures += 1;
            return;
        }
    };
    if metadata.len() as usize != body.len() || !metadata.is_file() {
        println!("ruststd: FAIL metadata says {} bytes", metadata.len());
        *failures += 1;
        return;
    }

    let modified = match metadata.modified() {
        Ok(modified) => modified,
        Err(error) => {
            println!("ruststd: FAIL modified {error}");
            *failures += 1;
            return;
        }
    };
    let now = SystemTime::now();
    let skew = match now.duration_since(modified) {
        Ok(skew) => skew,
        Err(error) => error.duration(),
    };
    if skew > Duration::from_secs(600) {
        println!("ruststd: FAIL the file's mtime is {} s from now", skew.as_secs());
        *failures += 1;
        return;
    }

    let mut entries: Vec<String> = match fs::read_dir(directory) {
        Ok(iterator) => iterator
            .filter_map(|entry| entry.ok())
            .map(|entry| entry.file_name().to_string_lossy().into_owned())
            .collect(),
        Err(error) => {
            println!("ruststd: FAIL read_dir {error}");
            *failures += 1;
            return;
        }
    };
    entries.sort();
    if entries != ["written-by-rust"] {
        println!("ruststd: FAIL read_dir saw {entries:?}");
        *failures += 1;
        return;
    }

    let mut file = match fs::File::open(&path) {
        Ok(file) => file,
        Err(error) => {
            println!("ruststd: FAIL open {error}");
            *failures += 1;
            return;
        }
    };
    let mut clone = match file.try_clone() {
        Ok(clone) => clone,
        Err(error) => {
            println!("ruststd: FAIL try_clone {error}");
            *failures += 1;
            return;
        }
    };
    let mut head = [0u8; 10];
    if file.read_exact(&mut head).is_err() {
        println!("ruststd: FAIL read_exact");
        *failures += 1;
        return;
    }
    let position = clone.seek(SeekFrom::Current(0)).unwrap_or(0);
    if position != 10 {
        println!("ruststd: FAIL the cloned descriptor is at {position}, not 10");
        *failures += 1;
        return;
    }

    let _ = fs::remove_dir_all(directory);
    println!("ruststd: std::fs wrote, read, walked and stat'd a file on leanfs");
    println!("ruststd: try_clone gave a descriptor sharing one file position");
}

fn threads(failures: &mut usize) {
    let counter = Arc::new(AtomicU64::new(0));
    let mut handles = Vec::new();
    for index in 0..4u64 {
        let counter = Arc::clone(&counter);
        handles.push(thread::spawn(move || {
            for step in 0..1000u64 {
                counter.fetch_add(index * step % 7, Ordering::SeqCst);
            }
            index * index
        }));
    }
    let mut squares = 0u64;
    for handle in handles {
        match handle.join() {
            Ok(value) => squares += value,
            Err(_) => {
                println!("ruststd: FAIL a thread panicked");
                *failures += 1;
                return;
            }
        }
    }
    if squares != 14 {
        println!("ruststd: FAIL the joined threads returned {squares}");
        *failures += 1;
        return;
    }
    println!("ruststd: four std::thread threads ran and joined with their values");

    let gate = Arc::new((Mutex::new(0usize), Condvar::new()));
    let worker = {
        let gate = Arc::clone(&gate);
        thread::spawn(move || {
            let (lock, signal) = &*gate;
            let mut count = lock.lock().unwrap();
            *count += 41;
            signal.notify_one();
        })
    };
    {
        let (lock, signal) = &*gate;
        let mut count = lock.lock().unwrap();
        while *count == 0 {
            let (next, timeout) = signal.wait_timeout(count, Duration::from_secs(10)).unwrap();
            count = next;
            if timeout.timed_out() {
                println!("ruststd: FAIL the condvar timed out");
                *failures += 1;
                return;
            }
        }
        if *count != 41 {
            println!("ruststd: FAIL the condvar woke with {}", *count);
            *failures += 1;
            return;
        }
    }
    let _ = worker.join();
    println!("ruststd: a Mutex and a Condvar over this libc's pthreads");
}

fn collections(failures: &mut usize) {
    let mut map = HashMap::new();
    for index in 0..512u32 {
        map.insert(format!("key-{index}"), index * index);
    }
    if map.get("key-19") != Some(&361) || map.len() != 512 {
        println!("ruststd: FAIL HashMap");
        *failures += 1;
        return;
    }
    let mut sorted: Vec<&String> = map.keys().collect();
    sorted.sort();
    if sorted.first().map(|key| key.as_str()) != Some("key-0") {
        println!("ruststd: FAIL sorting the keys");
        *failures += 1;
        return;
    }
    let aligned = Box::new(OverAligned([7u8; 96]));
    if aligned.0[0] != 7 || (&*aligned as *const OverAligned).addr() % 64 != 0 {
        println!("ruststd: FAIL a 64-byte-aligned Box is not 64-byte aligned");
        *failures += 1;
        return;
    }
    println!("ruststd: a 512-entry HashMap seeded from getrandom, and an over-aligned Box");
}

#[repr(C, align(64))]
struct OverAligned([u8; 96]);

fn clocks_and_environment(failures: &mut usize) {
    let start = Instant::now();
    thread::sleep(Duration::from_millis(120));
    let slept = start.elapsed();
    if slept < Duration::from_millis(100) || slept > Duration::from_secs(5) {
        println!("ruststd: FAIL sleep(120ms) measured {} ms", slept.as_millis());
        *failures += 1;
        return;
    }
    let wall = match SystemTime::now().duration_since(UNIX_EPOCH) {
        Ok(wall) => wall,
        Err(error) => {
            println!("ruststd: FAIL the wall clock is before the epoch: {error}");
            *failures += 1;
            return;
        }
    };
    if wall.as_secs() < 1_600_000_000 {
        println!("ruststd: FAIL the wall clock reads {}", wall.as_secs());
        *failures += 1;
        return;
    }
    println!("ruststd: Instant and SystemTime over clock_gettime");

    let arguments: Vec<String> = std::env::args().collect();
    if arguments.is_empty() {
        println!("ruststd: FAIL std::env::args is empty");
        *failures += 1;
        return;
    }
    unsafe { std::env::set_var("M138", "rust std") };
    if std::env::var("M138").as_deref() != Ok("rust std") {
        println!("ruststd: FAIL the environment did not keep what was set");
        *failures += 1;
        return;
    }
    let cwd = std::env::current_dir();
    if cwd.is_err() {
        println!("ruststd: FAIL current_dir {cwd:?}");
        *failures += 1;
        return;
    }
    println!(
        "ruststd: argv[0] is {}, the environment answers, and the cwd is {}",
        arguments[0],
        cwd.unwrap().display()
    );
}

fn main() {
    let mut failures = 0usize;

    abi(&mut failures);
    files(&mut failures);
    threads(&mut failures);
    collections(&mut failures);
    clocks_and_environment(&mut failures);

    let mut out = std::io::stdout();
    if failures == 0 {
        let _ = writeln!(out, "ruststd: done");
    }
    let _ = out.flush();
    std::process::exit(if failures == 0 { 0 } else { 1 });
}
