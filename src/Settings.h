#pragma once
#include <cstdint>

namespace SETTINGS
{
    struct Data
    {
        // General
        bool  enabled{ true };
        bool  playerOnly{ false };
        bool  differentNPCs{ false };
        // Player MR 
        float mrPlayer{ 40.0f };
        float maxDRPlayer{ 567.0f };
        // NPC MR 
        float mrNPC{ 40.0f };
        float maxDRNPC{ 567.0f };
        // Log
        int   logLevel{ 1 };
        // Material bonuses 
        bool  materialBonusEnabled{ true };
        bool  materialBonusPlayerOnly{ false };
        bool  materialBonusDifferentNPCs{ false };
        // Misc
        bool  advancedDebug{ false };
        bool  npcRepair{ false };
        bool  playerRepair{ false };
        // Resistances Rescaled compatibility
        bool           rrArmorCompat{ false };
        bool           rrMRCompat{ false };
    };

    // Curve parameters read directly from RR MCM INI
    struct RRParams
    {
        int   armorFormula = 0;    // 0 = hyperbolic 1 = exponential
        float armorAt0 = 0.0f;
        float armorAt1000 = 75.0f;
        int   magicFormula = 0;
        float magicAt0 = 0.0f;
        float magicAt100 = 75.0f;
        bool  loaded = false;
    };

    RRParams LoadRRParams();

    bool        Load();
    const Data& Get();
}