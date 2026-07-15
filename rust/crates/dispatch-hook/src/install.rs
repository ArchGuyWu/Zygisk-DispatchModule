use anyhow::{Context, Result};
use dispatch_engine::{Library, NativeEventRegistry, TARGET_LIB};
use dispatch_exec::ExecSymbols;
use crate::inline::install as inline_install;
use tracing::{error, info};

use crate::event_perception::{detour_event_group_add, set_orig_event_group_add};
use crate::causation::{
    detour_generate_damage_event, detour_vehicle_inflict_damage, set_orig_generate_damage,
    set_orig_vehicle_inflict_damage,
};
use crate::lifecycle::{
    detour_population_remove_ped, detour_possibly_remove_vehicle, detour_register_kill,
    set_orig_register_kill, set_orig_remove_ped, set_orig_remove_vehicle,
};
use crate::runtime::init;
use crate::scripts::{detour_the_scripts_process, set_orig_scripts_process};
use crate::spawn::{
    detour_create_car_for_script, detour_generate_emergency_car, detour_script_generate_emergency_car,
    set_orig_create_car_for_script, set_orig_generate_emergency, set_orig_script_generate_emergency,
};
use crate::buoyancy_gate::{detour_process_buoyancy, set_orig_process_buoyancy};
use crate::event_scan_gate::{detour_scan_for_events, set_orig_scan_for_events};
use crate::static_counter_gate::{
    detour_process_static_counter, set_orig_process_static_counter,
};
use crate::control_sub_task_gate::{
    detour_control_sub_task, set_orig_control_sub_task,
};
use crate::manage_tasks_gate::{
    detour_manage_tasks, set_orig_manage_tasks,
};
use crate::record_relationship_gate::{
    detour_record_relationship, set_orig_record_relationship,
};
use crate::wanted_population::{
    detour_report_crime, detour_report_crime_now, detour_set_wanted_level,
    detour_set_wanted_level_no_drop, set_orig_report_crime, set_orig_report_crime_now,
    set_orig_set_wanted_level, set_orig_set_wanted_level_no_drop,
};

pub fn install_lifecycle_hooks() -> Result<()> {
    if !crate::config::HOOKS_ENABLED {
        log_android("install: hooks DISABLED — no patches");
        return Ok(());
    }
    log_android("install: open lib");
    let lib = Library::open(TARGET_LIB).context("open libUE4")?;
    log_android("install: resolve ExecSymbols");
    let symbols = ExecSymbols::resolve(&lib).context("resolve exec symbols")?;
    log_android("install: resolve NativeEventRegistry");
    let native_events = NativeEventRegistry::resolve(&lib);
    log_android("install: init_tracked_vptrs");
    crate::event_perception::init_tracked_vptrs(&native_events);
    log_android("install: runtime init");
    init(symbols, native_events);
    // Do not probe game state before hooks — FindPlayerPed/is_alive during early
    // load can fault the worker thread. Gate refreshes on first script tick.
    let mut installed = 0usize;
    let mut failed = 0usize;
    let mask = crate::config::HOOK_INSTALL_MASK;
    log_android(&format!("install: begin hook installs mask=0x{mask:04x}"));
    let mut one = |r: Result<usize>| {
        match r {
            Ok(n) => installed += n,
            Err(e) => {
                failed += 1;
                log_android(&format!("install skip: {e:#}"));
            }
        }
    };
    let mut hook_bit = |bit: u16, r: Result<usize>| {
        if mask & (1 << bit) != 0 {
            one(r);
        }
    };
    hook_bit(0, hook_register_kill(&lib));
    hook_bit(1, hook_remove_ped(&lib));
    hook_bit(2, hook_remove_vehicle(&lib));
    hook_bit(3, hook_scripts_process(&lib));
    hook_bit(4, hook_generate_damage_event(&lib));
    hook_bit(5, hook_vehicle_inflict_damage(&lib));
    hook_bit(6, hook_wanted_suppression(&lib));
    hook_bit(7, hook_generate_emergency_car(&lib));
    hook_bit(8, hook_script_generate_emergency_car(&lib));
    hook_bit(9, hook_create_car_for_script(&lib));
    hook_bit(10, hook_event_group_add(&lib));
    hook_bit(11, hook_process_buoyancy_gate(&lib));
    hook_bit(12, hook_scan_for_events_gate(&lib));
    hook_bit(13, hook_process_static_counter_gate(&lib));
    hook_bit(14, hook_control_sub_task_gate(&lib));
    hook_bit(15, hook_manage_tasks_gate(&lib));
    hook_bit(16, hook_record_relationship_gate(&lib));
    std::mem::forget(lib);
    info!(hooks = installed, failed, "rust dispatch hooks installed");
    let logic = if crate::config::MOD_LOGIC_ENABLED {
        "logic=ON"
    } else {
        "logic=OFF"
    };
    log_android(&format!("hooks installed: {installed} failed: {failed} ({logic})"));
    if installed == 0 {
        anyhow::bail!("no hooks installed ({failed} failed)");
    }
    Ok(())
}

fn log_android(msg: &str) {
    let cstr = std::ffi::CString::new(msg).unwrap_or_default();
    unsafe {
        extern "C" {
            fn dispatch_android_log(priority: i32, msg: *const std::ffi::c_char);
        }
        dispatch_android_log(4, cstr.as_ptr()); // ANDROID_LOG_INFO
    }
}

fn hook_register_kill(lib: &Library) -> Result<usize> {
    let mut orig: *mut std::ffi::c_void = std::ptr::null_mut();
    install_hook(
        lib,
        "_ZN13CEventHandler12RegisterKillEPK4CPedPK7CEntity11eWeaponTypeb",
        detour_register_kill as *const () as usize,
        &mut orig,
        "RegisterKill",
    )?;
    set_orig_register_kill(unsafe { std::mem::transmute(orig) });
    Ok(1)
}

fn hook_remove_ped(lib: &Library) -> Result<usize> {
    let mut orig: *mut std::ffi::c_void = std::ptr::null_mut();
    install_hook(
        lib,
        "_ZN11CPopulation9RemovePedEP4CPed",
        detour_population_remove_ped as *const () as usize,
        &mut orig,
        "RemovePed",
    )?;
    set_orig_remove_ped(unsafe { std::mem::transmute(orig) });
    Ok(1)
}

fn hook_remove_vehicle(lib: &Library) -> Result<usize> {
    let mut orig: *mut std::ffi::c_void = std::ptr::null_mut();
    install_hook(
        lib,
        "_ZN8CCarCtrl21PossiblyRemoveVehicleEP8CVehicle",
        detour_possibly_remove_vehicle as *const () as usize,
        &mut orig,
        "PossiblyRemoveVehicle",
    )?;
    set_orig_remove_vehicle(unsafe { std::mem::transmute(orig) });
    Ok(1)
}

fn hook_scripts_process(lib: &Library) -> Result<usize> {
    let mut orig: *mut std::ffi::c_void = std::ptr::null_mut();
    install_hook(
        lib,
        "_ZN11CTheScripts7ProcessEv",
        detour_the_scripts_process as *const () as usize,
        &mut orig,
        "TheScriptsProcess",
    )?;
    set_orig_scripts_process(unsafe { std::mem::transmute(orig) });
    Ok(1)
}

fn hook_generate_damage_event(lib: &Library) -> Result<usize> {
    let mut orig: *mut std::ffi::c_void = std::ptr::null_mut();
    install_hook(
        lib,
        "_ZN7CWeapon19GenerateDamageEventEP4CPedP7CEntity11eWeaponTypei14ePedPieceTypesi",
        detour_generate_damage_event as *const () as usize,
        &mut orig,
        "GenerateDamageEvent",
    )?;
    set_orig_generate_damage(unsafe { std::mem::transmute(orig) });
    Ok(1)
}

fn hook_vehicle_inflict_damage(lib: &Library) -> Result<usize> {
    let mut orig: *mut std::ffi::c_void = std::ptr::null_mut();
    install_hook(
        lib,
        "_ZN8CVehicle13InflictDamageEP7CEntity11eWeaponTypef7CVector",
        detour_vehicle_inflict_damage as *const () as usize,
        &mut orig,
        "VehicleInflictDamage",
    )?;
    set_orig_vehicle_inflict_damage(unsafe { std::mem::transmute(orig) });
    Ok(1)
}

/// C++ parity: swallow vanilla wanted via these four entry points only.
fn hook_wanted_suppression(lib: &Library) -> Result<usize> {
    let mut count = 0usize;
    count += install_wanted_hook(
        lib,
        "_ZN6CCrime11ReportCrimeE10eCrimeTypeP7CEntityP4CPed",
        detour_report_crime as *const () as usize,
        |orig| set_orig_report_crime(unsafe { std::mem::transmute(orig) }),
        "ReportCrime",
    )?;
    count += install_wanted_hook(
        lib,
        "_ZN7CWanted14ReportCrimeNowE10eCrimeTypeRK7CVectorb",
        detour_report_crime_now as *const () as usize,
        |orig| set_orig_report_crime_now(unsafe { std::mem::transmute(orig) }),
        "ReportCrimeNow",
    )?;
    count += install_wanted_hook(
        lib,
        "_ZN7CWanted14SetWantedLevelEi",
        detour_set_wanted_level as *const () as usize,
        |orig| set_orig_set_wanted_level(unsafe { std::mem::transmute(orig) }),
        "SetWantedLevel",
    )?;
    count += install_wanted_hook(
        lib,
        "_ZN7CWanted20SetWantedLevelNoDropEi",
        detour_set_wanted_level_no_drop as *const () as usize,
        |orig| set_orig_set_wanted_level_no_drop(unsafe { std::mem::transmute(orig) }),
        "SetWantedLevelNoDrop",
    )?;
    Ok(count)
}

fn install_wanted_hook(
    lib: &Library,
    mangled: &str,
    detour: usize,
    store: impl FnOnce(*mut std::ffi::c_void),
    label: &str,
) -> Result<usize> {
    let mut orig: *mut std::ffi::c_void = std::ptr::null_mut();
    install_hook(lib, mangled, detour, &mut orig, label)?;
    store(orig);
    Ok(1)
}

fn hook_generate_emergency_car(lib: &Library) -> Result<usize> {
    let mut orig: *mut std::ffi::c_void = std::ptr::null_mut();
    install_hook(
        lib,
        "_ZN8CCarCtrl31GenerateOneEmergencyServicesCarEj7CVector",
        detour_generate_emergency_car as *const () as usize,
        &mut orig,
        "GenerateEmergencyCar",
    )?;
    set_orig_generate_emergency(unsafe { std::mem::transmute(orig) });
    Ok(1)
}

fn hook_script_generate_emergency_car(lib: &Library) -> Result<usize> {
    let mut orig: *mut std::ffi::c_void = std::ptr::null_mut();
    install_hook(
        lib,
        "_ZN8CCarCtrl37ScriptGenerateOneEmergencyServicesCarEj7CVector",
        detour_script_generate_emergency_car as *const () as usize,
        &mut orig,
        "ScriptGenerateEmergencyCar",
    )?;
    set_orig_script_generate_emergency(unsafe { std::mem::transmute(orig) });
    Ok(1)
}

fn hook_event_group_add(lib: &Library) -> Result<usize> {
    let mut orig: *mut std::ffi::c_void = std::ptr::null_mut();
    install_hook(
        lib,
        "_ZN11CEventGroup3AddER6CEventb",
        detour_event_group_add as *const () as usize,
        &mut orig,
        "EventGroupAdd",
    )?;
    set_orig_event_group_add(unsafe { std::mem::transmute(orig) });
    Ok(1)
}

fn hook_create_car_for_script(lib: &Library) -> Result<usize> {
    let mut orig: *mut std::ffi::c_void = std::ptr::null_mut();
    install_hook(
        lib,
        "_ZN8CCarCtrl18CreateCarForScriptEi7CVectorh",
        detour_create_car_for_script as *const () as usize,
        &mut orig,
        "CreateCarForScript",
    )?;
    set_orig_create_car_for_script(unsafe { std::mem::transmute(orig) });
    Ok(1)
}

/// Fail-closed gate only: unwalkable task graph → skip ProcessBuoyancy (no slot writes).
fn hook_process_buoyancy_gate(lib: &Library) -> Result<usize> {
    let mut orig: *mut std::ffi::c_void = std::ptr::null_mut();
    install_hook(
        lib,
        "_ZN4CPed15ProcessBuoyancyEv",
        detour_process_buoyancy as *const () as usize,
        &mut orig,
        "ProcessBuoyancyGate",
    )?;
    set_orig_process_buoyancy(unsafe { std::mem::transmute(orig) });
    Ok(1)
}

/// Fail-closed gate: zeroed task at intel+0x28 → skip ScanForEvents (fault 0x28).
fn hook_scan_for_events_gate(lib: &Library) -> Result<usize> {
    let mut orig: *mut std::ffi::c_void = std::ptr::null_mut();
    install_hook(
        lib,
        "_ZN13CEventScanner13ScanForEventsER4CPed",
        detour_scan_for_events as *const () as usize,
        &mut orig,
        "ScanForEventsGate",
    )?;
    set_orig_scan_for_events(unsafe { std::mem::transmute(orig) });
    Ok(1)
}

/// Fail-closed gate: zeroed task on intelligence → skip ProcessStaticCounter (fault 0x18).
fn hook_process_static_counter_gate(lib: &Library) -> Result<usize> {
    let mut orig: *mut std::ffi::c_void = std::ptr::null_mut();
    install_hook(
        lib,
        "_ZN16CPedIntelligence20ProcessStaticCounterEv",
        detour_process_static_counter as *const () as usize,
        &mut orig,
        "ProcessStaticCounterGate",
    )?;
    set_orig_process_static_counter(unsafe { std::mem::transmute(orig) });
    Ok(1)
}

/// Fail-closed gate: child task with null vtable → skip ControlSubTask (fault 0x38).
fn hook_control_sub_task_gate(lib: &Library) -> Result<usize> {
    let mut orig: *mut std::ffi::c_void = std::ptr::null_mut();
    install_hook(
        lib,
        "_ZN18CTaskComplexFacial14ControlSubTaskEP4CPed",
        detour_control_sub_task as *const () as usize,
        &mut orig,
        "ControlSubTaskGate",
    )?;
    set_orig_control_sub_task(unsafe { std::mem::transmute(orig) });
    Ok(1)
}

/// Fail-closed gate: unwalkable CTaskManager slot → skip ManageTasks (fault 0x18/0x20/0x28/…).
/// One hook covers all per-frame vtable-offset variants.
fn hook_manage_tasks_gate(lib: &Library) -> Result<usize> {
    let mut orig: *mut std::ffi::c_void = std::ptr::null_mut();
    install_hook(
        lib,
        "_ZN12CTaskManager11ManageTasksEv",
        detour_manage_tasks as *const () as usize,
        &mut orig,
        "ManageTasksGate",
    )?;
    set_orig_manage_tasks(unsafe { std::mem::transmute(orig) });
    Ok(1)
}

/// Fail-closed gate: null vtable on CPlayerRelationshipRecorder → skip RecordRelationshipWithPlayer.
fn hook_record_relationship_gate(lib: &Library) -> Result<usize> {
    let mut orig: *mut std::ffi::c_void = std::ptr::null_mut();
    install_hook(
        lib,
        "_ZN27CPlayerRelationshipRecorder28RecordRelationshipWithPlayerEPK4CPed",
        detour_record_relationship as *const () as usize,
        &mut orig,
        "RecordRelationshipGate",
    )?;
    set_orig_record_relationship(unsafe { std::mem::transmute(orig) });
    Ok(1)
}

/// Dlopen resolve + Rust aarch64 absolute-jump inline hook.
fn install_hook(
    lib: &Library,
    mangled: &str,
    detour: usize,
    orig: &mut *mut std::ffi::c_void,
    label: &str,
) -> Result<()> {
    let addr = lib
        .sym(mangled)
        .with_context(|| format!("dlsym missing: {label} ({mangled})"))?;
    log_android(&format!("hook try {label} @ {addr:#x} detour {detour:#x}"));
    match inline_install(addr, detour) {
        Ok(tramp) => {
            *orig = tramp as *mut std::ffi::c_void;
            log_android(&format!("hook OK {label} orig={tramp:#x}"));
            Ok(())
        }
        Err(err) => {
            let msg = err.message();
            error!(hook = label, ?err, %msg, "install failed");
            log_android(&format!("hook FAIL {label}: {msg}"));
            anyhow::bail!("hook failed: {label}: {msg}");
        }
    }
}