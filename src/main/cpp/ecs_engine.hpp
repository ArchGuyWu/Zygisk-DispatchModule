#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <typeindex>
#include <memory>
#include <functional>
#include <string>
#include <mutex>
#include <iostream>
#include <algorithm>

// =====================================================================
// 🚀 极量级高性能 ECS & 事件驱动引擎 (Zygisk-DispatchModule 专用版)
// =====================================================================

namespace ecs {

// ---------------------------------------------------------------------
// 1. 实体 (Entity)
// ---------------------------------------------------------------------
// 在本模组中，实体直接由游戏底层对象的内存指针 (如 CPed*, CVehicle*) 标识。
// 这保证了与游戏引擎原本对象树的 直接持有引擎对象指针。
using Entity = void*;

// ---------------------------------------------------------------------
// 2. 组件 (Component) 基类
// ---------------------------------------------------------------------
struct IComponent {
    virtual ~IComponent() = default;
};

// ---------------------------------------------------------------------
// 3. 事件基类与事件派发器 (Event Driven System)
// ---------------------------------------------------------------------
struct Event {
    virtual ~Event() = default;
    virtual std::string name() const = 0;
};

class EventDispatcher {
public:
    using EventHandler = std::function<void(const Event&)>;

    static EventDispatcher& get() {
        static EventDispatcher instance;
        return instance;
    }

    template <typename TEvent>
    void subscribe(const std::string& event_name, std::function<void(const TEvent&)> handler) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        handlers_[event_name].push_back([handler](const Event& event) {
            handler(static_cast<const TEvent&>(event));
        });
    }

    void dispatch(const Event& event) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto it = handlers_.find(event.name());
        if (it != handlers_.end()) {
            for (auto& handler : it->second) {
                handler(event);
            }
        }
    }

private:
    EventDispatcher() = default;
    std::unordered_map<std::string, std::vector<EventHandler>> handlers_;
    std::recursive_mutex mutex_;
};

// ---------------------------------------------------------------------
// 4. 实体与组件管理器 (EntityManager)
// ---------------------------------------------------------------------
class EntityManager {
public:
    static EntityManager& get() {
        static EntityManager instance;
        return instance;
    }

    // 注册/创建实体
    void register_entity(Entity entity) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (entities_.find(entity) == entities_.end()) {
            entities_.insert(entity);
        }
    }

    // 销毁实体及其上绑定的所有组件
    void destroy_entity(Entity entity) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        entities_.erase(entity);
        components_map_.erase(entity);
    }

    bool has_entity(Entity entity) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return entities_.find(entity) != entities_.end();
    }

    // 为实体添加组件
    template <typename TComponent, typename... Args>
    TComponent* add_component(Entity entity, Args&&... args) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        register_entity(entity);
        auto& entity_components = components_map_[entity];
        auto type_idx = std::type_index(typeid(TComponent));
        
        auto comp = std::make_shared<TComponent>(std::forward<Args>(args)...);
        entity_components[type_idx] = comp;
        return comp.get();
    }

    // 获取实体的组件
    template <typename TComponent>
    TComponent* get_component(Entity entity) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto it = components_map_.find(entity);
        if (it == components_map_.end()) return nullptr;

        auto& entity_components = it->second;
        auto type_idx = std::type_index(typeid(TComponent));
        auto comp_it = entity_components.find(type_idx);
        if (comp_it == entity_components.end()) return nullptr;

        return static_cast<TComponent*>(comp_it->second.get());
    }

    // 移除实体的组件
    template <typename TComponent>
    void remove_component(Entity entity) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        auto it = components_map_.find(entity);
        if (it != components_map_.end()) {
            auto type_idx = std::type_index(typeid(TComponent));
            it->second.erase(type_idx);
        }
    }

    // 获取带有特定组件的所有实体
    template <typename TComponent>
    std::vector<Entity> get_entities_with() {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        std::vector<Entity> results;
        auto type_idx = std::type_index(typeid(TComponent));
        for (const auto& pair : components_map_) {
            if (pair.second.find(type_idx) != pair.second.end()) {
                results.push_back(pair.first);
            }
        }
        return results;
    }

    // 清理所有实体
    void clear() {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        entities_.clear();
        components_map_.clear();
    }

private:
    EntityManager() = default;
    std::unordered_set<Entity> entities_;
    std::unordered_map<Entity, std::unordered_map<std::type_index, std::shared_ptr<IComponent>>> components_map_;
    std::recursive_mutex mutex_;
};

// ---------------------------------------------------------------------
// 5. Zygisk-PoliceMod 专用组件定义 (Components)
// ---------------------------------------------------------------------

// 5.1 警员组件 (保存警车绑定、下车状态、以及卡死检测所需的所有警员原始数据)
struct CopComponent : public IComponent {
    Entity cop_ped = nullptr;
    bool is_in_vehicle = false;
    bool has_exited_vehicle = false;
    int64_t last_assign_time_ms = 0;
    int64_t last_enter_vehicle_command_time_ms = 0;
    int pool_handle = -1;
    bool dispatch_active = false;
    void* cached_vehicle = nullptr;
    int64_t cached_vehicle_lookup_ms = 0;
    
    // Stuck Tracker (卡死检测)
    int64_t last_stuck_check_time_ms = 0;
    float last_pos_x = 0.0f;
    float last_pos_y = 0.0f;
    float last_pos_z = 0.0f;
    int stuck_count = 0;

    CopComponent(Entity ped) : cop_ped(ped) {}
};

// 5.2 战斗组件 (管理警员与犯罪NPC之间的武器匹配状态和指派目标)
struct CombatComponent : public IComponent {
    Entity target_entity = nullptr;
    int target_ped_handle = -1;
    uint64_t target_case_id = 0;
    uint64_t assigned_case_id = 0;
    int64_t last_dispatch_assign_ms = 0;
    int64_t last_armed_time_ms = 0;
    int64_t last_shot_time_ms = 0;
    int64_t last_weapon_switch_time_ms = 0;
    int current_weapon_type = 0;

    CombatComponent() = default;
};

enum class CriminalThreatLevel {
    UNARMED_INACTIVE = 0,
    UNARMED_ACTIVE = 1,
    MELEE_INACTIVE = 2,
    MELEE_ACTIVE = 3,
    FIREARM_INACTIVE = 4,
    FIREARM_AIR_SHOOT = 5,
    FIREARM_ACTIVE = 6
};

// 5.3 犯罪分子组件 (管理被警方锁定的犯罪分子的开枪和自卫状态数据)
struct CriminalComponent : public IComponent {
    Entity criminal_ped = nullptr;
    int initial_weapon_category = 0; // 0: UNARMED, 1: MELEE, 2: FIREARM
    bool is_active = false;          // 是否活跃 (仍在攻击人)
    bool is_air_shooter = false;     // 是否对空气开枪
    bool is_fleeing = false;         // 是否在开枪后逃跑
    int64_t last_attack_time_ms = 0; // 上次攻击/开火时间
    Entity current_victim = nullptr; // 当前受害人
    int64_t first_detect_time_ms = 0;
    int pool_handle = -1;
    uint64_t case_id = 0;

    CriminalComponent(Entity ped) : criminal_ped(ped) {}

    // 计算分级优先级
    CriminalThreatLevel get_threat_level() const {
        if (initial_weapon_category == 2) { // FIREARM
            if (is_fleeing || !is_active) {
                return CriminalThreatLevel::FIREARM_INACTIVE;
            }
            if (is_air_shooter) {
                return CriminalThreatLevel::FIREARM_AIR_SHOOT;
            }
            return CriminalThreatLevel::FIREARM_ACTIVE;
        } else if (initial_weapon_category == 1) { // MELEE
            if (!is_active) {
                return CriminalThreatLevel::MELEE_INACTIVE;
            }
            return CriminalThreatLevel::MELEE_ACTIVE;
        } else { // UNARMED
            if (!is_active) {
                return CriminalThreatLevel::UNARMED_INACTIVE;
            }
            return CriminalThreatLevel::UNARMED_ACTIVE;
        }
    }
};

// ---------------------------------------------------------------------
// 6. Zygisk-PoliceMod 专用核心事件定义 (Events)
// ---------------------------------------------------------------------

// 6.1 伤害/受袭事件 (取代旧的 CEventDamage constructors 的过程式回调)
struct DamageEvent : public Event {
    Entity victim = nullptr;       // 受害者
    Entity attacker = nullptr;     // 施袭者
    int weapon_type = 0;           // 使用的武器
    bool is_fatal = false;         // 是否致命伤害
    int64_t time_ms = 0;           // 伤害发生的时间戳

    DamageEvent(Entity vic, Entity att, int weap, bool fatal, int64_t t)
        : victim(vic), attacker(att), weapon_type(weap), is_fatal(fatal), time_ms(t) {}

    std::string name() const override { return "DamageEvent"; }
};

// 6.2 武器切换事件 (当任意犯罪NPC或警员切换武器时派发)
struct WeaponSwitchEvent : public Event {
    Entity ped = nullptr;          // 切枪的 Ped
    int previous_weapon = 0;
    int current_weapon = 0;
    int64_t time_ms = 0;

    WeaponSwitchEvent(Entity p, int prev, int curr, int64_t t)
        : ped(p), previous_weapon(prev), current_weapon(curr), time_ms(t) {}

    std::string name() const override { return "WeaponSwitchEvent"; }
};

// 6.3 犯罪通报事件 (当游戏内部产生犯罪上报，或触发暴动作弊码时派发)
struct CrimeReportEvent : public Event {
    Entity criminal = nullptr;     // 犯罪头目
    Entity victim = nullptr;       // 受害者 (如有)
    CVector location;              // 犯罪区域中心
    bool is_firearm = false;       // 是否涉及枪械犯罪
    int weapon_category = 0;       // 0: UNARMED, 1: MELEE, 2: FIREARM
    int64_t time_ms = 0;

    CrimeReportEvent(Entity crim, Entity vic, CVector loc, bool firearm, int weap_cat, int64_t t)
        : criminal(crim), victim(vic), location(loc), is_firearm(firearm), weapon_category(weap_cat), time_ms(t) {}

    std::string name() const override { return "CrimeReportEvent"; }
};

// 6.4 实体注销/清理事件 (在 Ped 死亡或被游戏引擎清理回收时派发)
struct EntityCleanupEvent : public Event {
    Entity entity = nullptr;       // 被清理的实体
    bool is_dead = false;          // 是否因为死亡被清理

    EntityCleanupEvent(Entity ent, bool dead) : entity(ent), is_dead(dead) {}

    std::string name() const override { return "EntityCleanupEvent"; }
};

// 6.5 主线程 Tick 事件 (事件驱动引擎在每帧或高频 Tick 时派发，分发给系统进行轮询管理)
struct TickEvent : public Event {
    int64_t current_time_ms = 0;

    TickEvent(int64_t t) : current_time_ms(t) {}

    std::string name() const override { return "TickEvent"; }
};

} // namespace ecs
