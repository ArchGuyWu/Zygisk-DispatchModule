use dispatch_case::{
    anchor_for_signal, pack_entity_refs, perpetrator_entity_refs, signal_entity_refs,
    witness_ranges_for, WitnessObservation, MAX_WITNESS_ENTITIES, MAX_WITNESS_PERPETRATORS,
};
use dispatch_core::{CausalKind, CausalSignal, PoolKey, WorldPos};


use crate::runtime::DispatchRuntime;
use crate::witness::witness_ped_type_eligible;
const MAX_GEOMETRIC_WITNESSES: usize = 16;
/// Full-pool geometric witness scan — run from main tick, not damage/event hooks.
pub(crate) fn geometric_scan(rt: &mut DispatchRuntime, signal: CausalSignal) {
    let anchor = anchor_for_signal(signal);
    if anchor == WorldPos::default() {
        return;
    }
    let (heard_range, saw_range) = witness_ranges_for(signal);
    let heard_sq = heard_range * heard_range;
    let saw_sq = saw_range * saw_range;
    let kind_bit = 1 << signal.kind() as u8;
    let entities: Vec<_> = signal_entity_refs(signal)
        .into_iter()
        .filter(|entity| entity.ptr() != std::ptr::null())
        .collect();
    let (entity_buf, entity_count) = pack_entity_refs::<MAX_WITNESS_ENTITIES>(&entities);
    let perpetrators: Vec<_> = perpetrator_entity_refs(signal)
        .into_iter()
        .filter(|entity| entity.ptr() != std::ptr::null())
        .collect();
    let (perp_buf, perp_count) = pack_entity_refs::<MAX_WITNESS_PERPETRATORS>(&perpetrators);
    let mut perp_ptrs = [0usize; MAX_WITNESS_PERPETRATORS];
    let perp_ptr_count = perpetrators.len().min(MAX_WITNESS_PERPETRATORS);
    for (dst, who) in perp_ptrs[..perp_ptr_count]
        .iter_mut()
        .zip(perpetrators.iter())
    {
        *dst = who.ptr() as usize;
    }

    let pool = match rt.symbols.open_ped_pool() {
        Some(pool) => pool,
        None => return,
    };

    // Online top-K by distance — avoid unbounded Vec then sort of whole pool hits.
    type Cand = (u16, u8, *const std::ffi::c_void, WorldPos, bool, bool, f32);
    let mut candidates: Vec<Cand> = Vec::with_capacity(MAX_GEOMETRIC_WITNESSES);
    let mut farthest_d2 = f32::MAX;
    let max_range = heard_range.max(saw_range);
    let max_range_sq = max_range * max_range;

    for slot in 0..pool.size as usize {
        let flag = pool.byte_map[slot];
        if flag < 0 {
            continue;
        }
        let key = PoolKey::from_slot_flag(slot as u16, flag as u8);
        let ped = rt.symbols.pool_ped(key.handle());
        if !rt.symbols.ped_alive(ped) {
            continue;
        }
        let ped_type = rt.symbols.ped_type_of(ped);
        if !witness_ped_type_eligible(ped_type) {
            continue;
        }
        let ped_addr = ped as usize;
        if perp_ptr_count > 0 && perp_ptrs[..perp_ptr_count].contains(&ped_addr) {
            continue;
        }

        let witness_pos = rt.symbols.entity_world_pos(ped);
        if witness_pos == WorldPos::default() {
            continue;
        }
        let dx = witness_pos.x - anchor.x;
        let dy = witness_pos.y - anchor.y;
        let d2_xy = dx * dx + dy * dy;
        // Cheap XY reject before z / kind work.
        if d2_xy > max_range_sq {
            continue;
        }
        // If we already have K closer hits, skip farther XY (cannot beat farthest in top-K).
        if candidates.len() >= MAX_GEOMETRIC_WITNESSES && d2_xy >= farthest_d2 {
            continue;
        }
        let d2 = d2_xy + (witness_pos.z - anchor.z).powi(2);
        let heard = d2 <= heard_sq;
        let saw = d2 <= saw_sq && kind_is_visual(signal.kind());
        if !heard && !saw {
            continue;
        }
        let entry = (key.slot, key.generation, ped, witness_pos, heard, saw, d2);
        if candidates.len() < MAX_GEOMETRIC_WITNESSES {
            candidates.push(entry);
            if candidates.len() == MAX_GEOMETRIC_WITNESSES {
                farthest_d2 = candidates
                    .iter()
                    .map(|c| c.6)
                    .fold(0.0_f32, f32::max);
            }
        } else if d2 < farthest_d2 {
            let mut worst_i = 0usize;
            let mut worst_d = candidates[0].6;
            for (i, c) in candidates.iter().enumerate().skip(1) {
                if c.6 > worst_d {
                    worst_d = c.6;
                    worst_i = i;
                }
            }
            candidates[worst_i] = entry;
            farthest_d2 = candidates.iter().map(|c| c.6).fold(0.0_f32, f32::max);
        }
    }

    let suspect_pos = perpetrators
        .first()
        .map(|who| rt.symbols.entity_world_pos(who.ptr()));

    for (slot, generation, ped, witness_pos, heard, saw, _) in candidates {
        let key = PoolKey::from_slot_flag(slot, generation);
        rt.cache_ptr_key(ped, key, true);
        rt.cache_event_group_for_ped(ped);
        let ped_id = rt
            .registry
            .ped_by_pool(key)
            .unwrap_or_else(|| rt.registry.adopt_ped(key, rt.symbols.ped_type_of(ped)));
        rt.witness_reports.observe(WitnessObservation {
            ped: ped_id,
            pos: witness_pos,
            panicked: false,
            heard,
            saw,
            perceived_entities: entity_buf,
            perceived_entity_count: entity_count,
            perceived_kinds: kind_bit,
            perceived_perpetrators: perp_buf,
            perceived_perpetrator_count: perp_count,
            suspect_pos,
        });
    }
}

fn kind_is_visual(kind: CausalKind) -> bool {
    dispatch_case::kind_is_visual(kind)
}