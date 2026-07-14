//! Vehicle-bound cops — drive command issued once at dispatch; mod only assigns ped tasks.

use dispatch_core::{PedId, VehicleId, WorldPos};

use crate::attack::CopAttackContext;
use crate::game::ExecEnv;
use crate::response::make_single_cop_attack_criminal;

pub fn dispatch_vehicle_cop(
    env: &mut ExecEnv<'_>,
    ctx: &mut CopAttackContext,
    ped: PedId,
    target_criminal: PedId,
    _target_crime_pos: WorldPos,
    veh: VehicleId,
    now_ms: i64,
) {
    if !env.registry.contains_vehicle(veh) || env.globals.is_transport_vehicle(veh) {
        return;
    }

    env.sync_natural_vehicle_exits(now_ms);
    if env.globals.is_vehicle_emptied(veh) {
        ctx.vehicles_emptied.insert(veh);
    }

    make_single_cop_attack_criminal(env, ped, target_criminal, true, now_ms);
}