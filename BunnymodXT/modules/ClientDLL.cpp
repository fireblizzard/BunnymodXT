#include "stdafx.hpp"

#include "../sptlib-wrapper.hpp"
#include <SPTLib/MemUtils.hpp>
#include <SPTLib/Hooks.hpp>
#include "../modules.hpp"
#include "../patterns.hpp"
#include "../cvars.hpp"
#include "../hud_custom.hpp"

void __cdecl ClientDLL::HOOKED_PM_Jump()
{
	return clientDLL.HOOKED_PM_Jump_Func();
}

void __cdecl ClientDLL::HOOKED_PM_PreventMegaBunnyJumping()
{
	return clientDLL.HOOKED_PM_PreventMegaBunnyJumping_Func();
}

int __cdecl ClientDLL::HOOKED_Initialize(cl_enginefunc_t* pEnginefuncs, int iVersion)
{
	return clientDLL.HOOKED_Initialize_Func(pEnginefuncs, iVersion);
}

void __fastcall ClientDLL::HOOKED_CHud_Init(void* thisptr, int edx)
{
	return clientDLL.HOOKED_CHud_Init_Func(thisptr, edx);
}

void __fastcall ClientDLL::HOOKED_CHud_VidInit(void* thisptr, int edx)
{
	return clientDLL.HOOKED_CHud_VidInit_Func(thisptr, edx);
}

void __cdecl ClientDLL::HOOKED_V_CalcRefdef(ref_params_t* pparams)
{
	return clientDLL.HOOKED_V_CalcRefdef_Func(pparams);
}

void ClientDLL::Hook(const std::wstring& moduleName, void* moduleHandle, void* moduleBase, size_t moduleLength, bool needToIntercept)
{
	Clear(); // Just in case.

	m_Handle = moduleHandle;
	m_Base = moduleBase;
	m_Length = moduleLength;
	m_Name = moduleName;
	m_Intercepted = needToIntercept;

	MemUtils::ptnvec_size ptnNumber;

	void *pPMJump = nullptr,
		*pPMPreventMegaBunnyJumping = nullptr,
		*pCHud_AddHudElem = nullptr;

	auto fPMPreventMegaBunnyJumping = std::async(std::launch::async, MemUtils::FindUniqueSequence, moduleBase, moduleLength, Patterns::ptnsPMPreventMegaBunnyJumping, &pPMPreventMegaBunnyJumping);
	auto fCHud_AddHudElem = std::async(std::launch::async, MemUtils::FindUniqueSequence, moduleBase, moduleLength, Patterns::ptnsCHud_AddHudElem, &pCHud_AddHudElem);
	auto fIsGMC = std::async(std::launch::deferred, MemUtils::FindPattern, moduleBase, moduleLength, reinterpret_cast<const byte *>("weapon_SPchemicalgun"), "xxxxxxxxxxxxxxxxxxxx");

	ptnNumber = MemUtils::FindUniqueSequence(moduleBase, moduleLength, Patterns::ptnsPMJump, &pPMJump);
	if (ptnNumber != MemUtils::INVALID_SEQUENCE_INDEX)
	{
		ORIG_PM_Jump = reinterpret_cast<_PM_Jump>(pPMJump);
		EngineDevMsg("[client dll] Found PM_Jump at %p (using the %s pattern).\n", pPMJump, Patterns::ptnsPMJump[ptnNumber].build.c_str());

		switch (ptnNumber)
		{
		case 0:
			ppmove = *reinterpret_cast<void***>(reinterpret_cast<uintptr_t>(pPMJump) + 2);
			offOldbuttons = 200;
			offOnground = 224;
			break;

		case 1:
			ppmove = *reinterpret_cast<void***>(reinterpret_cast<uintptr_t>(pPMJump) + 2);
			offOldbuttons = 200;
			offOnground = 224;
			break;

		case 2: // AG-Server, shouldn't happen here but who knows.
			ppmove = *reinterpret_cast<void***>(reinterpret_cast<uintptr_t>(pPMJump) + 3);
			offOldbuttons = 200;
			offOnground = 224;
			break;

		case 3:
			ppmove = *reinterpret_cast<void***>(reinterpret_cast<uintptr_t>(pPMJump) + 3);
			offOldbuttons = 200;
			offOnground = 224;
			break;
		}
	}
	else
	{
		EngineDevWarning("[client dll] Could not find PM_Jump!\n");
		EngineWarning("Autojump prediction is not available.\n");
	}

	ptnNumber = fPMPreventMegaBunnyJumping.get();
	if (ptnNumber != MemUtils::INVALID_SEQUENCE_INDEX)
	{
		ORIG_PM_PreventMegaBunnyJumping = reinterpret_cast<_PM_PreventMegaBunnyJumping>(pPMPreventMegaBunnyJumping);
		EngineDevMsg("[client dll] Found PM_PreventMegaBunnyJumping at %p (using the %s pattern).\n", pPMPreventMegaBunnyJumping, Patterns::ptnsPMPreventMegaBunnyJumping[ptnNumber].build.c_str());
	}
	else
	{
		EngineDevWarning("[client dll] Could not find PM_PreventMegaBunnyJumping!\n");
		EngineWarning("Bhopcap disabling prediction is not available.\n");
	}

	// In AG, this thing is the main function, so check that first.
	_Initialize pInitialize = reinterpret_cast<_Initialize>(MemUtils::GetFunctionAddress(moduleHandle, "?Initialize_Body@@YAHPAUcl_enginefuncs_s@@H@Z"));

	if (!pInitialize)
		pInitialize = reinterpret_cast<_Initialize>(MemUtils::GetFunctionAddress(moduleHandle, "Initialize"));

	if (pInitialize)
	{
		// Find "mov edi, offset dword; rep movsd" inside Initialize. The pointer to gEngfuncs is that dword.
		const byte pattern[] = { 0xBF, '?', '?', '?', '?', 0xF3, 0xA5 };
		auto addr = MemUtils::FindPattern(pInitialize, 40, pattern, "x????xx");
		if (addr)
		{
			pEngfuncs = *reinterpret_cast<cl_enginefunc_t**>(reinterpret_cast<uintptr_t>(addr) + 1);
			EngineDevMsg("[client dll] pEngfuncs is %p.\n", pEngfuncs);

			// If we have engfuncs, register cvars and whatnot right away (in the end of this function because other stuff need to be done first). Otherwise wait till the engine gives us engfuncs.
			// This works because global variables are zero by default.
			if (!*reinterpret_cast<uintptr_t*>(pEngfuncs))
				ORIG_Initialize = pInitialize;
		}
		else
		{
			EngineDevWarning("[client dll] Couldn't find the pattern in Initialize!\n");
			EngineWarning("Clientside CVars and commands are not available.\n");
			EngineWarning("Custom HUD is not available.\n");
		}
	}
	else
	{
		EngineDevWarning("[client dll] Couldn't get the address of Initialize!\n");
		EngineWarning("Clientside CVars and commands are not available.\n");
		EngineWarning("Custom HUD is not available.\n");
	}

	// We can draw stuff only if we know that we have already received / will receive engfuncs.
	if (pEngfuncs)
	{
		auto pHUD_Init = MemUtils::GetFunctionAddress(moduleHandle, "HUD_Init");
		if (pHUD_Init)
		{
			// Just in case some HUD_Init contains extra stuff, find the first "mov ecx, offset dword; call func" sequence. Dword is the pointer to gHud and func is CHud::Init.
			const byte pattern[] = { 0xB9, '?', '?', '?', '?', 0xE8 };

			// BS has jmp instead of call.
			const byte pattern_bs[] = { 0xB9, '?', '?', '?', '?', 0xE9 };

			// OP4 contains some stuff between our instructions.
			const byte pattern_op4[] = { 0xB9, '?', '?', '?', '?', 0xA3, '?', '?', '?', '?',
				0xC7, 0x05, '?', '?', '?', '?', 0xA0, 0x00, 0x00, 0x00,
				0xA3, '?', '?', '?', '?', 0xE8 };

			ptrdiff_t offCallOffset = 6;
			auto addr = MemUtils::FindPattern(pHUD_Init, 0x15, pattern, "x????x");
			if (!addr)
				addr = MemUtils::FindPattern(pHUD_Init, 0x15, pattern_bs, "x????x");
			if (!addr)
			{
				addr = MemUtils::FindPattern(pHUD_Init, 0x15, pattern_op4, "x????x????xx????xxxxx????x");
				offCallOffset = 26;
				novd = true;
				EngineDevMsg("[client dll] Using CHudBase without a virtual destructor.\n");
			}
			else
			{
				// Check for GMC.
				if (fIsGMC.get() != NULL)
				{
					novd = true;
					EngineDevMsg("[client dll] Using CHudBase without a virtual destructor.\n");
				}
			}

			if (addr)
			{
				pHud = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(addr) + 1);
				ORIG_CHud_Init = reinterpret_cast<_CHud_InitFunc>(*reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(addr) + offCallOffset) + (reinterpret_cast<uintptr_t>(addr) + offCallOffset + 4)); // Call by offset.
				EngineDevMsg("[client dll] pHud is %p; CHud::Init is located at %p.\n", pHud, ORIG_CHud_Init);

				auto pHUD_Reset = MemUtils::GetFunctionAddress(moduleHandle, "HUD_Reset");
				if (pHUD_Reset)
				{
					// Same as with HUD_Init earlier, but we have another possibility - jmp instead of call.
					auto pHud_uintptr = reinterpret_cast<uintptr_t>(pHud);
					#define getbyte(a, n) static_cast<byte>((a >> n*8) & 0xFF)
					const byte ptn1[] = { 0xB9, getbyte(pHud_uintptr, 0), getbyte(pHud_uintptr, 1), getbyte(pHud_uintptr, 2), getbyte(pHud_uintptr, 3), 0xE8 },
						ptn2[] = { 0xB9, getbyte(pHud_uintptr, 0), getbyte(pHud_uintptr, 1), getbyte(pHud_uintptr, 2), getbyte(pHud_uintptr, 3), 0xE9 };
					#undef getbyte

					auto addr_ = MemUtils::FindPattern(pHUD_Reset, 0x10, ptn1, "xxxxxx");
					if (!addr_)
						addr_ = MemUtils::FindPattern(pHUD_Reset, 0x10, ptn2, "xxxxxx");

					if (addr_)
					{
						ORIG_CHud_VidInit = reinterpret_cast<_CHud_InitFunc>(*reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(addr_) + 6) + (reinterpret_cast<uintptr_t>(addr_) + 10));
						EngineDevMsg("[client dll] CHud::VidInit is located at %p.\n", ORIG_CHud_VidInit);
					}
					else
					{
						pHUD_Reset = nullptr; // Try with HUD_VidInit.
					}
				}

				if (!pHUD_Reset)
				{
					auto pHUD_VidInit = MemUtils::GetFunctionAddress(moduleHandle, "HUD_VidInit");
					if (pHUD_VidInit)
					{
						auto pHud_uintptr = reinterpret_cast<uintptr_t>(pHud);
						#define getbyte(a, n) (byte)((a >> n*8) & 0xFF)
						const byte ptn[] = { 0xB9, getbyte(pHud_uintptr, 0), getbyte(pHud_uintptr, 1), getbyte(pHud_uintptr, 2), getbyte(pHud_uintptr, 3), 0xE8 };
						#undef getbyte

						auto addr_ = MemUtils::FindPattern(pHUD_VidInit, 0x10, ptn, "xxxxxx");
						if (addr_)
						{
							ORIG_CHud_VidInit = reinterpret_cast<_CHud_InitFunc>(*reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(addr_) + 6) + (reinterpret_cast<uintptr_t>(addr_) + 10));
							EngineDevMsg("[client dll] CHud::VidInit is located at %p.\n", ORIG_CHud_VidInit);
						}
						else
						{
							ORIG_CHud_Init = nullptr;

							EngineDevWarning("[client dll] Couldn't find the pattern in HUD_Reset or HUD_VidInit!\n");
							EngineWarning("Custom HUD is not available.\n");
						}
					}
					else
					{
						ORIG_CHud_Init = nullptr;

						EngineDevWarning("[client dll] Couldn't get the address of HUD_Reset or HUD_VidInit!\n");
						EngineWarning("Custom HUD is not available.\n");
					}
				}
			}
			else
			{
				EngineDevWarning("[client dll] Couldn't find the pattern in HUD_Init!\n");
				EngineWarning("Custom HUD is not available.\n");
			}
		}
		else
		{
			EngineDevWarning("[client dll] Couldn't get the address of HUD_Init!\n");
			EngineWarning("Custom HUD is not available.\n");
		}

		if (ORIG_CHud_Init)
		{
			ptnNumber = fCHud_AddHudElem.get();
			if (ptnNumber != MemUtils::INVALID_SEQUENCE_INDEX)
			{
				CHud_AddHudElem = reinterpret_cast<_CHud_AddHudElem>(pCHud_AddHudElem);
				EngineDevMsg("[client dll] Found CHud::AddHudElem at %p (using the %s pattern).\n", pCHud_AddHudElem, Patterns::ptnsCHud_AddHudElem[ptnNumber].build.c_str());
			}
			else
			{
				ORIG_CHud_Init = nullptr;
				ORIG_CHud_VidInit = nullptr;

				EngineDevWarning("[client dll] Could not find CHud::AddHudElem!\n");
				EngineWarning("Custom HUD is not available.\n");
			}
		}
	}

	ORIG_V_CalcRefdef = reinterpret_cast<_V_CalcRefdef>(MemUtils::GetFunctionAddress(moduleHandle, "V_CalcRefdef"));
	if (!ORIG_V_CalcRefdef)
	{
		EngineDevWarning("[client dll] Couldn't find V_CalcRefdef!\n");
		EngineWarning("Velocity display during demo playback is not available.\n");
	}
	
	// Now we can register cvars and commands provided that we already have engfuncs.
	if (*reinterpret_cast<uintptr_t*>(pEngfuncs))
		RegisterCVarsAndCommands();

	if (needToIntercept)
		MemUtils::Intercept(moduleName, {
			{ reinterpret_cast<void**>(&ORIG_PM_Jump), HOOKED_PM_Jump },
			{ reinterpret_cast<void**>(&ORIG_PM_PreventMegaBunnyJumping), HOOKED_PM_PreventMegaBunnyJumping },
			{ reinterpret_cast<void**>(&ORIG_Initialize), HOOKED_Initialize },
			{ reinterpret_cast<void**>(&ORIG_CHud_Init), HOOKED_CHud_Init },
			{ reinterpret_cast<void**>(&ORIG_CHud_VidInit), HOOKED_CHud_VidInit },
			{ reinterpret_cast<void**>(&ORIG_V_CalcRefdef), HOOKED_V_CalcRefdef }
		});
}

void ClientDLL::Unhook()
{
	if (m_Intercepted)
		MemUtils::RemoveInterception(m_Name, {
			{ reinterpret_cast<void**>(&ORIG_PM_Jump), HOOKED_PM_Jump },
			{ reinterpret_cast<void**>(&ORIG_PM_PreventMegaBunnyJumping), HOOKED_PM_PreventMegaBunnyJumping },
			{ reinterpret_cast<void**>(&ORIG_Initialize), HOOKED_Initialize },
			{ reinterpret_cast<void**>(&ORIG_CHud_Init), HOOKED_CHud_Init },
			{ reinterpret_cast<void**>(&ORIG_CHud_VidInit), HOOKED_CHud_VidInit },
			{ reinterpret_cast<void**>(&ORIG_V_CalcRefdef), HOOKED_V_CalcRefdef }
		});

	Clear();
}

void ClientDLL::Clear()
{
	IHookableNameFilter::Clear();
	ORIG_PM_Jump = nullptr;
	ORIG_PM_PreventMegaBunnyJumping = nullptr;
	ORIG_Initialize = nullptr;
	ORIG_CHud_Init = nullptr;
	ORIG_CHud_VidInit = nullptr;
	CHud_AddHudElem = nullptr;
	ORIG_V_CalcRefdef = nullptr;
	ppmove = nullptr;
	offOldbuttons = 0;
	offOnground = 0;
	pEngfuncs = nullptr;
	pHud = nullptr;
	cantJumpNextTime = false;
	novd = false;
	m_Intercepted = false;
}

void ClientDLL::RegisterCVarsAndCommands()
{
	if (!pEngfuncs || !*reinterpret_cast<uintptr_t*>(pEngfuncs))
		return;

	if (ORIG_PM_Jump)
		y_bxt_autojump_prediction = pEngfuncs->pfnRegisterVariable("y_bxt_autojump_prediction", "0", 0);

	if (ORIG_PM_PreventMegaBunnyJumping)
		y_bxt_bhopcap_prediction = pEngfuncs->pfnRegisterVariable("y_bxt_bhopcap_prediction", "0", 0);

	if (ORIG_CHud_Init)
	{
		con_color = pEngfuncs->pfnGetCvarPointer("con_color");
		y_bxt_hud = pEngfuncs->pfnRegisterVariable("y_bxt_hud", "1", 0);
		y_bxt_hud_precision = pEngfuncs->pfnRegisterVariable("y_bxt_hud_precision", "6", 0);
		y_bxt_hud_velocity = pEngfuncs->pfnRegisterVariable("y_bxt_hud_velocity", "1", 0);
		y_bxt_hud_velocity_pos = pEngfuncs->pfnRegisterVariable("y_bxt_hud_velocity_pos", "-200 0", 0);
		y_bxt_hud_origin = pEngfuncs->pfnRegisterVariable("y_bxt_hud_origin", "0", 0);
		y_bxt_hud_origin_pos = pEngfuncs->pfnRegisterVariable("y_bxt_hud_origin_pos", "-200 115", 0);
	}

	EngineDevMsg("[client dll] Registered CVars.\n");
}

void ClientDLL::AddHudElem(void* pHudElem)
{
	if (pHud && CHud_AddHudElem)
		CHud_AddHudElem(pHud, 0, pHudElem);
}

void __cdecl ClientDLL::HOOKED_PM_Jump_Func()
{
	auto pmove = reinterpret_cast<uintptr_t>(*ppmove);
	int *onground = reinterpret_cast<int*>(pmove + offOnground);
	int orig_onground = *onground;

	int *oldbuttons = reinterpret_cast<int*>(pmove + offOldbuttons);
	int orig_oldbuttons = *oldbuttons;

	if (!y_bxt_autojump_prediction || (y_bxt_autojump_prediction->value != 0.0f))
	{
		if ((orig_onground != -1) && !cantJumpNextTime)
			*oldbuttons &= ~IN_JUMP;
	}

	cantJumpNextTime = false;

	ORIG_PM_Jump();

	if ((orig_onground != -1) && (*onground == -1))
		cantJumpNextTime = true;

	if (!y_bxt_autojump_prediction || (y_bxt_autojump_prediction->value != 0.0f))
	{
		*oldbuttons = orig_oldbuttons;
	}
}

void __cdecl ClientDLL::HOOKED_PM_PreventMegaBunnyJumping_Func()
{
	if (y_bxt_bhopcap_prediction && (y_bxt_bhopcap_prediction->value != 0.0f))
		ORIG_PM_PreventMegaBunnyJumping();
}

int __cdecl ClientDLL::HOOKED_Initialize_Func(cl_enginefunc_t* pEnginefuncs, int iVersion)
{
	int rv = ORIG_Initialize(pEnginefuncs, iVersion);

	RegisterCVarsAndCommands();

	return rv;
}

void __fastcall ClientDLL::HOOKED_CHud_Init_Func(void* thisptr, int edx)
{
	ORIG_CHud_Init(thisptr, edx);

	if (novd)
		customHudWrapper_NoVD.Init();
	else
		customHudWrapper.Init();
}

void __fastcall ClientDLL::HOOKED_CHud_VidInit_Func(void* thisptr, int edx)
{
	ORIG_CHud_VidInit(thisptr, edx);

	if (novd)
	{
		customHudWrapper_NoVD.InitIfNecessary();
		customHudWrapper_NoVD.VidInit();
	}
	else
	{
		customHudWrapper.InitIfNecessary();
		customHudWrapper.VidInit();
	}
}

void __cdecl ClientDLL::HOOKED_V_CalcRefdef_Func(ref_params_t* pparams)
{
	ORIG_V_CalcRefdef(pparams);

	CustomHud::UpdatePlayerInfoInaccurate(pparams->simvel, pparams->simorg);
}