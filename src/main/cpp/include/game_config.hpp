#pragma once

// 目标应用
static constexpr const char* TARGET_PACKAGE     = "com.rockstargames.gtasa.de";
static constexpr const char* TARGET_PACKAGE_ALT = "com.netflix.NGP.GTASanAndreasDefinitiveEdition";
static constexpr const char* TARGET_LIB         = "libUE4.so";

// PedType 常量
static constexpr int PED_TYPE_PLAYER    = 0;
static constexpr int PED_TYPE_COP       = 6;
static constexpr int PED_TYPE_CIVMALE   = 4;
static constexpr int PED_TYPE_CIVFEMALE = 5;
static constexpr int PED_TYPE_DEALER    = 17;
static constexpr int PED_TYPE_GANG1     = 7;   // Ballas
static constexpr int PED_TYPE_GANG8     = 14;  // Varrio Los Aztecas
static constexpr int PED_TYPE_CRIMINAL  = 20;

// 警车模型 ID
static constexpr unsigned int MODEL_POLICE_CAR = 596;  // LSPD 警车
static constexpr unsigned int MODEL_SWAT_VAN   = 427;  // SWAT Enforcer 警车