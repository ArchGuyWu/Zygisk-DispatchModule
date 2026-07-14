use dispatch_engine::CVector;

use crate::gate::hook_logic_allowed;

type OrigSetWantedLevel = unsafe extern "C" fn(*mut std::ffi::c_void, i32);
type OrigReportCrimeNow =
    unsafe extern "C" fn(*mut std::ffi::c_void, i32, *const CVector, bool);
type OrigReportCrime =
    unsafe extern "C" fn(i32, *mut std::ffi::c_void, *mut std::ffi::c_void);

static mut ORIG_SET_WANTED_LEVEL: Option<OrigSetWantedLevel> = None;
static mut ORIG_SET_WANTED_LEVEL_NO_DROP: Option<OrigSetWantedLevel> = None;
static mut ORIG_REPORT_CRIME_NOW: Option<OrigReportCrimeNow> = None;
static mut ORIG_REPORT_CRIME: Option<OrigReportCrime> = None;

pub fn set_orig_set_wanted_level(f: OrigSetWantedLevel) {
    unsafe { ORIG_SET_WANTED_LEVEL = Some(f) };
}

pub fn set_orig_set_wanted_level_no_drop(f: OrigSetWantedLevel) {
    unsafe { ORIG_SET_WANTED_LEVEL_NO_DROP = Some(f) };
}

pub fn set_orig_report_crime_now(f: OrigReportCrimeNow) {
    unsafe { ORIG_REPORT_CRIME_NOW = Some(f) };
}

pub fn set_orig_report_crime(f: OrigReportCrime) {
    unsafe { ORIG_REPORT_CRIME = Some(f) };
}

/// C++ `dispatch_active()` — true while mod dispatch zone is playable.
#[inline]
fn swallow_wanted() -> bool {
    hook_logic_allowed()
}

pub unsafe extern "C" fn detour_set_wanted_level(wanted: *mut std::ffi::c_void, level: i32) {
    if swallow_wanted() {
        return;
    }
    if let Some(orig) = ORIG_SET_WANTED_LEVEL {
        orig(wanted, level);
    }
}

pub unsafe extern "C" fn detour_set_wanted_level_no_drop(
    wanted: *mut std::ffi::c_void,
    level: i32,
) {
    if swallow_wanted() {
        return;
    }
    if let Some(orig) = ORIG_SET_WANTED_LEVEL_NO_DROP {
        orig(wanted, level);
    }
}

pub unsafe extern "C" fn detour_report_crime_now(
    wanted: *mut std::ffi::c_void,
    crime_type: i32,
    pos: *const CVector,
    unk: bool,
) {
    if swallow_wanted() {
        return;
    }
    if let Some(orig) = ORIG_REPORT_CRIME_NOW {
        orig(wanted, crime_type, pos, unk);
    }
}

pub unsafe extern "C" fn detour_report_crime(
    crime_type: i32,
    victim: *mut std::ffi::c_void,
    perp: *mut std::ffi::c_void,
) {
    if swallow_wanted() {
        return;
    }
    if let Some(orig) = ORIG_REPORT_CRIME {
        orig(crime_type, victim, perp);
    }
}