static_assert(true, "ArmorScaling.cpp is compiling");

#include "ArmorScaling.h"
#include "Settings.h"

#include <SKSE/SKSE.h>

#include <RE/A/ActiveEffect.h>
#include <RE/A/Actor.h>
#include <RE/A/ActorValueOwner.h>
#include <RE/E/EffectSetting.h>
#include <RE/M/MagicTarget.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/P/ProcessLists.h>
#include <RE/S/SpellItem.h>
#include <RE/T/TESDataHandler.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESObjectARMO.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <spdlog/spdlog.h>

namespace ARMOR_SCALING
{
    struct MaterialRule
    {
        std::string      name;
        std::string      pluginName;
        RE::FormID       localFormID = 0;
        RE::BGSKeyword* keyword = nullptr;
        float            playerMult = 1.0f;
        float            npcMult = 1.0f;
        std::string      sourceFile;
    };

    struct ScalingData
    {
        float           lastAppliedMR = 0.0f;
        float           lastSeenDR = -1.0f;
        RE::ActorHandle handle;
    };

    struct SavedScalingData
    {
        RE::FormID formID = 0;
        float      lastAppliedMR = 0.0f;
        float      lastSeenDR = -1.0f;
    };

    bool g_systemReady = false;
    bool g_bulkRefreshing = false;
    bool g_npcRepairPassRunning = false;
    bool g_npcRepairWasEnabled = false;
    bool g_playerRepairWasEnabled = false;
    bool g_playerRepairedThisActivation = false;

    std::mutex                       g_updatingMutex;
    std::unordered_set<RE::FormID>   g_currentlyUpdating;

    std::mutex                                  g_dataMutex;
    std::unordered_map<RE::FormID, ScalingData> g_actorData;

    std::unordered_map<RE::FormID, bool>        g_repairedNpcThisActivation;
    std::vector<MaterialRule>                   g_materialRules;

    RE::SpellItem* g_scalingSpell = nullptr;
    SETTINGS::RRParams g_rrParams{};

    namespace
    {
        constexpr float kEpsilon = 0.01f;

        inline const std::filesystem::path kConfigDir =
            std::filesystem::path("Data") / "SKSE" / "Plugins";
        inline const std::filesystem::path kMaterialsDir =
            kConfigDir / "ArmorResistsMagic" / "Materials";

        constexpr std::uint32_t kSerializationVersion = 1;
        constexpr std::uint32_t kRecordType = 'ARMR';


        std::string Trim(std::string_view text)
        {
            std::size_t start = 0;
            while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])))
                ++start;
            std::size_t end = text.size();
            while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])))
                --end;
            return std::string(text.substr(start, end - start));
        }

        std::string ToLower(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        bool StartsWith(std::string_view text, std::string_view prefix)
        {
            return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
        }

        std::optional<float> ParseFloat(std::string_view text)
        {
            const std::string trimmed = Trim(text);
            if (trimmed.empty()) return std::nullopt;
            try { return std::stof(trimmed); }
            catch (...) { return std::nullopt; }
        }

        std::optional<RE::FormID> ParseHexFormIDRaw(std::string_view text)
        {
            std::string trimmed = Trim(text);
            if (trimmed.empty()) return std::nullopt;
            if (StartsWith(trimmed, "0x") || StartsWith(trimmed, "0X"))
                trimmed = trimmed.substr(2);
            RE::FormID  value = 0;
            const char* begin = trimmed.data();
            const char* end = trimmed.data() + trimmed.size();
            const auto  result = std::from_chars(begin, end, value, 16);
            if (result.ec != std::errc{} || result.ptr != end) return std::nullopt;
            return value;
        }

        RE::FormID NormalizeFormIDForLookup(RE::FormID formID)
        {
            if ((formID & 0xFF000000u) == 0xFE000000u) return formID & 0x00000FFFu;
            if (formID > 0x00FFFFFFu)                  return formID & 0x00FFFFFFu;
            return formID;
        }

        // Actor value helpers-

        RE::ActorValueOwner* GetActorValueOwner(RE::Actor* actor)
        {
            return actor ? actor->AsActorValueOwner() : nullptr;
        }

        RE::MagicTarget* GetMagicTarget(RE::Actor* actor)
        {
            return actor ? actor->AsMagicTarget() : nullptr;
        }

        float GetDamageResist(RE::Actor* actor)
        {
            RE::ActorValueOwner* avo = GetActorValueOwner(actor);
            return avo ? avo->GetClampedActorValue(RE::ActorValue::kDamageResist) : 0.0f;
        }

        void ModResistMagicTemporary(RE::Actor* actor, float delta)
        {
            RE::ActorValueOwner* avo = GetActorValueOwner(actor);
            if (!avo || std::abs(delta) <= kEpsilon) return;
            avo->RestoreActorValue(
                RE::ACTOR_VALUE_MODIFIER::kTemporary,
                RE::ActorValue::kResistMagic,
                delta);
        }

        std::unordered_set<RE::EffectSetting*> GetOwnSpellEffectSettings()
        {
            std::unordered_set<RE::EffectSetting*> result;
            if (!g_scalingSpell) return result;
            for (auto* item : g_scalingSpell->effects)
                if (item && item->baseEffect)
                    result.insert(item->baseEffect);
            return result;
        }

        void UpdatePlayerInfoEffectMagnitude(RE::Actor* actor, float magnitude)
        {
            if (!actor || !actor->IsPlayerRef() || !g_scalingSpell) return;
            if (g_scalingSpell->effects.empty()) return;

            RE::EffectSetting* infoEffect = g_scalingSpell->effects[0]
                ? g_scalingSpell->effects[0]->baseEffect
                : nullptr;
            if (!infoEffect) return;

            RE::MagicTarget* magicTarget = GetMagicTarget(actor);
            if (!magicTarget) return;

            struct InfoEffectVisitor final : RE::MagicTarget::ForEachActiveEffectVisitor
            {
                RE::EffectSetting* targetEffect;
                float              newMagnitude;
                InfoEffectVisitor(RE::EffectSetting* e, float m)
                    : targetEffect(e), newMagnitude(m) {
                }
                RE::BSContainer::ForEachResult Accept(RE::ActiveEffect* effect) override
                {
                    if (effect && effect->effect->baseEffect == targetEffect)
                        effect->magnitude = newMagnitude;
                    return RE::BSContainer::ForEachResult::kContinue;
                }
            };

            InfoEffectVisitor visitor(infoEffect, magnitude);
            magicTarget->VisitEffects(visitor);
        }


        bool UseNPCGlobalsForActor(RE::Actor* actor)
        {
            return actor && !actor->IsPlayerRef() && SETTINGS::Get().differentNPCs;
        }

        bool UseNPCMaterialValuesForActor(RE::Actor* actor)
        {
            return actor && !actor->IsPlayerRef() && SETTINGS::Get().materialBonusDifferentNPCs;
        }

        bool ShouldUseMaterialBonuses(RE::Actor* actor)
        {
            if (!actor) return false;
            const SETTINGS::Data& s = SETTINGS::Get();
            if (!s.materialBonusEnabled) return false;
            if (s.materialBonusPlayerOnly && !actor->IsPlayerRef()) return false;
            return true;
        }

        float RRForwardMR(float vanillaMR)
        {
            const SETTINGS::RRParams& p = g_rrParams;
            const float at100 = p.magicAt100;
            if (at100 <= kEpsilon) return vanillaMR;

            if (p.magicFormula == 1) {
                const float k = -std::log(1.0f - at100 / 100.0f) / 100.0f;
                return 100.0f * (1.0f - std::exp(-k * vanillaMR));
            }
            else {
                const float c = 100.0f * (100.0f - at100) / at100;
                return 100.0f * vanillaMR / (vanillaMR + c);
            }
        }

        float RRInverseMR(float rescaledMR)
        {
            const SETTINGS::RRParams& p = g_rrParams;
            const float at100 = p.magicAt100;
            if (at100 <= kEpsilon) return rescaledMR;
.
            const float y = (std::min)(rescaledMR, 99.9f);
            if (y <= 0.0f) return 0.0f;

            if (p.magicFormula == 1) {
                const float k = -std::log(1.0f - at100 / 100.0f) / 100.0f;
                return -std::log(1.0f - y / 100.0f) / k;
            }
            else {
                const float c = 100.0f * (100.0f - at100) / at100;
                return c * y / (100.0f - y);
            }
        }

        float RRForwardArmor(float armorRating)
        {
            const SETTINGS::RRParams& p = g_rrParams;
            const float at1000 = p.armorAt1000;
            if (at1000 <= kEpsilon) return armorRating;

            if (p.armorFormula == 1) {
                const float k = -std::log(1.0f - at1000 / 100.0f) / 1000.0f;
                return 100.0f * (1.0f - std::exp(-k * armorRating));
            }
            else {
                const float c = 1000.0f * (100.0f - at1000) / at1000;
                return 100.0f * armorRating / (armorRating + c);
            }
        }

        float RRInverseArmor(float drPct)
        {
            const SETTINGS::RRParams& p = g_rrParams;
            const float at1000 = p.armorAt1000;
            if (at1000 <= kEpsilon) return drPct;

            const float y = (std::min)(drPct, 99.9f);
            if (y <= 0.0f) return 0.0f;

            if (p.armorFormula == 1) {
                const float k = -std::log(1.0f - at1000 / 100.0f) / 1000.0f;
                return -std::log(1.0f - y / 100.0f) / k;
            }
            else {
                const float c = 1000.0f * (100.0f - at1000) / at1000;
                return c * y / (100.0f - y);
            }
        }

        float GetInfoSpellMagnitude(float vanillaBase, float rawBonus)
        {
            const float rescaledWithBonus = RRForwardMR(vanillaBase + rawBonus);
            const float rescaledWithoutBonus = RRForwardMR(vanillaBase);
            return (std::max)(rescaledWithBonus - rescaledWithoutBonus, 0.0f);
        }

        // MR breakdown

        float GetBaseMR(RE::Actor* actor)
        {
            RE::ActorValueOwner* avo = GetActorValueOwner(actor);
            return avo ? avo->GetBaseActorValue(RE::ActorValue::kResistMagic) : 0.0f;
        }

        float GetThirdPartyActiveEffectMR(RE::Actor* actor)
        {
            if (!actor) return 0.0f;
            RE::MagicTarget* mt = GetMagicTarget(actor);
            if (!mt) return 0.0f;

            const auto ownEffects = GetOwnSpellEffectSettings();

            if (g_scalingSpell) {
                logger::trace("  [MR AUDIT] Own spell: '{}' FormID={:08X} has {} effect slot(s):",
                    g_scalingSpell->GetName(),
                    static_cast<std::uint32_t>(g_scalingSpell->GetFormID()),
                    g_scalingSpell->effects.size());
                for (std::size_t i = 0; i < g_scalingSpell->effects.size(); ++i) {
                    auto* item = g_scalingSpell->effects[i];
                    if (item && item->baseEffect)
                        logger::trace("    slot[{}] effect='{}' FormID={:08X} primaryAV={}",
                            i, item->baseEffect->GetName(),
                            static_cast<std::uint32_t>(item->baseEffect->GetFormID()),
                            static_cast<int>(item->baseEffect->data.primaryAV));
                }
            }

            float total = 0.0f;

            struct Visitor final : RE::MagicTarget::ForEachActiveEffectVisitor
            {
                float& total;
                const std::unordered_set<RE::EffectSetting*>& ownEffects;
                Visitor(float& t, const std::unordered_set<RE::EffectSetting*>& own)
                    : total(t), ownEffects(own) {
                }

                RE::BSContainer::ForEachResult Accept(RE::ActiveEffect* effect) override
                {
                    if (!effect) return RE::BSContainer::ForEachResult::kContinue;
                    if (!effect->effect) return RE::BSContainer::ForEachResult::kContinue;
                    RE::EffectSetting* base = effect->effect->baseEffect;
                    if (!base) return RE::BSContainer::ForEachResult::kContinue;

                    using Flag = RE::ActiveEffect::Flag;
                    const bool isOwnEffect = ownEffects.count(base) > 0;
                    const bool affectsMR = base->data.primaryAV == RE::ActorValue::kResistMagic;
                    const bool isInactive = effect->flags.any(Flag::kInactive);

                    std::string sourceName = "(unknown source)";
                    if (effect->spell)
                        sourceName = std::format("spell='{}'({:08X})",
                            effect->spell->GetName(),
                            static_cast<std::uint32_t>(effect->spell->GetFormID()));

                    if (isOwnEffect) {
                        logger::trace("  [MR AUDIT] SKIP (own spell) effect='{}' FormID={:08X} "
                            "magnitude={:.2f} primaryAV={} {}",
                            base->GetName(), static_cast<std::uint32_t>(base->GetFormID()),
                            effect->magnitude, static_cast<int>(base->data.primaryAV), sourceName);
                    }
                    else if (isInactive) {
                        logger::trace("  [MR AUDIT] SKIP (inactive) effect='{}' FormID={:08X} "
                            "magnitude={:.2f} {}",
                            base->GetName(), static_cast<std::uint32_t>(base->GetFormID()),
                            effect->magnitude, sourceName);
                    }
                    else if (affectsMR) {
                        logger::trace("  [MR AUDIT] COUNT effect='{}' FormID={:08X} "
                            "magnitude={:.2f} {}",
                            base->GetName(), static_cast<std::uint32_t>(base->GetFormID()),
                            effect->magnitude, sourceName);
                        total += effect->magnitude;
                    }
                    else {
                        logger::trace("  [MR AUDIT] IGNORE (not MR) effect='{}' FormID={:08X} "
                            "magnitude={:.2f} primaryAV={} {}",
                            base->GetName(), static_cast<std::uint32_t>(base->GetFormID()),
                            effect->magnitude, static_cast<int>(base->data.primaryAV), sourceName);
                    }
                    return RE::BSContainer::ForEachResult::kContinue;
                }
            };

            Visitor v(total, ownEffects);
            mt->VisitEffects(v);

            logger::trace("  [MR AUDIT] ThirdPartyActiveEffectMR total={:.2f}", total);
            return total;
        }

        void ShowPlayerMRBreakdownNotification(RE::Actor* player, float armorScalingBonus)
        {
            if (!player) return;
            const float baseMR = GetBaseMR(player);
            const float effectsMR = GetThirdPartyActiveEffectMR(player);
            const float legitMR = baseMR + effectsMR;
            const float fullMR = legitMR + armorScalingBonus;
            const std::string msg = std::format(
                "MR Breakdown: Base: {:.0f} | +Effects: {:.0f} = {:.0f} | +Armor: {:.0f} = {:.0f}",
                baseMR, effectsMR, legitMR, armorScalingBonus, fullMR);
            RE::DebugNotification(msg.c_str());
            logger::info("Player MR repair breakdown: base={:.1f} effects={:.1f} "
                "legit={:.1f} armorBonus={:.1f} full={:.1f}",
                baseMR, effectsMR, legitMR, armorScalingBonus, fullMR);
        }

        // Material Rules

        std::vector<MaterialRule> LoadMaterialRulesFromIniFiles(RE::TESDataHandler* dataHandler)
        {
            std::vector<MaterialRule> rules;

            std::error_code ec;
            std::filesystem::create_directories(kMaterialsDir, ec);

            if (!std::filesystem::exists(kMaterialsDir)) {
                logger::error("Materials directory does not exist: {}", kMaterialsDir.string());
                return rules;
            }

            std::vector<std::filesystem::path> iniFiles;
            for (const auto& entry : std::filesystem::directory_iterator(kMaterialsDir, ec)) {
                if (ec) {
                    logger::error("Failed to iterate material rules directory: {}", ec.message());
                    return rules;
                }
                if (!entry.is_regular_file()) continue;
                if (ToLower(entry.path().extension().string()) == ".ini")
                    iniFiles.push_back(entry.path());
            }

            std::sort(iniFiles.begin(), iniFiles.end());
            std::unordered_map<std::string, std::size_t> dedupeIndex;

            for (const auto& iniPath : iniFiles) {
                std::ifstream file(iniPath);
                if (!file.is_open()) {
                    logger::error("Failed to open material rules file: {}", iniPath.string());
                    continue;
                }

                logger::trace("Loading material rules from {}", iniPath.string());

                MaterialRule currentRule{};
                bool         inSection = false;
                std::size_t  lineNo = 0;

                auto flushCurrentRule = [&]() {
                    if (!inSection) return;
                    if (currentRule.name.empty()) {
                        inSection = false; currentRule = MaterialRule{}; return;
                    }
                    if (currentRule.pluginName.empty()) {
                        logger::warn("Skipping material rule '{}' in {} because plugin is missing",
                            currentRule.name, iniPath.string());
                        inSection = false; currentRule = MaterialRule{}; return;
                    }
                    if (currentRule.localFormID == 0) {
                        logger::warn("Skipping material rule '{}' in {} because form is missing or zero",
                            currentRule.name, iniPath.string());
                        inSection = false; currentRule = MaterialRule{}; return;
                    }

                    currentRule.sourceFile = iniPath.filename().string();
                    currentRule.keyword = dataHandler->LookupForm<RE::BGSKeyword>(
                        currentRule.localFormID, currentRule.pluginName);

                    if (!currentRule.keyword) {
                        logger::warn("Could not resolve material keyword '{}' -> {}|{:X} from {}",
                            currentRule.name, currentRule.pluginName,
                            static_cast<std::uint32_t>(currentRule.localFormID), iniPath.string());
                        inSection = false; currentRule = MaterialRule{}; return;
                    }

                    const std::string dedupeKey =
                        ToLower(currentRule.pluginName) + "|" + std::to_string(currentRule.localFormID);

                    auto it = dedupeIndex.find(dedupeKey);
                    if (it != dedupeIndex.end()) {
                        logger::trace("Duplicate material rule for {}|{:X}; later file overrides earlier rule",
                            currentRule.pluginName,
                            static_cast<std::uint32_t>(currentRule.localFormID));
                        rules[it->second] = currentRule;
                    }
                    else {
                        dedupeIndex.emplace(dedupeKey, rules.size());
                        rules.push_back(currentRule);
                    }

                    inSection = false; currentRule = MaterialRule{};
                    };

                std::string line;
                while (std::getline(file, line)) {
                    ++lineNo;
                    const std::string trimmed = Trim(line);
                    if (trimmed.empty()) continue;
                    if (StartsWith(trimmed, ";") || StartsWith(trimmed, "#")) continue;

                    if (trimmed.front() == '[' && trimmed.back() == ']') {
                        flushCurrentRule();
                        currentRule = MaterialRule{};
                        currentRule.name = Trim(std::string_view(trimmed).substr(1, trimmed.size() - 2));
                        inSection = true;
                        continue;
                    }

                    if (!inSection) {
                        logger::warn("Ignoring line {} in {} because it is outside a section",
                            lineNo, iniPath.string());
                        continue;
                    }

                    const std::size_t equalsPos = trimmed.find('=');
                    if (equalsPos == std::string::npos) {
                        logger::warn("Ignoring malformed line {} in {}: {}",
                            lineNo, iniPath.string(), trimmed);
                        continue;
                    }

                    const std::string key = ToLower(Trim(std::string_view(trimmed).substr(0, equalsPos)));
                    const std::string value = Trim(std::string_view(trimmed).substr(equalsPos + 1));

                    if (key == "plugin") {
                        currentRule.pluginName = value;
                    }
                    else if (key == "form") {
                        const auto rawParsed = ParseHexFormIDRaw(value);
                        if (!rawParsed.has_value())
                            logger::warn("Invalid form value '{}' in section '{}' from {}",
                                value, currentRule.name, iniPath.string());
                        else
                            currentRule.localFormID = NormalizeFormIDForLookup(*rawParsed);
                    }
                    else if (key == "mult") {
                        const auto parsed = ParseFloat(value);
                        if (!parsed.has_value())
                            logger::warn("Invalid mult value '{}' in section '{}' from {}",
                                value, currentRule.name, iniPath.string());
                        else { currentRule.playerMult = *parsed; currentRule.npcMult = *parsed; }
                    }
                    else if (key == "player_mult") {
                        const auto parsed = ParseFloat(value);
                        if (!parsed.has_value())
                            logger::warn("Invalid player_mult value '{}' in section '{}' from {}",
                                value, currentRule.name, iniPath.string());
                        else currentRule.playerMult = *parsed;
                    }
                    else if (key == "npc_mult") {
                        const auto parsed = ParseFloat(value);
                        if (!parsed.has_value())
                            logger::warn("Invalid npc_mult value '{}' in section '{}' from {}",
                                value, currentRule.name, iniPath.string());
                        else currentRule.npcMult = *parsed;
                    }
                    else {
                        logger::warn("Unknown key '{}' in section '{}' from {}",
                            key, currentRule.name, iniPath.string());
                    }
                }

                flushCurrentRule();
            }

            logger::info("Loaded {} material rule(s) from INI files", rules.size());
            return rules;
        }

        // Armor contribution / effective DR

        float GetArmorMaterialMultiplier(RE::Actor* actor, RE::TESObjectARMO* armor)
        {
            if (!actor || !armor) return 1.0f;

            const bool    useNPC = UseNPCMaterialValuesForActor(actor);
            float         totalMult = 0.0f;
            std::uint32_t matchCount = 0;

            for (const auto& rule : g_materialRules) {
                if (rule.keyword && armor->HasKeyword(rule.keyword)) {
                    const float mult = useNPC ? rule.npcMult : rule.playerMult;
                    totalMult += mult;
                    ++matchCount;
                    logger::trace("Matched material rule '{}' on actor {:08X}, multiplier={:.3f}",
                        rule.name.c_str(), static_cast<std::uint32_t>(actor->GetFormID()), mult);
                }
            }

            if (matchCount == 0) return 1.0f;
            return totalMult / static_cast<float>(matchCount);
        }

        struct ArmorContributionTotals
        {
            float vanillaEquippedArmorDR = 0.0f;
            float adjustedEquippedArmorDR = 0.0f;
        };

        bool ActorHasValidInventory(RE::Actor* actor)
        {
            if (!actor) return false;
            const auto* base = actor->GetBaseObject();
            if (!base) return false;
            return base->GetFormType() == RE::FormType::NPC;
        }

        ArmorContributionTotals GetEquippedArmorContributionTotals(RE::Actor* actor)
        {
            ArmorContributionTotals totals{};
            if (!actor) return totals;
            if (!ActorHasValidInventory(actor)) return totals;

            const auto inventory = actor->GetInventory();
            for (const auto& [boundObject, invDataPair] : inventory) {
                if (!boundObject) continue;
                RE::TESObjectARMO* armor = boundObject->As<RE::TESObjectARMO>();
                if (!armor) continue;
                const auto& [count, entry] = invDataPair;
                if (count <= 0 || !entry || !entry->IsWorn()) continue;
                const float pieceArmor = armor->GetArmorRating();
                if (pieceArmor <= 0.0f) continue;
                const float multiplier = GetArmorMaterialMultiplier(actor, armor);
                totals.vanillaEquippedArmorDR += pieceArmor;
                totals.adjustedEquippedArmorDR += pieceArmor * multiplier;
            }
            return totals;
        }

        float CalculateEffectiveArmorForMR(RE::Actor* actor, float maxDR)
        {
            if (!actor) return 0.0f;

            const SETTINGS::Data& s = SETTINGS::Get();

            float baseArmor = GetDamageResist(actor);

            // RR armor rescaling is player only since RR is also player only
            if (s.rrArmorCompat && actor->IsPlayerRef()) {
                baseArmor = RRInverseArmor(baseArmor);
                logger::trace("EffectiveDR {:08X} RR armor invert: dr={:.1f} -> armor={:.1f}",
                    static_cast<std::uint32_t>(actor->GetFormID()),
                    GetDamageResist(actor), baseArmor);
            }

            if (!ShouldUseMaterialBonuses(actor))
                return std::clamp(baseArmor, -maxDR, maxDR);

            const ArmorContributionTotals totals = GetEquippedArmorContributionTotals(actor);
            float effectiveDR = baseArmor
                - totals.vanillaEquippedArmorDR
                + totals.adjustedEquippedArmorDR;
            effectiveDR = std::clamp(effectiveDR, -maxDR, maxDR);

            logger::trace("EffectiveDR {:08X} baseArmor={:.1f} vanillaArmor={:.1f} "
                "adjustedArmor={:.1f} effectiveDR={:.1f}",
                static_cast<std::uint32_t>(actor->GetFormID()), baseArmor,
                totals.vanillaEquippedArmorDR, totals.adjustedEquippedArmorDR, effectiveDR);
            return effectiveDR;
        }

    } // anonymoose

    // Public 

    bool IsBulkRefreshing() { return g_bulkRefreshing; }

    float CalculateMagnitude(RE::Actor* actor)
    {
        if (!actor) return 0.0f;

        const SETTINGS::Data& s = SETTINGS::Get();
        const bool            useNPC = UseNPCGlobalsForActor(actor);
        const float           maxDR = useNPC ? s.maxDRNPC : s.maxDRPlayer;
        const float           maxMR = useNPC ? s.mrNPC : s.mrPlayer;

        if (maxDR <= 0.0f) return 0.0f;

        const float effectiveDR = CalculateEffectiveArmorForMR(actor, maxDR);
        const float intendedBonus = (effectiveDR / maxDR) * maxMR;

        if (s.rrMRCompat && actor->IsPlayerRef()) {

            const float permMod = actor->GetActorValueModifier(
                RE::ACTOR_VALUE_MODIFIER::kPermanent, RE::ActorValue::kResistMagic);

            float lastBonus = 0.0f;
            {
                std::lock_guard<std::mutex> lock(g_dataMutex);
                auto it = g_actorData.find(actor->GetFormID());
                if (it != g_actorData.end())
                    lastBonus = it->second.lastAppliedMR;
            }

            const float cleanBase = permMod - lastBonus;
            const float finalGoal = cleanBase + intendedBonus;
            const float vanillaNeeded = RRInverseMR(finalGoal);
            const float rawBonus = std::clamp(vanillaNeeded - cleanBase, 0.0f, 10000.0f);

            logger::trace("RRMRCompat {:08X} permMod={:.1f} lastBonus={:.1f} "
                "cleanBase={:.1f} intendedBonus={:.1f} finalGoal={:.1f} "
                "vanillaNeeded={:.1f} rawBonus={:.1f}",
                static_cast<std::uint32_t>(actor->GetFormID()),
                permMod, lastBonus, cleanBase, intendedBonus, finalGoal, vanillaNeeded, rawBonus);

            return rawBonus;
        }

        return intendedBonus;
    }

    void ResetNpcRepairPassState() { g_repairedNpcThisActivation.clear(); }
    void ResetPlayerRepairPassState() { g_playerRepairedThisActivation = false; }

    // NPC repair

    void RepairSingleNpcMagicResist(RE::Actor* actor)
    {
        if (!actor || actor->IsPlayerRef()) return;

        const RE::FormID formID = actor->GetFormID();
        if (g_repairedNpcThisActivation.count(formID)) return;

        RE::ActorValueOwner* avo = GetActorValueOwner(actor);
        if (!avo) { g_repairedNpcThisActivation[formID] = true; return; }

        const float baseMR = GetBaseMR(actor);
        const float thirdPartyMR = GetThirdPartyActiveEffectMR(actor);
        const float legitimateMR = baseMR + thirdPartyMR;

        const float permMod = actor->GetActorValueModifier(
            RE::ACTOR_VALUE_MODIFIER::kPermanent, RE::ActorValue::kResistMagic);
        const float permDelta = legitimateMR - permMod;
        if (std::abs(permDelta) > kEpsilon) {
            logger::trace("NPC MR repair {:08X}: correcting permanent modifier "
                "from {:.1f} to {:.1f} (delta={:.1f})",
                static_cast<std::uint32_t>(formID), permMod, legitimateMR, permDelta);
            avo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kPermanent,
                RE::ActorValue::kResistMagic, permDelta);
        }

        const float tempMod = actor->GetActorValueModifier(
            RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kResistMagic);
        if (std::abs(tempMod) > kEpsilon) {
            logger::trace("NPC MR repair {:08X}: zeroing temporary modifier {:.1f}",
                static_cast<std::uint32_t>(formID), tempMod);
            avo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary,
                RE::ActorValue::kResistMagic, -tempMod);
        }

        {
            std::lock_guard<std::mutex> lock(g_dataMutex);
            auto it = g_actorData.find(formID);
            if (it == g_actorData.end())
                it = g_actorData.emplace(formID, ScalingData{}).first;
            ScalingData& data = it->second;
            data.handle = actor->GetHandle();
            data.lastAppliedMR = 0.0f;
            data.lastSeenDR = -1.0f;
        }

        g_repairedNpcThisActivation[formID] = true;

        logger::trace("NPC MR repair {:08X}: done. legitimateMR={:.1f}  "
            "permMod now={:.1f}  tempMod was={:.1f}",
            static_cast<std::uint32_t>(formID), legitimateMR,
            actor->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kPermanent,
                RE::ActorValue::kResistMagic),
            tempMod);
    }

    void RunNpcMagicResistRepairPass()
    {
        if (!g_systemReady) return;
        RE::ProcessLists* processLists = RE::ProcessLists::GetSingleton();
        if (!processLists) return;
        for (auto& handle : processLists->highActorHandles)
            if (auto actor = handle.get().get())
                if (!actor->IsPlayerRef())
                    RepairSingleNpcMagicResist(actor);
    }

    void UpdateNpcRepairPass()
    {
        if (g_npcRepairPassRunning) return;
        const bool enabledNow = SETTINGS::Get().npcRepair;
        if (enabledNow && !g_npcRepairWasEnabled) {
            ResetNpcRepairPassState();
            g_npcRepairPassRunning = true;
            RunNpcMagicResistRepairPass();
            g_npcRepairPassRunning = false;
        }
        else if (!enabledNow && g_npcRepairWasEnabled) {
            ResetNpcRepairPassState();
        }
        g_npcRepairWasEnabled = enabledNow;
    }

    // Player repair

    void RepairPlayerMagicResist(RE::Actor* player)
    {
        if (!player || !player->IsPlayerRef()) return;
        if (g_playerRepairedThisActivation) return;

        const RE::FormID formID = player->GetFormID();
        {
            std::lock_guard<std::mutex> lock(g_dataMutex);
            auto it = g_actorData.find(formID);
            if (it == g_actorData.end())
                it = g_actorData.emplace(formID, ScalingData{}).first;
            it->second.handle = player->GetHandle();
        }

        RE::ActorValueOwner* avo = GetActorValueOwner(player);
        if (!avo) { g_playerRepairedThisActivation = true; return; }

        logger::info("=== PLAYER MR REPAIR BEGIN ===");
        logger::trace("  MR before repair: GetActorValue={:.1f}  GetBaseActorValue={:.1f}",
            avo->GetActorValue(RE::ActorValue::kResistMagic),
            avo->GetBaseActorValue(RE::ActorValue::kResistMagic));

        const float baseMR = GetBaseMR(player);
        const float thirdPartyMR = GetThirdPartyActiveEffectMR(player);
        const float legitimateMR = baseMR + thirdPartyMR;
        logger::trace("  baseMR={:.1f}  thirdPartyMR={:.1f}  legitimateMR={:.1f}",
            baseMR, thirdPartyMR, legitimateMR);

        {
            const float permMod = player->GetActorValueModifier(
                RE::ACTOR_VALUE_MODIFIER::kPermanent, RE::ActorValue::kResistMagic);
            const float permDelta = legitimateMR - permMod;
            logger::trace("  permanent modifier={:.1f}  legitimateMR={:.1f}  delta={:.1f}",
                permMod, legitimateMR, permDelta);
            if (std::abs(permDelta) > kEpsilon) {
                logger::trace("  Step2: correcting permanent modifier by {:.1f}", permDelta);
                avo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kPermanent,
                    RE::ActorValue::kResistMagic, permDelta);
            }
            else {
                logger::trace("  Step2: permanent modifier already correct");
            }
        }

        {
            const float tempMod = player->GetActorValueModifier(
                RE::ACTOR_VALUE_MODIFIER::kTemporary, RE::ActorValue::kResistMagic);
            logger::trace("  temporary modifier (direct read)={:.1f}", tempMod);
            if (std::abs(tempMod) > kEpsilon) {
                logger::trace("  Step3: zeroing temporary modifier {:.1f}", tempMod);
                avo->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary,
                    RE::ActorValue::kResistMagic, -tempMod);
                logger::trace("  MR after temp zeroing: GetActorValue={:.1f}",
                    avo->GetActorValue(RE::ActorValue::kResistMagic));
            }
            else {
                logger::trace("  Step3: temporary modifier already zero");
            }
        }

        {
            std::lock_guard<std::mutex> lock(g_dataMutex);
            auto it = g_actorData.find(formID);
            if (it != g_actorData.end()) {
                it->second.lastAppliedMR = 0.0f;
                it->second.lastSeenDR = -1.0f;
            }
        }

        const float armorBonus = CalculateMagnitude(player);
        logger::trace("  armorBonus (next frame)={:.1f}", armorBonus);
        logger::info("=== PLAYER MR REPAIR END: legitimateMR={:.1f} armorBonus={:.1f} expected={:.1f} ===",
            legitimateMR, armorBonus, legitimateMR + armorBonus);

        auto* task = SKSE::GetTaskInterface();
        if (task) {
            task->AddTask([playerHandle = player->GetHandle(), armorBonus]() {
                if (auto* p = playerHandle.get().get())
                    ShowPlayerMRBreakdownNotification(p, armorBonus);
                });
        }

        g_playerRepairedThisActivation = true;
    }

    void UpdatePlayerRepairPass()
    {
        const bool enabledNow = SETTINGS::Get().playerRepair;
        if (enabledNow && !g_playerRepairWasEnabled) {
            ResetPlayerRepairPassState();
            RE::PlayerCharacter* player = RE::PlayerCharacter::GetSingleton();
            if (player) RepairPlayerMagicResist(player);
        }
        else if (!enabledNow && g_playerRepairWasEnabled) {
            ResetPlayerRepairPassState();
        }
        g_playerRepairWasEnabled = enabledNow;
    }

    // Runtime state

    void OnSettingsChanged() // saves resources
    {
        {
            std::lock_guard<std::mutex> lock(g_dataMutex);
            g_rrParams = SETTINGS::LoadRRParams();
        }

        UpdateNpcRepairPass();
        UpdatePlayerRepairPass();

        std::lock_guard<std::mutex> lock(g_dataMutex);
        for (auto& [id, data] : g_actorData)
            data.lastSeenDR = -1.0f;
    }

    void ClearRuntimeState()
    {
        {
            std::lock_guard<std::mutex> lock(g_updatingMutex);
            g_currentlyUpdating.clear();
        }
        {
            std::lock_guard<std::mutex> lock(g_dataMutex);
            g_actorData.clear();
        }
        g_repairedNpcThisActivation.clear();
        g_npcRepairWasEnabled = false;
        g_playerRepairWasEnabled = false;
        g_playerRepairedThisActivation = false;
    }

    // ApplyToActor

    void ApplyToActor(RE::Actor* actor, bool)
    {
        if (!g_systemReady || !actor) return;

        const RE::FormID formID = actor->GetFormID();

        {
            std::lock_guard<std::mutex> lock(g_updatingMutex);
            if (g_currentlyUpdating.count(formID)) return;
            g_currentlyUpdating.insert(formID);
        }

        struct UpdateGuard {
            RE::FormID id;
            ~UpdateGuard() {
                std::lock_guard<std::mutex> lock(g_updatingMutex);
                g_currentlyUpdating.erase(id);
            }
        } guard{ formID };

        const SETTINGS::Data& s = SETTINGS::Get();

        if (s.npcRepair && !actor->IsPlayerRef() && !g_npcRepairPassRunning)
            RepairSingleNpcMagicResist(actor);

        if (!s.enabled) {
            RemoveFromActor(actor);
            return;
        }

        if (s.playerOnly && !actor->IsPlayerRef()) return;

        const float currentDR = GetDamageResist(actor);

        float oldMR = 0.0f;
        bool  needsWork = false;

        {
            std::lock_guard<std::mutex> lock(g_dataMutex);
            auto it = g_actorData.find(formID);
            if (it == g_actorData.end())
                it = g_actorData.emplace(formID, ScalingData{}).first;

            ScalingData& data = it->second;
            data.handle = actor->GetHandle();
            oldMR = data.lastAppliedMR;

            if (std::abs(currentDR - data.lastSeenDR) >= kEpsilon) {
                data.lastSeenDR = currentDR;
                needsWork = true;
            }
        }

        if (!needsWork) return;

        if (actor->IsPlayerRef() && g_scalingSpell && !actor->HasSpell(g_scalingSpell))
            actor->AddSpell(g_scalingSpell);

        const float newMR = CalculateMagnitude(actor);
        const float mrDelta = newMR - oldMR;

        if (std::abs(mrDelta) >= kEpsilon) {
            logger::trace("ApplyToActor {:08X} DR={:.1f} oldMR={:.1f} newMR={:.1f} delta={:.1f}",
                static_cast<std::uint32_t>(formID), currentDR, oldMR, newMR, mrDelta);

            ModResistMagicTemporary(actor, mrDelta);

            std::lock_guard<std::mutex> lock(g_dataMutex);
            auto it = g_actorData.find(formID);
            if (it != g_actorData.end())
                it->second.lastAppliedMR = newMR;
        }

        if (actor->IsPlayerRef()) {
            // When RR MR compat is on, newMR is the inflated raw bonus.
            // The info spell should show the intended unrescaled bonus instead.
            float displayMR = newMR;
            if (s.rrMRCompat) {
                const SETTINGS::Data& sd = SETTINGS::Get();
                const bool   useNPC = UseNPCGlobalsForActor(actor);
                const float  maxDR = useNPC ? sd.maxDRNPC : sd.maxDRPlayer;
                const float  maxMR = useNPC ? sd.mrNPC : sd.mrPlayer;
                const float  effDR = CalculateEffectiveArmorForMR(actor, maxDR);
                displayMR = (maxDR > 0.0f) ? (effDR / maxDR) * maxMR : 0.0f;
            }
            UpdatePlayerInfoEffectMagnitude(actor, displayMR);
        }
    }

    void RemoveFromActor(RE::Actor* actor)
    {
        if (!actor) return;

        float appliedMR = 0.0f;
        {
            std::lock_guard<std::mutex> lock(g_dataMutex);
            auto it = g_actorData.find(actor->GetFormID());
            if (it != g_actorData.end()) {
                appliedMR = it->second.lastAppliedMR;
                g_actorData.erase(it);
            }
        }

        if (std::abs(appliedMR) > kEpsilon)
            ModResistMagicTemporary(actor, -appliedMR);

        if (actor->IsPlayerRef()) UpdatePlayerInfoEffectMagnitude(actor, 0.0f);

        if (g_scalingSpell && actor->HasSpell(g_scalingSpell))
            actor->RemoveSpell(g_scalingSpell);
    }

    void RemoveAllScaling()
    {
        if (!g_systemReady) return;
        g_systemReady = false;

        std::unordered_map<RE::FormID, ScalingData> snapshot;
        {
            std::lock_guard<std::mutex> lock(g_dataMutex);
            snapshot = g_actorData;
            g_actorData.clear();
        }

        for (auto& [id, data] : snapshot) {
            if (std::abs(data.lastAppliedMR) <= kEpsilon) continue;
            if (auto actor = data.handle.get().get()) {
                ModResistMagicTemporary(actor, -data.lastAppliedMR);
                if (actor->IsPlayerRef()) UpdatePlayerInfoEffectMagnitude(actor, 0.0f);
            }
        }

        logger::info("Removed all armor scaling before save/load");
        g_systemReady = true;
    }

    void RefreshAllActors()
    {
        if (!g_systemReady) return;

        SETTINGS::Load();
        UpdateNpcRepairPass();
        UpdatePlayerRepairPass();

        RE::ProcessLists* processLists = RE::ProcessLists::GetSingleton();
        if (!processLists) return;

        g_bulkRefreshing = true;

        if (RE::PlayerCharacter* player = RE::PlayerCharacter::GetSingleton())
            ApplyToActor(player, true);

        for (auto& handle : processLists->highActorHandles)
            if (auto actor = handle.get().get())
                if (!actor->IsPlayerRef())
                    ApplyToActor(actor, false);

        g_bulkRefreshing = false;
    }

    void CleanupActor(RE::Actor* actor) { RemoveFromActor(actor); }

    // Serialization

    void Save(SKSE::SerializationInterface* a_intfc)
    {
        if (!a_intfc) return;

        std::vector<SavedScalingData> records;
        {
            std::lock_guard<std::mutex> lock(g_dataMutex);
            records.reserve(g_actorData.size());
            for (const auto& [formID, data] : g_actorData) {
                SavedScalingData rec;
                rec.formID = formID;
                rec.lastAppliedMR = data.lastAppliedMR;
                rec.lastSeenDR = data.lastSeenDR;
                records.push_back(rec);
            }
        }

        const std::uint32_t count = static_cast<std::uint32_t>(records.size());

        if (!a_intfc->OpenRecord(kRecordType, kSerializationVersion)) {
            logger::critical("Failed to open scaling serialization record"); return;
        }
        if (!a_intfc->WriteRecordData(&count, sizeof(count))) {
            logger::critical("Failed to write scaling record count"); return;
        }
        for (const auto& rec : records) {
            if (!a_intfc->WriteRecordData(&rec, sizeof(rec))) {
                logger::critical("Failed to write scaling record for {:08X}", rec.formID); return;
            }
        }

        logger::info("Saved {} armor scaling records", count);
    }

    void Load(SKSE::SerializationInterface* a_intfc)
    {
        if (!a_intfc) return;

        ClearRuntimeState();

        std::uint32_t type = 0, version = 0, length = 0;

        while (a_intfc->GetNextRecordInfo(type, version, length)) {
            if (type != kRecordType) continue;

            if (version != kSerializationVersion) {
                logger::critical("Unsupported armor scaling serialization version {}", version);
                continue;
            }

            std::uint32_t count = 0;
            if (!a_intfc->ReadRecordData(&count, sizeof(count))) {
                logger::critical("Failed to read scaling record count"); return;
            }

            std::lock_guard<std::mutex> lock(g_dataMutex);
            for (std::uint32_t i = 0; i < count; ++i) {
                SavedScalingData rec{};
                if (!a_intfc->ReadRecordData(&rec, sizeof(rec))) {
                    logger::critical("Failed to read scaling record {}", i); return;
                }

                RE::FormID resolvedFormID = 0;
                if (!a_intfc->ResolveFormID(rec.formID, resolvedFormID)) {
                    logger::critical("Failed to resolve saved actor formID {:08X}", rec.formID);
                    continue;
                }

                ScalingData data;
                data.lastAppliedMR = rec.lastAppliedMR;
                data.lastSeenDR = rec.lastSeenDR;

                if (RE::TESForm* form = RE::TESForm::LookupByID(resolvedFormID))
                    if (RE::Actor* actor = form->As<RE::Actor>())
                        data.handle = actor->GetHandle();

                g_actorData[resolvedFormID] = data;
            }

            logger::info("Loaded {} armor scaling records", count);
        }
    }

    void Revert(SKSE::SerializationInterface*)
    {
        ClearRuntimeState();
        logger::info("Armor scaling runtime state reverted");
    }

    void Initialize()
    {
        logger::info("ARMOR_SCALING::Initialize() entered");

        RE::TESDataHandler* dataHandler = RE::TESDataHandler::GetSingleton();
        if (!dataHandler) {
            logger::critical("TESDataHandler is null"); return;
        }

        g_scalingSpell = dataHandler->LookupForm<RE::SpellItem>(0x1A6, "Armor Resist Magic.esp");
        if (!g_scalingSpell)
            logger::critical("Failed to find scaling spell");

        g_materialRules = LoadMaterialRulesFromIniFiles(dataHandler);
        SETTINGS::Load();
        g_rrParams = SETTINGS::LoadRRParams();

        logger::info("Armor Scaling Initialized");
        g_systemReady = true;
    }
}