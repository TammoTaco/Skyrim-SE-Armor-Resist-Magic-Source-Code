static_assert(true, "ArmorScaling.cpp is compiling");

#include "ArmorScaling.h"

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
#include <RE/T/TESGlobal.h>
#include <RE/T/TESObjectARMO.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

namespace ARMOR_SCALING
{
	struct MaterialRule
	{
		std::string name;
		std::string pluginName;
		RE::FormID localFormID = 0;
		RE::BGSKeyword* keyword = nullptr;
		float playerMult = 1.0f;
		float npcMult = 1.0f;
		std::string sourceFile;
	};

	struct ScalingData
	{
		float lastAppliedMR = 0.0f;
		float lastSeenDR = -1.0f;
		RE::ActorHandle handle;
	};

	struct SavedScalingData
	{
		RE::FormID formID = 0;
		float lastAppliedMR = 0.0f;
		float lastSeenDR = -1.0f;
	};

	bool g_systemReady = false;
	bool g_bulkRefreshing = false;
	bool g_npcRepairPassRunning = false;
	bool g_npcRepairWasEnabled = false;

	std::unordered_map<RE::FormID, ScalingData> g_actorData;
	std::unordered_map<RE::FormID, bool> g_repairedNpcThisActivation;
	std::vector<MaterialRule> g_materialRules;

	RE::TESGlobal* g_enabled = nullptr;
	RE::TESGlobal* g_useDifferentNPCValues = nullptr;
	RE::TESGlobal* g_useDifferentNPCMaterialValues = nullptr;

	RE::TESGlobal* g_playerMaxDR = nullptr;
	RE::TESGlobal* g_playerMRAtMax = nullptr;

	RE::TESGlobal* g_npcMaxDR = nullptr;
	RE::TESGlobal* g_npcMRAtMax = nullptr;

	RE::SpellItem* g_scalingSpell = nullptr;
	RE::TESGlobal* g_logLevel = nullptr;

	RE::TESGlobal* g_enableMaterialBonuses = nullptr;
	RE::TESGlobal* g_materialBonusesPlayerOnly = nullptr;
	RE::TESGlobal* g_npcRepairEnabled = nullptr;

	namespace
	{
		constexpr float kEpsilon = 0.01f;
		constexpr float kUnsetValue = -9999.0f;

		inline const std::filesystem::path kConfigDir =
			std::filesystem::path("Data") / "SKSE" / "Plugins";
		inline const std::filesystem::path kConfigPath =
			kConfigDir / "ArmorResistsMagic.ini";
		inline const std::filesystem::path kMaterialsDir =
			kConfigDir / "ArmorResistsMagic" / "Materials";

		constexpr std::uint32_t kSerializationVersion = 1;
		constexpr std::uint32_t kRecordType = 'ARMR';

		std::string Trim(std::string_view text)
		{
			std::size_t start = 0;
			while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) {
				++start;
			}

			std::size_t end = text.size();
			while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
				--end;
			}

			return std::string(text.substr(start, end - start));
		}

		std::string ToLower(std::string text)
		{
			std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
				return static_cast<char>(std::tolower(c));
				});
			return text;
		}

		bool StartsWith(std::string_view text, std::string_view prefix)
		{
			return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
		}

		std::optional<float> ParseFloat(std::string_view text)
		{
			const std::string trimmed = Trim(text);
			if (trimmed.empty()) {
				return std::nullopt;
			}

			try {
				return std::stof(trimmed);
			}
			catch (...) {
				return std::nullopt;
			}
		}

		std::optional<RE::FormID> ParseHexFormIDRaw(std::string_view text)
		{
			std::string trimmed = Trim(text);
			if (trimmed.empty()) {
				return std::nullopt;
			}

			if (StartsWith(trimmed, "0x") || StartsWith(trimmed, "0X")) {
				trimmed = trimmed.substr(2);
			}

			RE::FormID value = 0;
			const char* begin = trimmed.data();
			const char* end = trimmed.data() + trimmed.size();

			const auto result = std::from_chars(begin, end, value, 16);
			if (result.ec != std::errc{} || result.ptr != end) {
				return std::nullopt;
			}

			return value;
		}

		RE::FormID NormalizeFormIDForLookup(RE::FormID formID)
		{
			if ((formID & 0xFF000000u) == 0xFE000000u) {
				return formID & 0x00000FFFu;
			}

			if (formID > 0x00FFFFFFu) {
				return formID & 0x00FFFFFFu;
			}

			return formID;
		}

		std::optional<RE::FormID> ParseHexFormID(std::string_view text)
		{
			const auto parsed = ParseHexFormIDRaw(text);
			if (!parsed.has_value()) {
				return std::nullopt;
			}

			return NormalizeFormIDForLookup(*parsed);
		}

		int ClampLogLevel(int level)
		{
			return std::clamp(level, 0, 2);
		}

		void ApplyLogLevel(int level)
		{
			level = ClampLogLevel(level);

			switch (level) {
			case 0:
				spdlog::set_level(spdlog::level::off);
				break;
			case 1:
				spdlog::set_level(spdlog::level::info);
				spdlog::flush_on(spdlog::level::info);
				break;
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
			if (!avo || std::abs(delta) <= kEpsilon) {
				return;
			}

			avo->RestoreActorValue(
				RE::ACTOR_VALUE_MODIFIER::kTemporary,
				RE::ActorValue::kResistMagic,
				delta);
		}

		RE::EffectSetting* GetInfoEffectSetting(RE::SpellItem* spell)
		{
			if (!spell || spell->effects.empty()) {
				return nullptr;
			}

			auto* item = spell->effects[0];
			return item ? item->baseEffect : nullptr;
		}

		void UpdatePlayerInfoEffectMagnitude(RE::Actor* actor, float magnitude)
		{
			if (!actor || !actor->IsPlayerRef() || !g_scalingSpell) {
				return;
			}

			RE::EffectSetting* infoEffect = GetInfoEffectSetting(g_scalingSpell);
			if (!infoEffect) {
				return;
			}

			RE::MagicTarget* magicTarget = GetMagicTarget(actor);
			if (!magicTarget) {
				return;
			}

			struct InfoEffectVisitor final : RE::MagicTarget::ForEachActiveEffectVisitor
			{
				RE::EffectSetting* targetEffect;
				float newMagnitude;

				InfoEffectVisitor(RE::EffectSetting* a_targetEffect, float a_newMagnitude) :
					targetEffect(a_targetEffect), newMagnitude(a_newMagnitude)
				{
				}

				RE::BSContainer::ForEachResult Accept(RE::ActiveEffect* effect) override
				{
					if (effect && effect->GetBaseObject() == targetEffect) {
						effect->magnitude = newMagnitude;
					}
					return RE::BSContainer::ForEachResult::kContinue;
				}
			};

			InfoEffectVisitor visitor(infoEffect, magnitude);
			magicTarget->VisitEffects(visitor);
		}

		bool UseNPCGlobalsForActor(RE::Actor* actor)
		{
			return actor &&
				!actor->IsPlayerRef() &&
				g_useDifferentNPCValues &&
				g_useDifferentNPCValues->value >= 1.0f;
		}

		bool UseNPCMaterialValuesForActor(RE::Actor* actor)
		{
			return actor &&
				!actor->IsPlayerRef() &&
				g_useDifferentNPCMaterialValues &&
				g_useDifferentNPCMaterialValues->value >= 1.0f;
		}

		bool MaterialBonusesEnabled()
		{
			return g_enableMaterialBonuses && g_enableMaterialBonuses->value >= 1.0f;
		}

		bool MaterialBonusesPlayerOnly()
		{
			return g_materialBonusesPlayerOnly && g_materialBonusesPlayerOnly->value >= 1.0f;
		}

		bool ShouldUseMaterialBonuses(RE::Actor* actor)
		{
			if (!actor) {
				return false;
			}

			if (!MaterialBonusesEnabled()) {
				return false;
			}

			if (MaterialBonusesPlayerOnly() && !actor->IsPlayerRef()) {
				return false;
			}

			return true;
		}

		bool IsNpcRepairEnabled()
		{
			return g_npcRepairEnabled && g_npcRepairEnabled->value >= 1.0f;
		}

		float GetActorBaseResistMagic(RE::Actor* actor)
		{
			RE::ActorValueOwner* avo = GetActorValueOwner(actor);
			return avo ? avo->GetBaseActorValue(RE::ActorValue::kResistMagic) : 0.0f;
		}

		float GetActiveEffectResistMagicContribution(RE::Actor* actor)
		{
			if (!actor) {
				return 0.0f;
			}

			RE::MagicTarget* magicTarget = GetMagicTarget(actor);
			if (!magicTarget) {
				return 0.0f;
			}

			float total = 0.0f;

			struct ResistMagicVisitor final : RE::MagicTarget::ForEachActiveEffectVisitor
			{
				float& totalRef;

				explicit ResistMagicVisitor(float& a_totalRef) :
					totalRef(a_totalRef)
				{
				}

				RE::BSContainer::ForEachResult Accept(RE::ActiveEffect* effect) override
				{
					if (!effect) {
						return RE::BSContainer::ForEachResult::kContinue;
					}

					RE::EffectSetting* base = effect->GetBaseObject();
					if (!base) {
						return RE::BSContainer::ForEachResult::kContinue;
					}

					if (base->data.primaryAV == RE::ActorValue::kResistMagic) {
						totalRef += effect->magnitude;
					}

					return RE::BSContainer::ForEachResult::kContinue;
				}
			};

			ResistMagicVisitor visitor(total);
			magicTarget->VisitEffects(visitor);

			return total;
		}

		float GetExpectedLegitimateNpcResistMagic(RE::Actor* actor)
		{
			if (!actor) {
				return 0.0f;
			}

			const float baseMR = GetActorBaseResistMagic(actor);
			const float activeEffectMR = GetActiveEffectResistMagicContribution(actor);

			return baseMR + activeEffectMR;
		}

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

				if (!entry.is_regular_file()) {
					continue;
				}

				if (ToLower(entry.path().extension().string()) == ".ini") {
					iniFiles.push_back(entry.path());
				}
			}

			std::sort(iniFiles.begin(), iniFiles.end());

			std::unordered_map<std::string, std::size_t> dedupeIndex;

			for (const auto& iniPath : iniFiles) {
				std::ifstream file(iniPath);
				if (!file.is_open()) {
					logger::error("Failed to open material rules file: {}", iniPath.string());
					continue;
				}

				logger::info("Loading material rules from {}", iniPath.string());

				MaterialRule currentRule{};
				bool inSection = false;
				std::size_t lineNo = 0;

				auto flushCurrentRule = [&]() {
					if (!inSection) {
						return;
					}

					if (currentRule.name.empty()) {
						inSection = false;
						currentRule = MaterialRule{};
						return;
					}

					if (currentRule.pluginName.empty()) {
						logger::warn(
							"Skipping material rule '{}' in {} because plugin is missing",
							currentRule.name,
							iniPath.string());
						inSection = false;
						currentRule = MaterialRule{};
						return;
					}

					if (currentRule.localFormID == 0) {
						logger::warn(
							"Skipping material rule '{}' in {} because form is missing or zero",
							currentRule.name,
							iniPath.string());
						inSection = false;
						currentRule = MaterialRule{};
						return;
					}

					currentRule.sourceFile = iniPath.filename().string();
					currentRule.keyword =
						dataHandler->LookupForm<RE::BGSKeyword>(currentRule.localFormID, currentRule.pluginName);

					if (!currentRule.keyword) {
						logger::warn(
							"Could not resolve material keyword '{}' -> {}|{:X} from {}",
							currentRule.name,
							currentRule.pluginName,
							static_cast<std::uint32_t>(currentRule.localFormID),
							iniPath.string());
						inSection = false;
						currentRule = MaterialRule{};
						return;
					}

					const std::string dedupeKey =
						ToLower(currentRule.pluginName) + "|" + std::to_string(currentRule.localFormID);

					auto it = dedupeIndex.find(dedupeKey);
					if (it != dedupeIndex.end()) {
						logger::info(
							"Duplicate material rule for {}|{:X}; later file overrides earlier rule",
							currentRule.pluginName,
							static_cast<std::uint32_t>(currentRule.localFormID));
						rules[it->second] = currentRule;
					}
					else {
						dedupeIndex.emplace(dedupeKey, rules.size());
						rules.push_back(currentRule);
					}

					inSection = false;
					currentRule = MaterialRule{};
					};

				std::string line;
				while (std::getline(file, line)) {
					++lineNo;

					const std::string trimmed = Trim(line);
					if (trimmed.empty()) {
						continue;
					}
					if (StartsWith(trimmed, ";") || StartsWith(trimmed, "#")) {
						continue;
					}

					if (trimmed.front() == '[' && trimmed.back() == ']') {
						flushCurrentRule();

						currentRule = MaterialRule{};
						currentRule.name = Trim(std::string_view(trimmed).substr(1, trimmed.size() - 2));
						inSection = true;
						continue;
					}

					if (!inSection) {
						logger::warn(
							"Ignoring line {} in {} because it is outside a section",
							lineNo,
							iniPath.string());
						continue;
					}

					const std::size_t equalsPos = trimmed.find('=');
					if (equalsPos == std::string::npos) {
						logger::warn(
							"Ignoring malformed line {} in {}: {}",
							lineNo,
							iniPath.string(),
							trimmed);
						continue;
					}

					const std::string key = ToLower(Trim(std::string_view(trimmed).substr(0, equalsPos)));
					const std::string value = Trim(std::string_view(trimmed).substr(equalsPos + 1));

					if (key == "plugin") {
						currentRule.pluginName = value;
					}
					else if (key == "form") {
						const auto rawParsed = ParseHexFormIDRaw(value);
						if (!rawParsed.has_value()) {
							logger::warn(
								"Invalid form value '{}' in section '{}' from {}",
								value,
								currentRule.name,
								iniPath.string());
						}
						else {
							const RE::FormID normalized = NormalizeFormIDForLookup(*rawParsed);
							currentRule.localFormID = normalized;

							if (normalized != *rawParsed) {
								logger::trace(
									"Normalized form value '{}' -> {:X} for section '{}' from {}",
									value,
									static_cast<std::uint32_t>(normalized),
									currentRule.name,
									iniPath.string());
							}
						}
					}
					else if (key == "mult") {
						const auto parsed = ParseFloat(value);
						if (!parsed.has_value()) {
							logger::warn(
								"Invalid mult value '{}' in section '{}' from {}",
								value,
								currentRule.name,
								iniPath.string());
						}
						else {
							currentRule.playerMult = *parsed;
							currentRule.npcMult = *parsed;
						}
					}
					else if (key == "player_mult") {
						const auto parsed = ParseFloat(value);
						if (!parsed.has_value()) {
							logger::warn(
								"Invalid player_mult value '{}' in section '{}' from {}",
								value,
								currentRule.name,
								iniPath.string());
						}
						else {
							currentRule.playerMult = *parsed;
						}
					}
					else if (key == "npc_mult") {
						const auto parsed = ParseFloat(value);
						if (!parsed.has_value()) {
							logger::warn(
								"Invalid npc_mult value '{}' in section '{}' from {}",
								value,
								currentRule.name,
								iniPath.string());
						}
						else {
							currentRule.npcMult = *parsed;
						}
					}
					else {
						logger::warn(
							"Unknown key '{}' in section '{}' from {}",
							key,
							currentRule.name,
							iniPath.string());
					}
				}

				flushCurrentRule();
			}

			logger::info("Loaded {} material rule(s) from INI files", rules.size());
			return rules;
		}

		float GetArmorMaterialMultiplier(RE::Actor* actor, RE::TESObjectARMO* armor)
		{
			if (!actor || !armor) {
				return 1.0f;
			}

			const bool useNPC = UseNPCMaterialValuesForActor(actor);

			float totalMult = 0.0f;
			std::uint32_t matchCount = 0;

			for (const auto& rule : g_materialRules) {
				if (rule.keyword && armor->HasKeyword(rule.keyword)) {
					const float mult = useNPC ? rule.npcMult : rule.playerMult;

					totalMult += mult;
					++matchCount;

					logger::trace(
						"Matched material rule '{}' on actor {:08X}, multiplier={:.3f}",
						rule.name.c_str(),
						static_cast<std::uint32_t>(actor->GetFormID()),
						mult);
				}
			}

			if (matchCount == 0) {
				return 1.0f;
			}

			const float averageMult = totalMult / static_cast<float>(matchCount);

			logger::trace(
				"Material multiplier average on actor {:08X}: total={:.3f}, matches={}, average={:.3f}",
				static_cast<std::uint32_t>(actor->GetFormID()),
				totalMult,
				matchCount,
				averageMult);

			return averageMult;
		}

		struct ArmorContributionTotals
		{
			float vanillaEquippedArmorDR = 0.0f;
			float adjustedEquippedArmorDR = 0.0f;
		};

		ArmorContributionTotals GetEquippedArmorContributionTotals(RE::Actor* actor)
		{
			ArmorContributionTotals totals{};

			if (!actor) {
				return totals;
			}

			const auto inventory = actor->GetInventory();
			for (const auto& [boundObject, invDataPair] : inventory) {
				if (!boundObject) {
					continue;
				}

				RE::TESObjectARMO* armor = boundObject->As<RE::TESObjectARMO>();
				if (!armor) {
					continue;
				}

				const auto& [count, entry] = invDataPair;
				if (count <= 0 || !entry) {
					continue;
				}

				if (!entry->IsWorn()) {
					continue;
				}

				const float pieceArmor = armor->GetArmorRating();
				if (pieceArmor <= 0.0f) {
					continue;
				}

				const float multiplier = GetArmorMaterialMultiplier(actor, armor);

				totals.vanillaEquippedArmorDR += pieceArmor;
				totals.adjustedEquippedArmorDR += pieceArmor * multiplier;
			}

			return totals;
		}

		float CalculateEffectiveArmorForMR(RE::Actor* actor, float maxDR)
		{
			if (!actor) {
				return 0.0f;
			}

			const float actorCurrentDR = GetDamageResist(actor);

			if (!ShouldUseMaterialBonuses(actor)) {
				const float clampedDR = std::clamp(actorCurrentDR, -maxDR, maxDR);

				logger::trace(
					"EffectiveDR {:08X} materialBonusesDisabled actorDR={} effectiveDR={}",
					static_cast<std::uint32_t>(actor->GetFormID()),
					actorCurrentDR,
					clampedDR);

				return clampedDR;
			}

			const ArmorContributionTotals totals = GetEquippedArmorContributionTotals(actor);

			float effectiveDR =
				actorCurrentDR - totals.vanillaEquippedArmorDR + totals.adjustedEquippedArmorDR;

			effectiveDR = std::clamp(effectiveDR, -maxDR, maxDR);

			logger::trace(
				"EffectiveDR {:08X} actorDR={} vanillaArmor={} adjustedArmor={} effectiveDR={}",
				static_cast<std::uint32_t>(actor->GetFormID()),
				actorCurrentDR,
				totals.vanillaEquippedArmorDR,
				totals.adjustedEquippedArmorDR,
				effectiveDR);

			return effectiveDR;
		}
	}

	float g_lastEnabled = kUnsetValue;
	float g_lastUseDifferentNPC = kUnsetValue;
	float g_lastUseDifferentNPCMaterial = kUnsetValue;
	float g_lastPlayerMaxDR = kUnsetValue;
	float g_lastPlayerMRAtMax = kUnsetValue;
	float g_lastNpcMaxDR = kUnsetValue;
	float g_lastNpcMRAtMax = kUnsetValue;
	float g_lastLogLevel = kUnsetValue;
	float g_lastSavedLogLevel = kUnsetValue;
	float g_lastEnableMaterialBonuses = kUnsetValue;
	float g_lastMaterialBonusesPlayerOnly = kUnsetValue;
	float g_lastNpcRepairEnabledValue = kUnsetValue;

	void SaveLogLevelToIni(int level)
	{
		level = ClampLogLevel(level);

		std::error_code ec;
		std::filesystem::create_directories(kConfigDir, ec);

		std::ofstream file(kConfigPath, std::ios::trunc);
		if (!file.is_open()) {
			logger::error("Failed to open log config file for writing: {}", kConfigPath.string());
			return;
		}

		file << level;
	}

	void UpdateLogLevel()
	{
		if (!g_logLevel) {
			return;
		}

		const int level = static_cast<int>(g_logLevel->value);
		ApplyLogLevel(level);

		if (std::abs(static_cast<float>(level) - g_lastSavedLogLevel) > 0.001f) {
			SaveLogLevelToIni(level);
			g_lastSavedLogLevel = static_cast<float>(level);
		}
	}

	bool GlobalsChanged()
	{
		bool changed = false;

		auto check = [&](float& cached, RE::TESGlobal* globalVar) {
			if (!globalVar) {
				return;
			}

			const float current = globalVar->value;
			if (std::abs(current - cached) > 0.001f) {
				cached = current;
				changed = true;
			}
			};

		check(g_lastEnabled, g_enabled);
		check(g_lastUseDifferentNPC, g_useDifferentNPCValues);
		check(g_lastUseDifferentNPCMaterial, g_useDifferentNPCMaterialValues);
		check(g_lastPlayerMaxDR, g_playerMaxDR);
		check(g_lastPlayerMRAtMax, g_playerMRAtMax);
		check(g_lastNpcMaxDR, g_npcMaxDR);
		check(g_lastNpcMRAtMax, g_npcMRAtMax);
		check(g_lastLogLevel, g_logLevel);
		check(g_lastEnableMaterialBonuses, g_enableMaterialBonuses);
		check(g_lastMaterialBonusesPlayerOnly, g_materialBonusesPlayerOnly);
		check(g_lastNpcRepairEnabledValue, g_npcRepairEnabled);

		return changed;
	}

	float CalculateMagnitude(RE::Actor* actor)
	{
		if (!actor) {
			return 0.0f;
		}

		const bool useNPCGlobals = UseNPCGlobalsForActor(actor);

		float maxDR = 0.0f;
		float maxMR = 0.0f;

		if (!useNPCGlobals) {
			if (!g_playerMaxDR || !g_playerMRAtMax) {
				return 0.0f;
			}

			maxDR = g_playerMaxDR->value;
			maxMR = g_playerMRAtMax->value;
		}
		else {
			if (!g_npcMaxDR || !g_npcMRAtMax) {
				return 0.0f;
			}

			maxDR = g_npcMaxDR->value;
			maxMR = g_npcMRAtMax->value;
		}

		if (maxDR <= 0.0f) {
			return 0.0f;
		}

		const float effectiveDR = CalculateEffectiveArmorForMR(actor, maxDR);
		return (effectiveDR / maxDR) * maxMR;
	}

	bool IsBulkRefreshing()
	{
		return g_bulkRefreshing;
	}

	void ResetNpcRepairPassState()
	{
		g_repairedNpcThisActivation.clear();
	}

	void RepairSingleNpcMagicResist(RE::Actor* actor)
	{
		if (!actor || actor->IsPlayerRef()) {
			return;
		}

		const RE::FormID formID = actor->GetFormID();

		if (g_repairedNpcThisActivation.find(formID) != g_repairedNpcThisActivation.end()) {
			return;
		}

		auto it = g_actorData.find(formID);
		if (it == g_actorData.end()) {
			it = g_actorData.emplace(formID, ScalingData{}).first;
		}

		ScalingData& data = it->second;
		data.handle = actor->GetHandle();

		if (std::abs(data.lastAppliedMR) > kEpsilon) {
			ModResistMagicTemporary(actor, -data.lastAppliedMR);
			data.lastAppliedMR = 0.0f;
		}

		RE::ActorValueOwner* avo = GetActorValueOwner(actor);
		if (!avo) {
			g_repairedNpcThisActivation[formID] = true;
			return;
		}

		const float currentMRWithoutTracked = avo->GetActorValue(RE::ActorValue::kResistMagic);
		const float expectedLegitMR = GetExpectedLegitimateNpcResistMagic(actor);
		const float staleExcess = currentMRWithoutTracked - expectedLegitMR;

		if (staleExcess > kEpsilon) {
			logger::trace(
				"NPC MR repair on {:08X}: currentWithoutTracked={} expectedLegit={} removingExcess={}",
				static_cast<std::uint32_t>(formID),
				currentMRWithoutTracked,
				expectedLegitMR,
				staleExcess);

			ModResistMagicTemporary(actor, -staleExcess);
		}
		else {
			logger::trace(
				"NPC MR repair on {:08X}: no excess found (currentWithoutTracked={}, expectedLegit={}, delta={})",
				static_cast<std::uint32_t>(formID),
				currentMRWithoutTracked,
				expectedLegitMR,
				staleExcess);
		}

		data.lastSeenDR = -1.0f;
		g_repairedNpcThisActivation[formID] = true;
	}

	void RunNpcMagicResistRepairPass()
	{
		if (!g_systemReady) {
			return;
		}

		RE::ProcessLists* processLists = RE::ProcessLists::GetSingleton();
		if (!processLists) {
			return;
		}

		logger::trace("Running one-pass NPC MR repair for currently loaded NPCs...");

		for (auto& handle : processLists->highActorHandles) {
			if (auto actor = handle.get().get()) {
				if (!actor->IsPlayerRef()) {
					RepairSingleNpcMagicResist(actor);
				}
			}
		}

		logger::trace("One-pass NPC MR repair finished.");
	}

	void UpdateNpcRepairPass()
	{
		if (g_npcRepairPassRunning) {
			return;
		}

		const bool enabledNow = IsNpcRepairEnabled();

		if (enabledNow && !g_npcRepairWasEnabled) {
			logger::trace("NPC MR repair toggled ON. Starting new repair activation.");
			ResetNpcRepairPassState();

			g_npcRepairPassRunning = true;
			RunNpcMagicResistRepairPass();
			g_npcRepairPassRunning = false;
		}
		else if (!enabledNow && g_npcRepairWasEnabled) {
			logger::trace("NPC MR repair toggled OFF. Next ON state will allow another one-pass repair.");
			ResetNpcRepairPassState();
		}

		g_npcRepairWasEnabled = enabledNow;
	}

	void ClearRuntimeState()
	{
		g_actorData.clear();
		g_repairedNpcThisActivation.clear();
		g_npcRepairWasEnabled = false;
	}

	void ApplyToActor(RE::Actor* actor, bool)
	{
		if (!g_systemReady || !actor || !g_enabled) {
			return;
		}

		const bool globalsChanged = GlobalsChanged();
		if (globalsChanged) {
			UpdateLogLevel();
		}

		UpdateNpcRepairPass();

		if (IsNpcRepairEnabled() && !actor->IsPlayerRef() && !g_npcRepairPassRunning) {
			RepairSingleNpcMagicResist(actor);
		}

		if (g_enabled->value <= 0.0f) {
			RemoveFromActor(actor);
			return;
		}

		static bool updating = false;
		if (updating) {
			return;
		}

		updating = true;

		const RE::FormID formID = actor->GetFormID();

		auto it = g_actorData.find(formID);
		if (it == g_actorData.end()) {
			it = g_actorData.emplace(formID, ScalingData{}).first;
		}

		ScalingData& data = it->second;
		data.handle = actor->GetHandle();

		const float currentDR = GetDamageResist(actor);

		if (!globalsChanged && std::abs(currentDR - data.lastSeenDR) < kEpsilon) {
			updating = false;
			return;
		}

		data.lastSeenDR = currentDR;

		if (actor->IsPlayerRef() && g_scalingSpell && !actor->HasSpell(g_scalingSpell)) {
			actor->AddSpell(g_scalingSpell);
		}

		const float newValue = CalculateMagnitude(actor);
		const float delta = newValue - data.lastAppliedMR;

		if (std::abs(delta) < kEpsilon) {
			if (actor->IsPlayerRef()) {
				UpdatePlayerInfoEffectMagnitude(actor, newValue);
			}
			updating = false;
			return;
		}

		logger::trace(
			"ApplyToActor {:08X} DR={} oldMR={} newMR={} delta={}",
			static_cast<std::uint32_t>(actor->GetFormID()),
			currentDR,
			data.lastAppliedMR,
			newValue,
			delta);

		ModResistMagicTemporary(actor, delta);

		data.lastAppliedMR = newValue;

		if (actor->IsPlayerRef()) {
			UpdatePlayerInfoEffectMagnitude(actor, newValue);
		}

		updating = false;
	}

	void RemoveFromActor(RE::Actor* actor)
	{
		if (!actor) {
			return;
		}

		auto it = g_actorData.find(actor->GetFormID());
		if (it != g_actorData.end()) {
			if (std::abs(it->second.lastAppliedMR) > kEpsilon) {
				ModResistMagicTemporary(actor, -it->second.lastAppliedMR);
			}

			g_actorData.erase(it);
		}

		if (actor->IsPlayerRef()) {
			UpdatePlayerInfoEffectMagnitude(actor, 0.0f);
		}

		if (g_scalingSpell && actor->HasSpell(g_scalingSpell)) {
			actor->RemoveSpell(g_scalingSpell);
		}
	}

	void RemoveAllScaling()
	{
		if (!g_systemReady) {
			return;
		}

		g_systemReady = false;

		for (auto& [id, data] : g_actorData) {
			if (std::abs(data.lastAppliedMR) <= kEpsilon) {
				continue;
			}

			if (auto actor = data.handle.get().get()) {
				ModResistMagicTemporary(actor, -data.lastAppliedMR);

				if (actor->IsPlayerRef()) {
					UpdatePlayerInfoEffectMagnitude(actor, 0.0f);
				}
			}

			data.lastAppliedMR = 0.0f;
		}

		g_actorData.clear();

		logger::info("Removed all armor scaling before save/load");

		g_systemReady = true;
	}

	void RefreshAllActors()
	{
		if (!g_systemReady) {
			return;
		}

		UpdateNpcRepairPass();

		RE::ProcessLists* processLists = RE::ProcessLists::GetSingleton();
		if (!processLists) {
			return;
		}

		g_bulkRefreshing = true;

		if (RE::PlayerCharacter* player = RE::PlayerCharacter::GetSingleton()) {
			ApplyToActor(player, true);
		}

		for (auto& handle : processLists->highActorHandles) {
			if (auto actor = handle.get().get()) {
				if (!actor->IsPlayerRef()) {
					ApplyToActor(actor, false);
				}
			}
		}

		g_bulkRefreshing = false;
	}

	void CleanupActor(RE::Actor* actor)
	{
		RemoveFromActor(actor);
	}

	void Save(SKSE::SerializationInterface* a_intfc)
	{
		if (!a_intfc) {
			return;
		}

		std::vector<SavedScalingData> records;
		records.reserve(g_actorData.size());

		for (const auto& [formID, data] : g_actorData) {
			SavedScalingData rec;
			rec.formID = formID;
			rec.lastAppliedMR = data.lastAppliedMR;
			rec.lastSeenDR = data.lastSeenDR;
			records.push_back(rec);
		}

		const std::uint32_t count = static_cast<std::uint32_t>(records.size());

		if (!a_intfc->OpenRecord(kRecordType, kSerializationVersion)) {
			logger::critical("Failed to open scaling serialization record");
			return;
		}

		if (!a_intfc->WriteRecordData(&count, sizeof(count))) {
			logger::critical("Failed to write scaling record count");
			return;
		}

		for (const auto& rec : records) {
			if (!a_intfc->WriteRecordData(&rec, sizeof(rec))) {
				logger::critical("Failed to write scaling record for {:08X}", rec.formID);
				return;
			}
		}

		logger::info("Saved {} armor scaling records", count);
	}

	void Load(SKSE::SerializationInterface* a_intfc)
	{
		if (!a_intfc) {
			return;
		}

		ClearRuntimeState();

		std::uint32_t type = 0;
		std::uint32_t version = 0;
		std::uint32_t length = 0;

		while (a_intfc->GetNextRecordInfo(type, version, length)) {
			if (type != kRecordType) {
				continue;
			}

			if (version != kSerializationVersion) {
				logger::critical("Unsupported armor scaling serialization version {}", version);
				continue;
			}

			std::uint32_t count = 0;
			if (!a_intfc->ReadRecordData(&count, sizeof(count))) {
				logger::critical("Failed to read scaling record count");
				return;
			}

			for (std::uint32_t i = 0; i < count; ++i) {
				SavedScalingData rec{};
				if (!a_intfc->ReadRecordData(&rec, sizeof(rec))) {
					logger::critical("Failed to read scaling record {}", i);
					return;
				}

				RE::FormID resolvedFormID = 0;
				if (!a_intfc->ResolveFormID(rec.formID, resolvedFormID)) {
					logger::critical("Failed to resolve saved actor formID {:08X}", rec.formID);
					continue;
				}

				ScalingData data;
				data.lastAppliedMR = rec.lastAppliedMR;
				data.lastSeenDR = rec.lastSeenDR;

				if (RE::TESForm* form = RE::TESForm::LookupByID(resolvedFormID)) {
					if (RE::Actor* actor = form->As<RE::Actor>()) {
						data.handle = actor->GetHandle();
					}
				}

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
			logger::critical("TESDataHandler is null");
			return;
		}

		g_enabled = dataHandler->LookupForm<RE::TESGlobal>(0x805, "Armor Resist Magic.esp");
		g_useDifferentNPCValues = dataHandler->LookupForm<RE::TESGlobal>(0x807, "Armor Resist Magic.esp");

		g_playerMaxDR = dataHandler->LookupForm<RE::TESGlobal>(0x806, "Armor Resist Magic.esp");
		g_playerMRAtMax = dataHandler->LookupForm<RE::TESGlobal>(0x804, "Armor Resist Magic.esp");

		g_npcMaxDR = dataHandler->LookupForm<RE::TESGlobal>(0x808, "Armor Resist Magic.esp");
		g_npcMRAtMax = dataHandler->LookupForm<RE::TESGlobal>(0x809, "Armor Resist Magic.esp");

		g_scalingSpell = dataHandler->LookupForm<RE::SpellItem>(0x1A6, "Armor Resist Magic.esp");
		g_logLevel = dataHandler->LookupForm<RE::TESGlobal>(0x1A8, "Armor Resist Magic.esp");

		g_enableMaterialBonuses = dataHandler->LookupForm<RE::TESGlobal>(0x1A9, "Armor Resist Magic.esp");
		g_materialBonusesPlayerOnly = dataHandler->LookupForm<RE::TESGlobal>(0x1AA, "Armor Resist Magic.esp");

		g_useDifferentNPCMaterialValues = dataHandler->LookupForm<RE::TESGlobal>(0x1D9, "Armor Resist Magic.esp");

		g_npcRepairEnabled = dataHandler->LookupForm<RE::TESGlobal>(0x1DA, "Armor Resist Magic.esp");

		if (!g_enabled) {
			logger::critical("Failed to find global: enabled");
		}
		if (!g_useDifferentNPCValues) {
			logger::critical("Failed to find global: use different NPC values");
		}
		if (!g_useDifferentNPCMaterialValues) {
			logger::critical("Failed to find global: use different NPC material values");
		}
		if (!g_playerMaxDR) {
			logger::critical("Failed to find global: player max DR");
		}
		if (!g_playerMRAtMax) {
			logger::critical("Failed to find global: player MR at max");
		}
		if (!g_npcMaxDR) {
			logger::critical("Failed to find global: npc max DR");
		}
		if (!g_npcMRAtMax) {
			logger::critical("Failed to find global: npc MR at max");
		}
		if (!g_scalingSpell) {
			logger::critical("Failed to find scaling spell");
		}
		if (!g_logLevel) {
			logger::critical("Failed to find log level global");
		}
		if (!g_enableMaterialBonuses) {
			logger::critical("Failed to find global: enable material bonuses");
		}
		if (!g_materialBonusesPlayerOnly) {
			logger::critical("Failed to find global: material bonuses player only");
		}
		if (!g_npcRepairEnabled) {
			logger::critical("Failed to find global: npc repair enabled");
		}

		g_materialRules = LoadMaterialRulesFromIniFiles(dataHandler);

		g_lastLogLevel = g_logLevel ? g_logLevel->value : kUnsetValue;
		g_lastSavedLogLevel = g_logLevel ? g_logLevel->value : kUnsetValue;
		g_npcRepairWasEnabled = false;
		ResetNpcRepairPassState();

		GlobalsChanged();
		UpdateLogLevel();

		logger::info("Armor Scaling Initialized");
		g_systemReady = true;
	}
}