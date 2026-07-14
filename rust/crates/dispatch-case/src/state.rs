#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DispatchState {
    Idle,
    Timing,
    OnScene,
    Cleanup,
}