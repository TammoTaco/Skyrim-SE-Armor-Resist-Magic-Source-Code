#pragma once
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
namespace ARMOR_SCALING
{
	void Initialize();
	void ApplyToActor(RE::Actor* actor, bool isPlayer);
	void RemoveFromActor(RE::Actor* actor);
	void RemoveAllScaling();
	void RefreshAllActors();
	void CleanupActor(RE::Actor* actor);
	void ClearRuntimeState();
	void ResetNpcRepairPassState();
	void RepairSingleNpcMagicResist(RE::Actor* actor);
	void RunNpcMagicResistRepairPass();
	void UpdateNpcRepairPass();
	void OnSettingsChanged();
	void Save(SKSE::SerializationInterface* a_intfc);
	void Load(SKSE::SerializationInterface* a_intfc);
	void Revert(SKSE::SerializationInterface* a_intfc);
	bool IsBulkRefreshing();
	extern bool g_systemReady;
	extern bool g_bulkRefreshing;
}