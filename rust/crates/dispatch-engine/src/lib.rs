mod entity;
mod event_vtables;
mod ffi_types;
mod lifecycle;
mod pool;
mod probe;
mod symbols;
mod xdl;

pub use entity::{
    dist_sq, entity_model_index, entity_pos, vehicle_driver, vehicle_is_burning,
    ENTITY_MODEL_OFFSET,
};
pub use event_vtables::{event_vptr_identity, NativeEventRegistry, ResolvedNativeEvent};
pub use ffi_types::CVector;
pub use pool::{
    entity_from_pool_key, entity_pool_key, open_entity_pool, pool_key_live, EntityPoolView,
};
pub use lifecycle::{LivePed, PoolPed, PoolVehicle};
pub use probe::{probe_globals_changed, probe_loader_inputs};
pub use symbols::{GameSymbols, SymbolError, TARGET_LIB};
pub use xdl::Library;