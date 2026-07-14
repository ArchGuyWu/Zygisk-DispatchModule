use dispatch_core::WorldPos;

const MATRIX_POS_X: usize = 48;
const MATRIX_POS_Y: usize = 52;
const MATRIX_POS_Z: usize = 56;

pub const ENTITY_MODEL_OFFSET: usize = 0x3a;

const CVEHICLE_DRIVER: usize = 0x460;
const CVEHICLE_FIRE: usize = 0x490;
const CVEHICLE_HEALTH: usize = 0x4C0;
const CVEHICLE_BLOWN_UP_TIME: usize = 0x4D8;
const VEHICLE_BURNING_HEALTH: f32 = 250.0;

pub fn entity_pos(
    entity: *const std::ffi::c_void,
    get_matrix: unsafe extern "C" fn(*mut std::ffi::c_void) -> *mut std::ffi::c_void,
) -> WorldPos {
    if entity.is_null() {
        return WorldPos::default();
    }
    let matrix = unsafe { get_matrix(entity as *mut _) };
    if matrix.is_null() {
        return WorldPos::default();
    }
    unsafe {
        let base = matrix as *const u8;
        WorldPos {
            x: *(base.add(MATRIX_POS_X) as *const f32),
            y: *(base.add(MATRIX_POS_Y) as *const f32),
            z: *(base.add(MATRIX_POS_Z) as *const f32),
        }
    }
}

pub fn vehicle_driver(vehicle: *const std::ffi::c_void) -> *const std::ffi::c_void {
    if vehicle.is_null() {
        return std::ptr::null();
    }
    unsafe {
        let base = vehicle as *const u8;
        let slot = base.add(CVEHICLE_DRIVER) as *const *const std::ffi::c_void;
        if slot.is_null() {
            return std::ptr::null();
        }
        *slot
    }
}

pub fn vehicle_is_burning(vehicle: *const std::ffi::c_void) -> bool {
    if vehicle.is_null() {
        return false;
    }
    unsafe {
        let base = vehicle as *const u8;
        let fire = *(base.add(CVEHICLE_FIRE) as *const *const std::ffi::c_void);
        if !fire.is_null() {
            return true;
        }
        let blown = *(base.add(CVEHICLE_BLOWN_UP_TIME) as *const u32);
        if blown != 0 {
            return true;
        }
        let health = *(base.add(CVEHICLE_HEALTH) as *const f32);
        health <= VEHICLE_BURNING_HEALTH
    }
}

pub fn entity_model_index(entity: *const std::ffi::c_void) -> u16 {
    if entity.is_null() {
        return 0;
    }
    unsafe {
        let slot = (entity as *const u8).add(ENTITY_MODEL_OFFSET) as *const u16;
        *slot
    }
}

pub fn dist_sq(a: WorldPos, b: WorldPos) -> f32 {
    let dx = a.x - b.x;
    let dy = a.y - b.y;
    let dz = a.z - b.z;
    dx * dx + dy * dy + dz * dz
}