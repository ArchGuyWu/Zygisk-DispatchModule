//! Thread-safe storage for hook original function pointers (replaces `static mut Option<fn>`).

use std::marker::PhantomData;
use std::sync::atomic::{AtomicUsize, Ordering};

/// Stores a function pointer in an `AtomicUsize` (ARM64/x64: fn ptr fits in usize).
pub struct OrigSlot<F> {
    bits: AtomicUsize,
    _ty: PhantomData<F>,
}

impl<F: Copy> OrigSlot<F> {
    pub const fn new() -> Self {
        Self {
            bits: AtomicUsize::new(0),
            _ty: PhantomData,
        }
    }

    #[inline]
    pub fn set(&self, f: F) {
        // SAFETY: F is a function pointer type of pointer size on this target.
        let bits = unsafe { std::mem::transmute_copy::<F, usize>(&f) };
        self.bits.store(bits, Ordering::Release);
    }

    #[inline]
    pub fn get(&self) -> Option<F> {
        let bits = self.bits.load(Ordering::Acquire);
        if bits == 0 {
            return None;
        }
        // SAFETY: bits were stored from a valid F via set().
        Some(unsafe { std::mem::transmute_copy::<usize, F>(&bits) })
    }
}

// SAFETY: OrigSlot only stores plain function pointers; AtomicUsize syncs publication.
unsafe impl<F: Copy> Sync for OrigSlot<F> {}
unsafe impl<F: Copy> Send for OrigSlot<F> {}
