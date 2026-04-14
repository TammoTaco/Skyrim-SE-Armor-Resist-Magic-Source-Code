#include "ArmorHook.h"

#include "ArmorScaling.h"

#include <RE/C/Character.h>
#include <RE/P/PlayerCharacter.h>
#include <REL/Relocation.h>

namespace ARMOR_HOOK
{
	namespace
	{
		bool g_installed = false;
	}

	struct ActorUpdateHook
	{
		static void thunk(RE::Character* actor, float delta)
		{
			func(actor, delta);

			if (ARMOR_SCALING::IsBulkRefreshing()) {
				return;
			}

			if (!actor) {
				return;
			}

			ARMOR_SCALING::ApplyToActor(actor, false);
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct PlayerUpdateHook
	{
		static void thunk(RE::PlayerCharacter* player, float delta)
		{
			func(player, delta);

			if (ARMOR_SCALING::IsBulkRefreshing()) {
				return;
			}

			if (!player) {
				return;
			}

			ARMOR_SCALING::ApplyToActor(player, true);
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};

	void Install()
	{
		if (g_installed) {
			logger::info("Armor hooks already installed, skipping.");
			return;
		}

		REL::Relocation<std::uintptr_t> characterVtbl{ RE::VTABLE_Character[0] };
		REL::Relocation<std::uintptr_t> playerVtbl{ RE::VTABLE_PlayerCharacter[0] };

		ActorUpdateHook::func =
			characterVtbl.write_vfunc(0xAD, ActorUpdateHook::thunk);

		PlayerUpdateHook::func =
			playerVtbl.write_vfunc(0xAD, PlayerUpdateHook::thunk);

		g_installed = true;

		logger::info("Hooked Character::Update and PlayerCharacter::Update");
	}
}