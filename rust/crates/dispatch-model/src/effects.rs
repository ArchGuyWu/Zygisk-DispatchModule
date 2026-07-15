//! Effects: model → engine only language (MODEL.md §6).

use crate::case::CaseId;

/// Intent produced only by model tick; discarded after `engine.apply`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Effect {
    /// Case opened at Connected.
    OpenCase { case_id: CaseId },
    /// Case finished — stand-down / GC path.
    CloseCase { case_id: CaseId },
    /// Task existing nearby units toward scene.
    MobilizeNearby { case_id: CaseId, cap: u8 },
    /// Spawn police patrol response.
    SpawnPatrol { case_id: CaseId },
    /// Spawn SWAT when ResponseSize says so.
    SpawnSwat { case_id: CaseId },
    /// Spawn FBI when ResponseSize says so.
    SpawnFbi { case_id: CaseId },
    /// Spawn EMS; subject to ≤2 same dept in view on severe stack.
    SpawnAmbulance { case_id: CaseId },
    /// Spawn Fire; same ≤2 view/stream hard cap.
    SpawnFiretruck { case_id: CaseId },
    /// Reinforce wanted suppression while police cases active.
    SuppressWanted,
    /// OnScene police attack evaluation / tasking.
    AttackPass { case_id: CaseId },
    /// OnScene police arrest evaluation / tasking.
    ArrestPass { case_id: CaseId },
}
