#![no_std]

use core::panic::PanicInfo;
use core::sync::atomic::{AtomicU64, Ordering};

#[panic_handler]
fn panic(_: &PanicInfo) -> ! {
    loop {}
}

#[repr(C)]
pub struct Pair {
    pub first: u64,
    pub second: u64,
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_sum_slice(data: *const u32, len: usize) -> u64 {
    if data.is_null() {
        return 0;
    }
    let slice = unsafe { core::slice::from_raw_parts(data, len) };
    slice.iter().map(|value| *value as u64).sum()
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_u128_div(high: u64, low: u64, divisor: u64) -> u64 {
    let value = ((high as u128) << 64) | (low as u128);
    if divisor == 0 {
        return 0;
    }
    (value / (divisor as u128)) as u64
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_struct_return(a: u64, b: u64) -> Pair {
    Pair {
        first: a.wrapping_add(b),
        second: a.wrapping_mul(b),
    }
}

static COUNTER: AtomicU64 = AtomicU64::new(0);

#[unsafe(no_mangle)]
pub extern "C" fn rust_atomic_add(amount: u64) -> u64 {
    COUNTER.fetch_add(amount, Ordering::SeqCst) + amount
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_option_and_iterator(limit: u32) -> u64 {
    let found = (0..limit).filter(|value| value % 3 == 0).max();
    match found {
        Some(value) => value as u64,
        None => u64::MAX,
    }
}
