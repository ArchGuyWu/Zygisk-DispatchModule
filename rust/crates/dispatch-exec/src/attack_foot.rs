//! Foot-cop attack dispatch (ported from `dispatch_cop_attack_pass_foot.cpp`).

use dispatch_core::PedId;

use crate::attack::CopAttackContext;
use crate::game::ExecEnv;
use crate::response::make_single_cop_attack_criminal;


pub fn dispatch_foot_cop(
    env: &mut ExecEnv<'_>,
    ctx: &mut CopAttackContext,
    ped: PedId,
    target_criminal: PedId,
    _target_crime_pos: dispatch_core::WorldPos,
    dist_sq: f32,
    now_ms: i64,
) {
    let scan_range_sq = if ctx.av_range_sq > 0.0 {
        ctx.av_range_sq
    } else {
        crate::timing::AV_RANGE_FIREARM_M * crate::timing::AV_RANGE_FIREARM_M
    };
    if dist_sq > scan_range_sq {
        return;
    }

    let within_native_av = ctx.av_range_sq > 0.0 && dist_sq <= ctx.av_range_sq;
    let already_targeting = crate::response::cop_is_already_pursuing(env, ped, target_criminal);

    let just_exited = env.globals.is_cop_exiting(ped, now_ms);

    if !within_native_av && !just_exited && already_targeting {
        return;
    }

    let already_assigned = already_targeting;
    if !within_native_av
        && !already_assigned
        && ctx.active_foot_cops_count >= ctx.max_foot_cops
    {
        return;
    }

    make_single_cop_attack_criminal(env, ped, target_criminal, within_native_av, now_ms);

    if !already_assigned {
        ctx.active_foot_cops_count += 1;
    }
    tracing::info!(
        ?ped,
        ?target_criminal,
        within_native_av,
        active = ctx.active_foot_cops_count,
        max = ctx.max_foot_cops,
        "foot cop dispatch"
    );
}