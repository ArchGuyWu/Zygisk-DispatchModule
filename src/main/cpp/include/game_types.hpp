#pragma once

// GTA SA 引擎前向声明（不透明指针）
struct CEntity;
struct CPed;
struct CPlayerPed;
struct CCopPed;
struct CVehicle;
struct CPlaceable;
struct CTaskManager;
struct CPedIntelligence;
struct CTask;
struct CEvent;

// CVector: GTA SA 标准 3D 向量
struct CVector {
    float x, y, z;
    CVector() : x(0), y(0), z(0) {}
    CVector(float _x, float _y, float _z) : x(_x), y(_y), z(_z) {}
};

struct CVector2D {
    float x, y;
    CVector2D() : x(0), y(0) {}
    CVector2D(float _x, float _y) : x(_x), y(_y) {}
};

// CMatrix: GTA SA 4x4 矩阵（简化版，只需要位置分量）
struct CMatrix {
    float right_x, right_y, right_z, pad0;
    float up_x, up_y, up_z, pad1;
    float at_x, at_y, at_z, pad2;
    float pos_x, pos_y, pos_z, pad3;
};

// 犯罪类型枚举（部分常用值）
enum eCrimeType : int {
    CRIME_NONE = 0,
    CRIME_FIRE_WEAPON = 1,
    CRIME_DAMAGED_PED = 3,
    CRIME_DAMAGED_COP = 4,
    CRIME_DAMAGED_CAR = 5,
    CRIME_DAMAGED_COP_CAR = 6,
    CRIME_KILL_PED_WITH_GUN = 12,
    CRIME_KILL_PED_NO_GUN = 13,
    CRIME_KILL_COP = 14,
};

// 武器类型枚举（简化）
enum eWeaponType : int {
    WEAPON_UNARMED = 0,
    WEAPON_BRASSKNUCKLE = 1,
    WEAPON_GOLFCLUB = 2,
    WEAPON_NIGHTSTICK = 3,
    WEAPON_PISTOL = 22,
    WEAPON_PISTOL_SILENCED = 23,
    WEAPON_DESERT_EAGLE = 24,
    WEAPON_SHOTGUN = 25,
    WEAPON_SAWNOFF = 26,
    WEAPON_SPAS12 = 27,
    WEAPON_MICRO_UZI = 28,
    WEAPON_MP5 = 29,
    WEAPON_AK47 = 30,
    WEAPON_M4 = 31,
    WEAPON_TEC9 = 32,
    WEAPON_RIFLE = 33,
    WEAPON_SNIPER = 34,
    WEAPON_MINIGUN = 38,
};

// 函数指针类型定义（全部从 nm -D 确认 of 导出符号）

typedef CPlayerPed* (*fn_FindPlayerPed_t)(int);
typedef CVector (*fn_FindPlayerCoors_t)(int);
typedef int (*fn_GetPedType_t)(const void*);
typedef CMatrix* (*fn_GetMatrix_t)(void*);
typedef float (*fn_FindDistToNearestPedOfType_t)(int, CVector);
typedef void (*fn_ScriptGenEmergencyCar_t)(unsigned int, CVector);
typedef void (*fn_GenOneEmergencyCar_t)(unsigned int, CVector);
typedef void (*fn_AddPoliceOccupants_t)(CVehicle*, bool);
typedef void (*fn_AddCriminalToKill_t)(void*, CPed*);
typedef void (*fn_RegisterKill_t)(const CPed*, const CEntity*, eWeaponType, bool);
typedef int (*fn_IsPedDead_t)(void*, CPed*);
typedef void (*fn_WorldAdd_t)(CEntity*);
typedef void (*fn_WorldRemove_t)(CEntity*);
typedef void (*fn_SetTask_t)(void*, CTask*, int, bool);
typedef void (*fn_TaskLeaveCar_ctor_t)(void*, CVehicle*, int, int, bool, bool);
typedef void (*fn_TaskEnterCar_ctor_t)(void*, CVehicle*, bool, bool, bool, bool);
typedef void (*fn_TaskKillCriminal_ctor_t)(void*, CPed*, bool);
typedef void (*fn_ReportCrime_orig_t)(eCrimeType, CEntity*, CPed*);
typedef void (*fn_SetWantedLevel_orig_t)(void*, int);
typedef CEntity* (*fn_GetWeaponLockOnTarget_t)(const void*);
typedef bool (*fn_IsAlive_t)(const void*);
typedef bool (*fn_GenerateDamageEvent_orig_t)(CPed*, CEntity*, eWeaponType, int, int, int);
typedef void (*fn_VehicleInflictDamage_t)(void*, CEntity*, eWeaponType, float, CVector);
typedef CPed* (*fn_GetPoolPed_t)(int);
typedef void (*fn_GiveWeapon_t)(void*, eWeaponType, unsigned int, bool);
typedef void (*fn_SetCurrentWeapon_t)(void*, eWeaponType);
typedef void (*fn_GiveWeaponAtStartOfFight_t)(void*);