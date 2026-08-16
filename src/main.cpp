#include "ArmorHook.h"
#include "ArmorScaling.h"
#include "Settings.h"
#include <spdlog/spdlog.h>

namespace
{
    static constexpr REL::Version      kMinSupportedSKSEVersion{ 2, 0, 18, 0 };
    static constexpr std::uint32_t     kSerializationID = 'ARMR';


    class MenuEventSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
    public:
        static MenuEventSink* GetSingleton()
        {
            static MenuEventSink instance;
            return &instance;
        }

        RE::BSEventNotifyControl ProcessEvent(
            const RE::MenuOpenCloseEvent* a_event,
            RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            if (!a_event || a_event->opening)
                return RE::BSEventNotifyControl::kContinue;

            // Reload settings when opening or closing the journal or mcm menu
            const std::string_view name = a_event->menuName.c_str();
            if (name != "Journal Menu" && name != "ModConfigMenu")
                return RE::BSEventNotifyControl::kContinue;

            const bool changed = SETTINGS::Load();
            if (changed) {
                logger::trace("Settings reloaded after '{}' closed.", name);
                ARMOR_SCALING::OnSettingsChanged();
            }

            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        MenuEventSink() = default;
    };

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

            if (auto* ui = RE::UI::GetSingleton())
                ui->AddEventSink<RE::MenuOpenCloseEvent>(MenuEventSink::GetSingleton());
            else
                logger::error("Failed to get RE::UI singleton — menu event sink not registered.");
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
                    SETTINGS::Load();
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
                    SETTINGS::Load();
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

    SETTINGS::Load();

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