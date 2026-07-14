#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct CVector {
    pub x: f32,
    pub y: f32,
    pub z: f32,
}

impl CVector {
    pub fn as_array(self) -> [f32; 3] {
        [self.x, self.y, self.z]
    }
}