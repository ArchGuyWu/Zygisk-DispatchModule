use dispatch_core::PoolKey;

pub struct EntityPoolView<'a> {
    pub byte_map: &'a [i8],
    pub size: i32,
}

pub fn open_entity_pool(
    ms_p_pool: *mut *mut std::ffi::c_void,
) -> Option<EntityPoolView<'static>> {
    if ms_p_pool.is_null() {
        return None;
    }
    let pool = unsafe { *ms_p_pool };
    if pool.is_null() {
        return None;
    }
    let base = pool as *const u8;
    let byte_map_ptr = unsafe { *(base.add(8) as *const *const i8) };
    let size = unsafe { *(base.add(16) as *const i32) };
    if byte_map_ptr.is_null() || size <= 0 {
        return None;
    }
    let byte_map = unsafe { std::slice::from_raw_parts(byte_map_ptr, size as usize) };
    Some(EntityPoolView { byte_map, size })
}

/// O(1) slot/generation check — no pointer reverse lookup.
pub fn pool_key_live(pool: &EntityPoolView<'_>, key: PoolKey) -> bool {
    let slot = key.slot as usize;
    if slot >= pool.size as usize {
        return false;
    }
    let flag = pool.byte_map[slot];
    flag >= 0 && flag as u8 == key.generation
}

pub fn entity_pool_key(
    pool: &EntityPoolView<'_>,
    get_entity: unsafe extern "C" fn(i32) -> *mut std::ffi::c_void,
    entity: *const std::ffi::c_void,
) -> Option<PoolKey> {
    for slot in 0..pool.size as usize {
        let flag = pool.byte_map[slot];
        if flag < 0 {
            continue;
        }
        let handle = PoolKey::from_slot_flag(slot as u16, flag as u8).handle();
        let candidate = unsafe { get_entity(handle) };
        if candidate == entity as *mut _ {
            return Some(PoolKey::from_slot_flag(slot as u16, flag as u8));
        }
    }
    None
}

pub fn entity_from_pool_key(
    pool: &EntityPoolView<'_>,
    get_entity: unsafe extern "C" fn(i32) -> *mut std::ffi::c_void,
    key: PoolKey,
) -> Option<*mut std::ffi::c_void> {
    if !pool_key_live(pool, key) {
        return None;
    }
    let ptr = unsafe { get_entity(key.handle()) };
    if ptr.is_null() {
        return None;
    }
    Some(ptr)
}