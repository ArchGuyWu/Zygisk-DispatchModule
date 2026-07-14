#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DespawnReason {
    Killed,
    PoolRecycle,
}