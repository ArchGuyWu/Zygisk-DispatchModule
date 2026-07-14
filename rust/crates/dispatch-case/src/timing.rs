pub const DEFAULT_DISPATCH_DELAY_MS: i32 = 8_000;
/// High-priority report: shortens only post-report-end → dispatch wait.
pub const HIGH_PRIORITY_DISPATCH_DELAY_MS: i32 = 3_000;
/// Report interrupted mid-call: dispatch quickly on partial info.
pub const INTERRUPTED_REPORT_DISPATCH_DELAY_MS: i32 = 1_000;
pub const NPC_ON_SCENE_TIMEOUT_MS: i64 = 300_000;
/// All departments: idle on scene when no abnormality found, then leave.
pub const ON_SCENE_IDLE_WAIT_MS: i64 = 90_000;
pub const SPAWN_ARRIVED_SCENE_TIMEOUT_MS: i64 = 180_000;