//! En-route rerouting (ported from `dispatch_reroute.cpp`).

use std::collections::HashMap;

use dispatch_case::{CaseId, CaseRecord, CaseStore};
use dispatch_core::{PedId, VehicleId, WorldPos};

use crate::game::ExecEnv;
use crate::snapshot::CaseRerouteSnap;
use crate::response::make_single_cop_attack_criminal;

use crate::timing::av_range_for_firearm;

pub fn find_higher_threat_case_in_av<'a>(
    from: &CaseRerouteSnap,
    observer_pos: WorldPos,
    cases: &'a [CaseRerouteSnap],
) -> Option<(CaseId, &'a CaseRerouteSnap)> {
    let tier_a = from.threat_tier;
    let mut best: Option<(CaseId, usize, f32)> = None;

    for (idx, case_b) in cases.iter().enumerate() {
        if case_b.cancelled || case_b.case_id == from.case_id {
            continue;
        }
        if case_b.threat_tier < tier_a {
            continue;
        }
        let anchor = case_b.dispatch_pos();
        let dist = dist_3d(observer_pos, anchor);
        let av = av_range_for_firearm(case_b.is_firearm);
        if dist <= av {
            if best.map(|(_, _, d)| dist < d).unwrap_or(true) {
                best = Some((case_b.case_id, idx, dist));
            }
        }
    }
    best.map(|(id, idx, _)| (id, &cases[idx]))
}

pub fn try_reroute_foot_cop(
    env: &mut ExecEnv<'_>,
    cop: PedId,
    current_case: &CaseRerouteSnap,
    cases: &[CaseRerouteSnap],
    now_ms: i64,
) -> bool {
    let cop_pos = env.ped_pos(cop);
    let Some((target_id, target_case)) = find_higher_threat_case_in_av(current_case, cop_pos, cases)
    else {
        return false;
    };
    if target_id == current_case.case_id {
        return false;
    }
    let Some(criminal) = target_case.primary else {
        return false;
    };
    tracing::info!(
        ?cop,
        from = current_case.serial,
        to = target_case.serial,
        "foot cop en-route reroute"
    );
    make_single_cop_attack_criminal(env, cop, criminal, true, now_ms);
    true
}

pub fn apply_enroute_vehicle_reroutes(
    env: &mut ExecEnv<'_>,
    store: &mut CaseStore,
    all_read: &[CaseRerouteSnap],
    now_ms: i64,
) {
    struct Pending {
        vehicle: VehicleId,
        from_id: CaseId,
        to_id: CaseId,
        distance: f32,
    }
    let mut pending = Vec::new();

    for from_case in all_read {
        if from_case.case_vehicles.is_empty() {
            continue;
        }
        for &vehicle in &from_case.case_vehicles {
            if !env.registry.contains_vehicle(vehicle) {
                continue;
            }
            if env.globals.is_transport_vehicle(vehicle)
                || !env.globals.is_vehicle_ordered_to_scene(vehicle)
                || env.globals.is_vehicle_emptied(vehicle)
            {
                continue;
            }
            let veh_pos = env.vehicle_pos(vehicle);
            if let Some((to_id, to_case)) =
                find_higher_threat_case_in_av(from_case, veh_pos, all_read)
            {
                if to_id != from_case.case_id {
                    let anchor = to_case.dispatch_pos();
                    pending.push(Pending {
                        vehicle,
                        from_id: from_case.case_id,
                        to_id,
                        distance: dist_3d(veh_pos, anchor),
                    });
                }
            }
        }
    }

    let snap_by_id: HashMap<CaseId, &CaseRerouteSnap> =
        all_read.iter().map(|snap| (snap.case_id, snap)).collect();

    for record in pending {
        let Some(to_case_read) = snap_by_id.get(&record.to_id).copied() else {
            continue;
        };
        let intel_pos = to_case_read.dispatch_pos();
        let is_firearm = to_case_read.is_firearm;

        if let Some(from_mut) = store.get_mut(record.from_id) {
            from_mut.case_vehicles.retain(|v| *v != record.vehicle);
            if from_mut.spawned_vehicle == Some(record.vehicle) {
                from_mut.spawned_vehicle = None;
            }
        }
        if let Some(to_mut) = store.get_mut(record.to_id) {
            let _ = (to_mut.primary, is_firearm);
            if !to_mut.case_vehicles.contains(&record.vehicle) {
                to_mut.case_vehicles.push(record.vehicle);
            }
            if to_mut.spawned_vehicle.is_none() {
                to_mut.spawned_vehicle = Some(record.vehicle);
            }
            crate::dispatch_disturbance::dispatch_vehicle_to_disturbance(
                env, to_mut, record.vehicle, intel_pos, true,
            );
            tracing::info!(
                ?record.vehicle,
                to = to_case_read.serial,
                dist = record.distance,
                "vehicle en-route reroute"
            );
        }
        let _ = now_ms;
    }
}

fn dist_3d(a: WorldPos, b: WorldPos) -> f32 {
    let dx = a.x - b.x;
    let dy = a.y - b.y;
    let dz = a.z - b.z;
    (dx * dx + dy * dy + dz * dz).sqrt()
}