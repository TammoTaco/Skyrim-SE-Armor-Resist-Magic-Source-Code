#include "ArmorHook.h"
#include "ArmorScaling.h"
#include "Settings.h"

#include <spdlog/spdlog.h>

namespace
{
	static constexpr REL::Version kMinSupportedSKSEVersion{ 2, 0, 18, 0 };
	static constexpr std::uint32_t kSerializationID = 'ARMR';

	void ApplyStartupLogLevel()
	{
		switch (SETTINGS::LoadLogLevel()) {
		case 0:  // none
			spdlog::set_level(spdlog::level::off);
			break;

		case 1:  // info
			spdlog::set_level(spdlog::level::info);
			spdlog::flush_on(spdlog::level::info);
			break;

		case 2:  // debug
			spdlog::set_level(spdlog::level::trace);
			spdlog::flush_on(spdlog::level::trace);
			break;

		default:
			spdlog::set_level(spdlog::level::info);
			spdlog::flush_on(spdlog::level::info);
			break;
		}
	}

	void RegisterSerializationCallbacks()
	{
		auto* serialization = SKSE::GetSerializationInterface();
		if (!serialization) {
			logger::critical("Failed to get serialization interface!");
			return;
		}

		serialization->SetUniqueID(kSerializationID);

		serialization->SetSaveCallback([](SKSE::SerializationInterface* a_intfc) {
			ARMOR_SCALING::Save(a_intfc);
			});

		serialization->SetLoadCallback([](SKSE::SerializationInterface* a_intfc) {
			ARMOR_SCALING::Load(a_intfc);
			});

		serialization->SetRevertCallback([](SKSE::SerializationInterface* a_intfc) {
			ARMOR_SCALING::Revert(a_intfc);
			});
	}

	void SKSEMessageHandler(SKSE::MessagingInterface::Message* message)
	{
		switch (message->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			logger::info("kDataLoaded received. Initializing systems...");

			logger::info("Initializing armor scaling and installing hooks...");
			ARMOR_SCALING::Initialize();
			ARMOR_HOOK::Install();
			break;

		case SKSE::MessagingInterface::kPreLoadGame:
			logger::info("kPreLoadGame received. Removing active armor scaling and clearing runtime actor cache...");
			ARMOR_SCALING::RemoveAllScaling();
			ARMOR_SCALING::ClearRuntimeState();
			break;

		case SKSE::MessagingInterface::kPostLoadGame:
			logger::info("kPostLoadGame received. Refreshing armor scaling...");
			if (auto* taskInterface = SKSE::GetTaskInterface()) {
				taskInterface->AddTask([]() {
					ARMOR_SCALING::RefreshAllActors();
					});
			}
			else {
				logger::error("Failed to get task interface for kPostLoadGame refresh.");
			}
			break;

		case SKSE::MessagingInterface::kNewGame:
			logger::info("kNewGame received. Removing active armor scaling and clearing runtime actor cache...");
			ARMOR_SCALING::RemoveAllScaling();
			ARMOR_SCALING::ClearRuntimeState();

			if (auto* taskInterface = SKSE::GetTaskInterface()) {
				taskInterface->AddTask([]() {
					ARMOR_SCALING::RefreshAllActors();
					});
			}
			else {
				logger::error("Failed to get task interface for kNewGame refresh.");
			}
			break;

		default:
			break;
		}
	}
}

extern "C" DLLEXPORT bool SKSEAPI SKSEPlugin_Load(const SKSE::LoadInterface* a_skse)
{
	REL::Module::reset();
	SKSE::Init(a_skse);
	SKSE::AllocTrampoline(1 << 10);

	ApplyStartupLogLevel();

	const auto skseVersion = REL::Version::unpack(a_skse->SKSEVersion());
	if (skseVersion < kMinSupportedSKSEVersion) {
		logger::critical("Unsupported SKSE version: {}", skseVersion.string());
		return false;
	}

	logger::info("Skyrim runtime version: {}", a_skse->RuntimeVersion().string());
	logger::info("SKSE version: {}", skseVersion.string());

	auto* messaging = SKSE::GetMessagingInterface();
	if (!messaging) {
		logger::critical("Failed to get messaging interface!");
		return false;
	}

	RegisterSerializationCallbacks();

	if (!messaging->RegisterListener("SKSE", SKSEMessageHandler)) {
		logger::critical("Failed to register messaging listener!");
		return false;
	}

	logger::info("Plugin loaded successfully.");
	return true;
}