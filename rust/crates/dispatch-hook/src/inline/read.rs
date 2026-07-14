use super::relocate::PATCH_BYTES;

pub fn read_target_bytes(target: usize) -> Option<[u8; PATCH_BYTES]> {
    if target == 0 {
        return None;
    }
    let mut buf = [0u8; PATCH_BYTES];
    // Live mapping only — must match what the CPU executes (ShadowHook memcpy parity).
    unsafe {
        std::ptr::copy_nonoverlapping(target as *const u8, buf.as_mut_ptr(), PATCH_BYTES);
    }
    Some(buf)
}