//! Signal vocabulary (MODEL.md §3).

/// Engine-agnostic position; model never holds raw pointers.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct WorldPos {
    pub x: f32,
    pub y: f32,
    pub z: f32,
}

/// Clue kinds that feed department needs and threat.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum SignalKind {
    Casualty,
    Injury,
    Gunfire,
    PropertyDamage,
    Fire,
    Explosion,
}

/// Who reported the event (affects report-phase ladder).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ReporterKind {
    /// Civilian: Seeking → Calling → Connected delays.
    Civilian,
    /// In-world cop: may go straight to Connected.
    Police,
}

/// One enqueued signal from hooks (enqueue-only; model tick consumes).
#[derive(Debug, Clone, PartialEq)]
pub struct Signal {
    pub kind: SignalKind,
    pub pos: WorldPos,
    pub reporter: ReporterKind,
    /// Hint from hooks / perception; used for ResponseSize with threat.
    pub criminal_count: u32,
}

impl Signal {
    pub fn new(kind: SignalKind, pos: WorldPos, reporter: ReporterKind) -> Self {
        Self {
            kind,
            pos,
            reporter,
            criminal_count: 0,
        }
    }

    pub fn with_criminals(mut self, n: u32) -> Self {
        self.criminal_count = n;
        self
    }
}

/// Despawn / pool-recycle notice drained with signals each frame.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DespawnKind {
    Ped,
    Vehicle,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Despawn {
    pub kind: DespawnKind,
    /// Opaque generation handle from runtime (not a raw engine pointer).
    pub handle: u64,
}
