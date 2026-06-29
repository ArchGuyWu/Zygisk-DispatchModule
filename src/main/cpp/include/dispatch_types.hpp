#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "game_types.hpp"

struct CrimeEvent {
    CPed* criminal = nullptr;
    CVector location;
    bool is_firearm = false;
    int64_t detect_time_ms = 0;
    bool dispatch_sent = false;
    bool cancelled = false;
    int cops_dispatched = 0;
    int cops_killed = 0;
    int reinforcements_sent = 0;

    std::vector<CPed*> consolidated_criminals;
    std::vector<bool> criminal_is_firearm;

    struct CriminalState {
        int first_threat_category = 0;
        int current_threat_category = 0;
        bool is_active = true;
        bool shooting_air = false;
        bool fleeing = false;
    };
    std::unordered_map<CPed*, CriminalState> criminal_states;

    void* spawned_vehicle = nullptr;
    bool occupants_ordered_out = false;
    int64_t spawn_time_ms = 0;

    struct DelayedTask {
        int64_t execute_time_ms;
        std::function<void()> callback;
    };
    std::vector<DelayedTask> pending_tasks;

    bool road_closure_active = false;
    CVector road_closure_center;

    int dispatch_state = 0;
    int64_t timer_start = 0;
    int dispatch_delay_ms = 0;
    int last_cops_killed = 0;
    int64_t on_scene_start = 0;
    std::vector<void*> case_vehicles;
    uint64_t case_id = 0;
};

struct CopVehicleBinding {
    CPed* cop;
    void* vehicle;
    bool as_driver;
};

struct CrimeActiveCompat {
    bool load() const;
    operator bool() const { return load(); }
};

enum DispatchState {
    STATE_IDLE = 0,
    STATE_TIMING,
    STATE_DISPATCHED,
    STATE_ON_SCENE,
    STATE_CLEANUP,
};