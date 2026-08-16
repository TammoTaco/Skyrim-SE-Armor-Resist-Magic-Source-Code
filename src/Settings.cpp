#include "Settings.h"

#include <RE/Skyrim.h>
#include <spdlog/spdlog.h>

#include <util/SimpleIni.h>
#include <algorithm>
#include <filesystem>
#include <windows.h>

namespace SETTINGS
{
    namespace
    {
        Data g_settings{};
        bool g_loaded = false;

        inline const std::filesystem::path kMCMIniPath =
            std::filesystem::path("Data") / "MCM" / "Settings" / "Armor Resist Magic.ini";

        int ClampLogLevel(int level) { return std::clamp(level, 0, 2); }

        void ApplyLogLevel(int level)
        {
            level = ClampLogLevel(level);
            switch (level) {
            case 0:  spdlog::set_level(spdlog::level::off); break;
            case 2:
                spdlog::set_level(spdlog::level::trace);
                spdlog::flush_on(spdlog::level::trace);
                break;
            default:
                spdlog::set_level(spdlog::level::info);
                spdlog::flush_on(spdlog::level::info);
                break;
            }
        }

        bool IsMCMHelperLoaded()
        {
            static bool result = false;
            if (!result)
                result = (GetModuleHandleA("MCMHelper.dll") != nullptr);
            return result;
        }

        RE::TESGlobal* LookupGlobal(RE::FormID a_formID)
        {
            RE::TESDataHandler* dh = RE::TESDataHandler::GetSingleton();
            return dh ? dh->LookupForm<RE::TESGlobal>(a_formID, "Armor Resist Magic.esp") : nullptr;
        }

        bool ReadBoolGlobal(RE::FormID a_formID, bool a_default)
        {
            RE::TESGlobal* g = LookupGlobal(a_formID);
            return g ? (g->value >= 1.0f) : a_default;
        }

        float ReadFloatGlobal(RE::FormID a_formID, float a_default)
        {
            RE::TESGlobal* g = LookupGlobal(a_formID);
            return g ? g->value : a_default;
        }

        int ReadIntGlobal(RE::FormID a_formID, int a_default)
        {
            RE::TESGlobal* g = LookupGlobal(a_formID);
            return g ? static_cast<int>(g->value) : a_default;
        }

        bool ReadBoolINI(CSimpleIniA& ini, const char* key, bool def)
        {
            return ini.GetBoolValue("Main", key, def);
        }

        float ReadFloatINI(CSimpleIniA& ini, const char* key, float def)
        {
            return static_cast<float>(ini.GetDoubleValue("Main", key, static_cast<double>(def)));
        }

        int ReadIntINI(CSimpleIniA& ini, const char* key, int def)
        {
            return static_cast<int>(ini.GetLongValue("Main", key, static_cast<long>(def)));
        }

        Data ReadFromINI(CSimpleIniA& ini)
        {
            Data d{};
            d.enabled = ReadBoolINI(ini, "bEnabled", true);
            d.playerOnly = ReadBoolINI(ini, "bPlayerOnly", false);
            d.differentNPCs = ReadBoolINI(ini, "bDifferentNPCs", false);
            d.mrPlayer = ReadFloatINI(ini, "fMRPlayer", 40.0f);
            d.maxDRPlayer = ReadFloatINI(ini, "fMaxDRPlayer", 567.0f);
            d.mrNPC = ReadFloatINI(ini, "fMRNPC", 40.0f);
            d.maxDRNPC = ReadFloatINI(ini, "fMaxDRNPC", 567.0f);
            d.logLevel = ClampLogLevel(ReadIntINI(ini, "iLogLevel", 1));
            d.materialBonusEnabled = ReadBoolINI(ini, "bMaterialBonusEnabled", true);
            d.materialBonusPlayerOnly = ReadBoolINI(ini, "bMaterialBonusPlayerOnly", false);
            d.materialBonusDifferentNPCs = ReadBoolINI(ini, "bMaterialBonusDifferentNPCs", false);
            d.advancedDebug = ReadBoolINI(ini, "bAdvancedDebug", false);
            d.npcRepair = ReadBoolINI(ini, "bNPCRepair", false);
            d.playerRepair = ReadBoolINI(ini, "bPlayerRepair", false);
            d.rrArmorCompat = ReadBoolINI(ini, "bRRArmorCompat", false);
            d.rrMRCompat = ReadBoolINI(ini, "bRRMRCompat", false);
            return d;
        }

        Data ReadFromGlobals()
        {
            Data d{};
            d.enabled = ReadBoolGlobal(0x805, true);
            d.differentNPCs = ReadBoolGlobal(0x807, false);
            d.playerOnly = false;
            d.mrPlayer = ReadFloatGlobal(0x804, 40.0f);
            d.maxDRPlayer = ReadFloatGlobal(0x806, 567.0f);
            d.mrNPC = ReadFloatGlobal(0x809, 40.0f);
            d.maxDRNPC = ReadFloatGlobal(0x808, 567.0f);
            d.logLevel = ClampLogLevel(ReadIntGlobal(0x1A8, 1));
            d.materialBonusEnabled = ReadBoolGlobal(0x1A9, true);
            d.materialBonusPlayerOnly = ReadBoolGlobal(0x1AA, false);
            d.materialBonusDifferentNPCs = ReadBoolGlobal(0x1D9, false);
            d.advancedDebug = false;
            d.npcRepair = ReadBoolGlobal(0x1DA, false);
            d.playerRepair = ReadBoolGlobal(0x1DB, false);
            d.rrArmorCompat = ReadBoolGlobal(0x1DC, false);
            d.rrMRCompat = ReadBoolGlobal(0x1DD, false);
            return d;
        }

        bool AreEqual(const Data& a, const Data& b)
        {
            return
                a.enabled == b.enabled &&
                a.playerOnly == b.playerOnly &&
                a.differentNPCs == b.differentNPCs &&
                std::abs(a.mrPlayer - b.mrPlayer) < 0.001f &&
                std::abs(a.maxDRPlayer - b.maxDRPlayer) < 0.001f &&
                std::abs(a.mrNPC - b.mrNPC) < 0.001f &&
                std::abs(a.maxDRNPC - b.maxDRNPC) < 0.001f &&
                a.logLevel == b.logLevel &&
                a.materialBonusEnabled == b.materialBonusEnabled &&
                a.materialBonusPlayerOnly == b.materialBonusPlayerOnly &&
                a.materialBonusDifferentNPCs == b.materialBonusDifferentNPCs &&
                a.advancedDebug == b.advancedDebug &&
                a.npcRepair == b.npcRepair &&
                a.playerRepair == b.playerRepair &&
                a.rrArmorCompat == b.rrArmorCompat &&
                a.rrMRCompat == b.rrMRCompat;
        }

        Data ReadAll()
        {
            if (IsMCMHelperLoaded()) {
                CSimpleIniA ini;
                ini.SetUnicode();
                if (ini.LoadFile(kMCMIniPath.string().c_str()) >= 0)
                    return ReadFromINI(ini);
            }
            return ReadFromGlobals();
        }
    } // anonymoose

    // Public 

    RRParams LoadRRParams()
    {
        RRParams p{};

        CSimpleIniA ini;
        ini.SetUnicode();
        const std::string path = (std::filesystem::path("Data") /
            "MCM" / "Settings" / "ResistancesRescaled.ini").string();

        if (ini.LoadFile(path.c_str()) < 0) {
            logger::info("RR INI not found at {} — RR compat will use defaults", path);
            return p;
        }

        p.armorFormula = static_cast<int>(ini.GetLongValue("Armor", "iFormula", 0));
        p.armorAt0 = static_cast<float>(ini.GetDoubleValue("Armor", "iAt0", 0.0));
        p.armorAt1000 = static_cast<float>(ini.GetDoubleValue("Armor", "iAt1000", 75.0));
        p.magicFormula = static_cast<int>(ini.GetLongValue("Magic", "iFormula", 0));
        p.magicAt0 = static_cast<float>(ini.GetDoubleValue("Magic", "iAt0", 0.0));
        p.magicAt100 = static_cast<float>(ini.GetDoubleValue("Magic", "iAt100", 75.0));
        p.loaded = true;

        logger::trace("RR compat: loaded params - "
            "armor formula={} at0={:.1f} at1000={:.1f} | "
            "magic formula={} at0={:.1f} at100={:.1f}",
            p.armorFormula, p.armorAt0, p.armorAt1000,
            p.magicFormula, p.magicAt0, p.magicAt100);
        return p;
    }

    bool Load()
    {
        const Data prev = g_settings;
        g_settings = ReadAll();
        const bool changed = !g_loaded || !AreEqual(prev, g_settings);

        if (!g_loaded || prev.logLevel != g_settings.logLevel)
            ApplyLogLevel(g_settings.logLevel);

        if (changed)
            logger::trace("Settings loaded/changed.");

        g_loaded = true;
        return changed;
    }

    const Data& Get() { return g_settings; }

} 