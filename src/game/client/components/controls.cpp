/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include "controls.h"

#include <base/math.h>
#include <base/time.h>
#include <base/vmath.h>

#include <engine/client.h>
#include <engine/shared/config.h>

#include <generated/protocol.h>

#include <game/client/components/camera.h>
#include <game/client/components/chat.h>
#include <game/client/components/menus.h>
#include <game/client/components/scoreboard.h>
#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>
#include <game/collision.h>
#include <game/mapitems.h>

#include <algorithm>
#include <cmath>
#include <limits>

#include "pasta/aimbot.h"

constexpr int64_t gs_PastaSelfHitCooldownMs = 300;
constexpr int64_t gs_PastaRehookCooldownMs = 2800;
constexpr int64_t gs_PastaAutoAledCooldownMs = 250;

void CControls::TriggerFireTap(int Dummy)
{
	m_aInputData[Dummy].m_Fire = (m_aInputData[Dummy].m_Fire + 1) & INPUT_STATE_MASK;
	GameClient()->m_LaserUnfreeze.m_PendingSelfHitFireRelease[Dummy] = true;
}

bool CControls::IsMouseMoved(int LocalPlayerId) const
{
	const bool HasMoved = m_aMousePos[LocalPlayerId] != GameClient()->m_AvoidFreeze.m_AvoidLastMousePos[LocalPlayerId];
	GameClient()->m_AvoidFreeze.m_AvoidLastMousePos[LocalPlayerId] = m_aMousePos[LocalPlayerId];
	return HasMoved;
}

bool CControls::IsPlayerActive(int LocalPlayerId) const
{
	const CNetObj_PlayerInput &Input = m_aInputData[LocalPlayerId];
	return Input.m_Direction != 0 || Input.m_Jump != 0 || Input.m_Hook != 0 || IsMouseMoved(LocalPlayerId);
}

vec2 CControls::EnsureValidAim(vec2 Pos)
{
	if(length(Pos) < 0.001f)
		return vec2(1.0f, 0.0f);
	return Pos;
}

CControls::CControls()
{
	mem_zero(&m_aLastData, sizeof(m_aLastData));
	std::fill(std::begin(m_aMousePos), std::end(m_aMousePos), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aMousePosOnAction), std::end(m_aMousePosOnAction), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aTargetPos), std::end(m_aTargetPos), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aMouseInputType), std::end(m_aMouseInputType), EMouseInputType::ABSOLUTE);
	std::fill(std::begin(m_aRawFireState), std::end(m_aRawFireState), 0);
	std::fill(std::begin(m_aRawJumpState), std::end(m_aRawJumpState), 0);
	std::fill(std::begin(m_aRawHookState), std::end(m_aRawHookState), 0);
	std::fill(std::begin(m_aPastaLastHookState), std::end(m_aPastaLastHookState), HOOK_IDLE);
	std::fill(std::begin(m_aPastaMoonwalkFlip), std::end(m_aPastaMoonwalkFlip), 0);
	std::fill(std::begin(m_aPastaInputActivity), std::end(m_aPastaInputActivity), 0);
	std::fill(std::begin(m_aPastaAntiAfkPulseEnd), std::end(m_aPastaAntiAfkPulseEnd), 0);
	std::fill(std::begin(m_aPastaLastAntiAfkPulse), std::end(m_aPastaLastAntiAfkPulse), 0);
	std::fill(std::begin(m_aPastaLastRehook), std::end(m_aPastaLastRehook), 0);
	std::fill(std::begin(m_aPastaPendingRehookRelease), std::end(m_aPastaPendingRehookRelease), false);
	std::fill(std::begin(m_aPastaAssistAimActive), std::end(m_aPastaAssistAimActive), false);
	std::fill(std::begin(m_aPastaAssistAimWeapon), std::end(m_aPastaAssistAimWeapon), -1);
	std::fill(std::begin(m_aPastaAssistAimPos), std::end(m_aPastaAssistAimPos), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aPastaGhostMoveWarned), std::end(m_aPastaGhostMoveWarned), false);
	std::fill(std::begin(m_aPastaRenderMouseOverride), std::end(m_aPastaRenderMouseOverride), false);
	std::fill(std::begin(m_aPastaRenderMouseTargetId), std::end(m_aPastaRenderMouseTargetId), -1);
	std::fill(std::begin(m_aPastaVisibleMousePos), std::end(m_aPastaVisibleMousePos), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aPastaSentMousePos), std::end(m_aPastaSentMousePos), vec2(1.0f, 0.0f));
	std::fill(std::begin(m_aPastaFakeAimAngle), std::end(m_aPastaFakeAimAngle), 0.0f);
	std::fill(std::begin(m_aPastaFakeAimLastRandomUpdate), std::end(m_aPastaFakeAimLastRandomUpdate), 0);
}

void CControls::OnReset()
{
	ResetInput(0);
	ResetInput(1);

	for(int &AmmoCount : m_aAmmoCount)
		AmmoCount = 0;

	m_LastSendTime = 0;
}

void CControls::ResetInput(int Dummy)
{
	m_aLastData[Dummy].m_Direction = 0;
	// simulate releasing the fire button
	if((m_aLastData[Dummy].m_Fire & 1) != 0)
		m_aLastData[Dummy].m_Fire++;
	m_aLastData[Dummy].m_Fire &= INPUT_STATE_MASK;
	m_aLastData[Dummy].m_Jump = 0;
	m_aInputData[Dummy] = m_aLastData[Dummy];

	m_aInputDirectionLeft[Dummy] = 0;
	m_aInputDirectionRight[Dummy] = 0;
	m_aRawFireState[Dummy] = 0;
	m_aRawJumpState[Dummy] = 0;
	m_aRawHookState[Dummy] = 0;
	m_aPastaLastHookState[Dummy] = HOOK_IDLE;
	m_aPastaMoonwalkFlip[Dummy] = 0;
	m_aPastaAntiAfkPulseEnd[Dummy] = 0;
	m_aPastaLastAntiAfkPulse[Dummy] = time_get();
	m_aPastaLastRehook[Dummy] = 0;
	m_aPastaPendingRehookRelease[Dummy] = false;
	m_aPastaAssistAimActive[Dummy] = false;
	m_aPastaAssistAimWeapon[Dummy] = -1;
	m_aPastaAssistAimPos[Dummy] = vec2(0.0f, 0.0f);
	m_aPastaGhostMoveWarned[Dummy] = false;
	m_aPastaRenderMouseOverride[Dummy] = false;
	m_aPastaRenderMouseTargetId[Dummy] = -1;
	m_aPastaInputActivity[Dummy] = time_get();
	m_aPastaVisibleMousePos[Dummy] = vec2(0.0f, 0.0f);
	m_aPastaSentMousePos[Dummy] = vec2(1.0f, 0.0f);
	m_aPastaFakeAimAngle[Dummy] = 0.0f;
	m_aPastaFakeAimLastRandomUpdate[Dummy] = 0;
}

void CControls::OnPlayerDeath()
{
	for(int &AmmoCount : m_aAmmoCount)
		AmmoCount = 0;
}

struct CInputState
{
	CControls *m_pControls;
	int *m_apVariables[NUM_DUMMIES];
};

void CControls::ConKeyInputState(IConsole::IResult *pResult, void *pUserData)
{
	CInputState *pState = (CInputState *)pUserData;

	if(pState->m_pControls->GameClient()->m_GameInfo.m_BugDDRaceInput && pState->m_pControls->GameClient()->m_Snap.m_SpecInfo.m_Active)
		return;

	*pState->m_apVariables[g_Config.m_ClDummy] = pResult->GetInteger(0);
}

void CControls::ConKeyInputCounter(IConsole::IResult *pResult, void *pUserData)
{
	CInputState *pState = (CInputState *)pUserData;

	if((pState->m_pControls->GameClient()->m_GameInfo.m_BugDDRaceInput && pState->m_pControls->GameClient()->m_Snap.m_SpecInfo.m_Active) || pState->m_pControls->GameClient()->m_Spectator.IsActive())
		return;

	int *pVariable = pState->m_apVariables[g_Config.m_ClDummy];
	if(((*pVariable) & 1) != pResult->GetInteger(0))
		(*pVariable)++;
	*pVariable &= INPUT_STATE_MASK;
}

struct CFireInputState
{
	CControls *m_pControls;
	int *m_apVariables[NUM_DUMMIES];
	int *m_apHeld[NUM_DUMMIES];
};

void CControls::ConKeyFireInputCounter(IConsole::IResult *pResult, void *pUserData)
{
	CFireInputState *pState = (CFireInputState *)pUserData;

	if((pState->m_pControls->GameClient()->m_GameInfo.m_BugDDRaceInput && pState->m_pControls->GameClient()->m_Snap.m_SpecInfo.m_Active) || pState->m_pControls->GameClient()->m_Spectator.IsActive())
		return;

	*pState->m_apHeld[g_Config.m_ClDummy] = pResult->GetInteger(0);
	int *pVariable = pState->m_apVariables[g_Config.m_ClDummy];
	if(((*pVariable) & 1) != pResult->GetInteger(0))
		(*pVariable)++;
	*pVariable &= INPUT_STATE_MASK;
}

struct CInputSet
{
	CControls *m_pControls;
	int *m_apVariables[NUM_DUMMIES];
	int m_Value;
};

void CControls::ConKeyInputSet(IConsole::IResult *pResult, void *pUserData)
{
	CInputSet *pSet = (CInputSet *)pUserData;
	if(pResult->GetInteger(0))
	{
		*pSet->m_apVariables[g_Config.m_ClDummy] = pSet->m_Value;
	}
}

void CControls::ConKeyInputNextPrevWeapon(IConsole::IResult *pResult, void *pUserData)
{
	CInputSet *pSet = (CInputSet *)pUserData;
	ConKeyInputCounter(pResult, pSet);
	pSet->m_pControls->m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = 0;
}

void CControls::OnConsoleInit()
{
	// game commands
	{
		static CInputState s_State = {this, {&m_aInputDirectionLeft[0], &m_aInputDirectionLeft[1]}};
		Console()->Register("+left", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Move left");
	}
	{
		static CInputState s_State = {this, {&m_aInputDirectionRight[0], &m_aInputDirectionRight[1]}};
		Console()->Register("+right", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Move right");
	}
	{
		static CInputState s_State = {this, {&m_aRawJumpState[0], &m_aRawJumpState[1]}};
		Console()->Register("+jump", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Jump");
	}
	{
		static CInputState s_State = {this, {&m_aRawHookState[0], &m_aRawHookState[1]}};
		Console()->Register("+hook", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Hook");
	}
	{
		static CFireInputState s_State = {this, {&m_aInputData[0].m_Fire, &m_aInputData[1].m_Fire}, {&m_aRawFireState[0], &m_aRawFireState[1]}};
		Console()->Register("+fire", "", CFGFLAG_CLIENT, ConKeyFireInputCounter, &s_State, "Fire");
	}
	{
		static CInputState s_State = {this, {&m_aShowHookColl[0], &m_aShowHookColl[1]}};
		Console()->Register("+showhookcoll", "", CFGFLAG_CLIENT, ConKeyInputState, &s_State, "Show Hook Collision");
	}

	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 1};
		Console()->Register("+weapon1", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to hammer");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 2};
		Console()->Register("+weapon2", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to gun");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 3};
		Console()->Register("+weapon3", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to shotgun");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 4};
		Console()->Register("+weapon4", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to grenade");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_WantedWeapon, &m_aInputData[1].m_WantedWeapon}, 5};
		Console()->Register("+weapon5", "", CFGFLAG_CLIENT, ConKeyInputSet, &s_Set, "Switch to laser");
	}

	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_NextWeapon, &m_aInputData[1].m_NextWeapon}, 0};
		Console()->Register("+nextweapon", "", CFGFLAG_CLIENT, ConKeyInputNextPrevWeapon, &s_Set, "Switch to next weapon");
	}
	{
		static CInputSet s_Set = {this, {&m_aInputData[0].m_PrevWeapon, &m_aInputData[1].m_PrevWeapon}, 0};
		Console()->Register("+prevweapon", "", CFGFLAG_CLIENT, ConKeyInputNextPrevWeapon, &s_Set, "Switch to previous weapon");
	}
}

void CControls::OnMessage(int Msg, void *pRawMsg)
{
	if(Msg == NETMSGTYPE_SV_WEAPONPICKUP)
	{
		CNetMsg_Sv_WeaponPickup *pMsg = (CNetMsg_Sv_WeaponPickup *)pRawMsg;
		if(g_Config.m_ClAutoswitchWeapons)
			m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = pMsg->m_Weapon + 1;
		// We don't really know ammo count, until we'll switch to that weapon, but any non-zero count will suffice here
		m_aAmmoCount[maximum(0, pMsg->m_Weapon % NUM_WEAPONS)] = 10;
	}
}

int CControls::SnapInput(int *pData)
{
	// update player state
	if(GameClient()->m_Chat.IsActive())
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags = PLAYERFLAG_CHATTING;
	else if(GameClient()->m_Menus.IsActive())
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags = PLAYERFLAG_IN_MENU;
	else
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags = PLAYERFLAG_PLAYING;

	if(GameClient()->m_Scoreboard.IsActive())
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_SCOREBOARD;

	if(Client()->ServerCapAnyPlayerFlag() && GameClient()->m_Controls.m_aShowHookColl[g_Config.m_ClDummy])
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_AIM;

	if(Client()->ServerCapAnyPlayerFlag() && GameClient()->m_Camera.CamType() == CCamera::CAMTYPE_SPEC)
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_SPEC_CAM;

	switch(m_aMouseInputType[g_Config.m_ClDummy])
	{
	case CControls::EMouseInputType::AUTOMATED:
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_INPUT_ABSOLUTE;
		break;
	case CControls::EMouseInputType::ABSOLUTE:
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_INPUT_ABSOLUTE | PLAYERFLAG_INPUT_MANUAL;
		break;
	case CControls::EMouseInputType::RELATIVE:
		m_aInputData[g_Config.m_ClDummy].m_PlayerFlags |= PLAYERFLAG_INPUT_MANUAL;
		break;
	}

	// TClient
	if((g_Config.m_TcHideChatBubbles && Client()->RconAuthed()) || g_Config.m_PastaHideBubble)
		for(auto &InputData : m_aInputData)
			InputData.m_PlayerFlags &= ~PLAYERFLAG_CHATTING;

	if(g_Config.m_TcNameplatePingCircle)
		for(auto &InputData : m_aInputData)
			InputData.m_PlayerFlags |= PLAYERFLAG_SCOREBOARD;

	bool Send = m_aLastData[g_Config.m_ClDummy].m_PlayerFlags != m_aInputData[g_Config.m_ClDummy].m_PlayerFlags;

	m_aLastData[g_Config.m_ClDummy].m_PlayerFlags = m_aInputData[g_Config.m_ClDummy].m_PlayerFlags;

	// we freeze the input if chat or menu is activated
	if(!(m_aInputData[g_Config.m_ClDummy].m_PlayerFlags & PLAYERFLAG_PLAYING))
	{
		CNetObj_PlayerInput TasPlaybackInput;
		const bool TasPlaybackActive = GameClient()->m_PastaTas.GetPlaybackInput(TasPlaybackInput);
		if(!GameClient()->m_GameInfo.m_BugDDRaceInput)
			ResetInput(g_Config.m_ClDummy);

		if(TasPlaybackActive)
		{
			const int PlayerFlags = m_aInputData[g_Config.m_ClDummy].m_PlayerFlags & ~(PLAYERFLAG_IN_MENU | PLAYERFLAG_CHATTING);
			m_aInputData[g_Config.m_ClDummy] = TasPlaybackInput;
			m_aInputData[g_Config.m_ClDummy].m_PlayerFlags = PlayerFlags | PLAYERFLAG_PLAYING;
		}

		mem_copy(pData, &m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));

		// set the target anyway though so that we can keep seeing our surroundings,
		// even if chat or menu are activated
		vec2 Pos = GameClient()->m_Controls.m_aMousePos[g_Config.m_ClDummy];
		if(g_Config.m_TcScaleMouseDistance && !GameClient()->m_Snap.m_SpecInfo.m_Active)
		{
			const int MaxDistance = g_Config.m_ClDyncam ? g_Config.m_ClDyncamMaxDistance : g_Config.m_ClMouseMaxDistance;
			if(MaxDistance > 5 && MaxDistance < 1000) // Don't scale if angle bind or reduces precision
				Pos *= 1000.0f / (float)MaxDistance;
		}
		if(TasPlaybackActive)
			Pos = EnsureValidAim(vec2((float)TasPlaybackInput.m_TargetX, (float)TasPlaybackInput.m_TargetY));
		else
			Pos = EnsureValidAim(Pos);
		m_aPastaVisibleMousePos[g_Config.m_ClDummy] = Pos;
		m_aPastaSentMousePos[g_Config.m_ClDummy] = Pos;
		m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)Pos.x;
		m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)Pos.y;

		if(!m_aInputData[g_Config.m_ClDummy].m_TargetX && !m_aInputData[g_Config.m_ClDummy].m_TargetY)
			m_aInputData[g_Config.m_ClDummy].m_TargetX = 1;

		// send once a second just to be sure
		Send = Send || time_get() > m_LastSendTime + time_freq();
	}
	else
	{
		// TClient
		vec2 Pos;
		if(g_Config.m_ClSubTickAiming && m_aMousePosOnAction[g_Config.m_ClDummy] != vec2(0.0f, 0.0f))
		{
			Pos = GameClient()->m_Controls.m_aMousePosOnAction[g_Config.m_ClDummy];
			m_aMousePosOnAction[g_Config.m_ClDummy] = vec2(0.0f, 0.0f);
		}
		else
			Pos = GameClient()->m_Controls.m_aMousePos[g_Config.m_ClDummy];

		m_FastInputHookAction = false;
		m_FastInputFireAction = false;

		if(g_Config.m_TcScaleMouseDistance && !GameClient()->m_Snap.m_SpecInfo.m_Active)
		{
			const int MaxDistance = g_Config.m_ClDyncam ? g_Config.m_ClDyncamMaxDistance : g_Config.m_ClMouseMaxDistance;
			if(MaxDistance > 5 && MaxDistance < 1000) // Don't scale if angle bind or reduces precision
				Pos *= 1000.0f / (float)MaxDistance;
		}
		Pos = EnsureValidAim(Pos);
		vec2 SentAimPos = Pos;
		vec2 VisibleAimPos = Pos;

		m_aInputData[g_Config.m_ClDummy].m_Jump = m_aRawJumpState[g_Config.m_ClDummy];
		m_aInputData[g_Config.m_ClDummy].m_Hook = m_aRawHookState[g_Config.m_ClDummy];

		// set direction
		m_aInputData[g_Config.m_ClDummy].m_Direction = 0;
		if(m_aInputDirectionLeft[g_Config.m_ClDummy] && !m_aInputDirectionRight[g_Config.m_ClDummy])
			m_aInputData[g_Config.m_ClDummy].m_Direction = -1;
		if(!m_aInputDirectionLeft[g_Config.m_ClDummy] && m_aInputDirectionRight[g_Config.m_ClDummy])
			m_aInputData[g_Config.m_ClDummy].m_Direction = 1;

		const int Dummy = g_Config.m_ClDummy;
		const int64_t Now = time_get();
		const bool LeftHeld = m_aInputDirectionLeft[Dummy] != 0;
		const bool RightHeld = m_aInputDirectionRight[Dummy] != 0;
		const bool JumpHeld = m_aRawJumpState[Dummy] != 0;
		const bool HookHeld = m_aRawHookState[Dummy] != 0;
		const bool FireHeld = m_aRawFireState[Dummy] != 0;
		GameClient()->m_Aimbot.m_WeaponTargetId[Dummy] = -1;
		GameClient()->m_Aimbot.m_HookTargetId[Dummy] = -1;
		GameClient()->m_Aimbot.m_WeaponAimPos[Dummy] = vec2(0.0f, 0.0f);
		GameClient()->m_Aimbot.m_HookAimPos[Dummy] = vec2(0.0f, 0.0f);
		GameClient()->m_Aimbot.m_WeaponWorldPos[Dummy] = vec2(0.0f, 0.0f);
		GameClient()->m_Aimbot.m_HookWorldPos[Dummy] = vec2(0.0f, 0.0f);
		m_aPastaRenderMouseOverride[Dummy] = false;
		m_aPastaRenderMouseTargetId[Dummy] = -1;
		m_aPastaAssistAimActive[Dummy] = false;
		m_aPastaAssistAimWeapon[Dummy] = -1;
		m_aPastaAssistAimPos[Dummy] = vec2(0.0f, 0.0f);
		if(GameClient()->m_LaserUnfreeze.m_PendingSelfHitFireRelease[Dummy])
		{
			GameClient()->m_Aimbot.PulseFire(m_aInputData[Dummy]);
			GameClient()->m_LaserUnfreeze.m_PendingSelfHitFireRelease[Dummy] = false;
		}
		if(LeftHeld || RightHeld || JumpHeld || HookHeld || FireHeld || m_aInputData[Dummy].m_NextWeapon || m_aInputData[Dummy].m_PrevWeapon || m_aInputData[Dummy].m_WantedWeapon)
			m_aPastaInputActivity[Dummy] = Now;

		CCharacter *pLocalCharacter = nullptr;
		if(!GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_LocalClientId >= 0)
			pLocalCharacter = GameClient()->m_PredictedWorld.GetCharacterById(GameClient()->m_Snap.m_LocalClientId);
		const int HookState = pLocalCharacter != nullptr ? pLocalCharacter->Core()->m_HookState : HOOK_IDLE;
		const bool PulseTick = ((Now * 50 / time_freq()) & 1) != 0;

		if(g_Config.m_PastaMoonwalk && LeftHeld && RightHeld)
		{
			m_aPastaMoonwalkFlip[Dummy] ^= 1;
			m_aInputData[Dummy].m_Direction = m_aPastaMoonwalkFlip[Dummy] ? -1 : 1;
		}

		const vec2 RealAimPos = Pos;
		if(g_Config.m_PastaAvoidfreeze && pLocalCharacter != nullptr)
		{
			const int FreezeSide = GameClient()->m_AvoidFreeze.DetectFreezeSide(pLocalCharacter->GetPos());
			const bool MovingRight = m_aInputData[Dummy].m_Direction > 0 || pLocalCharacter->Core()->m_Vel.x > 0.35f;
			const bool MovingLeft = m_aInputData[Dummy].m_Direction < 0 || pLocalCharacter->Core()->m_Vel.x < -0.35f;
			if(FreezeSide > 0 && MovingRight && GameClient()->m_AvoidFreeze.HasImmediateSideFreeze(pLocalCharacter, 1))
				m_aInputData[Dummy].m_Direction = -1;
			else if(FreezeSide < 0 && MovingLeft && GameClient()->m_AvoidFreeze.HasImmediateSideFreeze(pLocalCharacter , - 1))
				m_aInputData[Dummy].m_Direction = 1;
		}

		if(g_Config.m_PastaAvoidfreeze && g_Config.m_PastaAvoidMode == 1)
		{
			GameClient()->m_AvoidFreeze.PastaAvoidFreeze();
			GameClient()->m_AvoidFreeze.PastaHookAssist();
			if(GameClient()->m_AvoidFreeze.m_AvoidForcing[Dummy] && Now <= GameClient()->m_AvoidFreeze.m_AvoidForceUntil[Dummy])
				m_aInputData[Dummy].m_Direction = GameClient()->m_AvoidFreeze.m_AvoidForcedDir[Dummy];
			else
				GameClient()->m_AvoidFreeze.m_AvoidForcing[Dummy] = false;
		}

		if(g_Config.m_PastaAvoidfreeze && g_Config.m_PastaAvoidMode == 2 && pLocalCharacter != nullptr)
		{
			const int LocalId = GameClient()->m_Snap.m_LocalClientId;
			const vec2 LocalPos = pLocalCharacter->GetPos();
			const vec2 LocalVel = pLocalCharacter->Core()->m_Vel;
			const bool DetectFreeze = true;
			const bool DetectDeath = false;
			const bool DetectTele = false;
			const bool DetectUnfreeze = false;
			const float VelocityFactor = maximum(0.1f, g_Config.m_PastaAvoidBlatantVelocityTickFactor / 10.0f);
			const int PredictTicks = std::clamp((int)(length(LocalVel) / VelocityFactor), g_Config.m_PastaAvoidBlatantMinTicks, g_Config.m_PastaAvoidBlatantMaxTicks);
			const CNetObj_DDNetCharacter *pData = &GameClient()->m_Snap.m_aCharacters[LocalId].m_ExtendedData;
			const bool FrozenNow = (pData && pData->m_FreezeEnd > 0) || GameClient()->m_AvoidFreeze.IsBlatantFrozenAtPos(LocalPos);

			auto InBlatantFov = [&](vec2 AimDir) {
				const vec2 BaseAim = normalize(EnsureValidAim(RealAimPos));
				if(g_Config.m_PastaAvoidBlatantAimbotFov >= 360)
					return true;
				const float FovCos = cosf(g_Config.m_PastaAvoidBlatantAimbotFov * pi / 360.0f);
				return dot(BaseAim, normalize(AimDir)) >= FovCos;
			};

			if(!FrozenNow && GameClient()->m_AvoidFreeze.SimulateBlatantFreezeTick(m_aInputData[Dummy], PredictTicks, DetectFreeze, DetectDeath, DetectTele, DetectUnfreeze) >= 0)
			{
				if(g_Config.m_PastaAvoidBlatantOnlyHorizontally)
				{
					for(int Dir = -1; Dir <= 1; Dir += 2)
					{
						CNetObj_PlayerInput Input = m_aInputData[Dummy];
						Input.m_Direction = Dir;
						if(GameClient()->m_AvoidFreeze.SimulateBlatantFreezeTick(Input, PredictTicks, DetectFreeze, DetectDeath, DetectTele, DetectUnfreeze) < 0)
						{
							m_aInputData[Dummy].m_Direction = Dir;
							break;
						}
					}
				}
				else
				{
					CNetObj_PlayerInput TempInput = m_aInputData[Dummy];

					if(g_Config.m_PastaAvoidBlatantCanSetDir)
					{
						for(int Dir = -1; Dir <= 1; Dir += 2)
						{
							CNetObj_PlayerInput TryInput = TempInput;
							TryInput.m_Direction = Dir;
							if(GameClient()->m_AvoidFreeze.SimulateBlatantFreezeTick(TryInput, PredictTicks, DetectFreeze, DetectDeath, DetectTele, DetectUnfreeze) < 0)
							{
								m_aInputData[Dummy].m_Direction = Dir;
								goto BlatantFreezeReset;
							}
						}
					}

					const bool FullSweep360 = g_Config.m_PastaAvoidBlatantAimbotFov >= 360;
					float BaseAngle = atan2f(RealAimPos.y, RealAimPos.x);
					int BaseDeg = (int)(BaseAngle * 180.0f / pi);
					if(BaseDeg < 0)
						BaseDeg += 360;

					int BestDangerTick = -2;
					CNetObj_PlayerInput BestImmediateInput = TempInput;
					bool FoundImmediate = false;
					for(int Hook = 0; Hook <= (g_Config.m_PastaAvoidBlatantHook ? 1 : 0); Hook++)
					{
						for(int Offset = 0; Offset < 360; Offset += 10)
						{
							const int AngleDeg = FullSweep360 ? Offset : (BaseDeg + Offset) % 360;
							const float Angle = AngleDeg * pi / 180.0f;
							const vec2 AimDir = vec2(cosf(Angle), sinf(Angle));

							CNetObj_PlayerInput TryInput = TempInput;
							TryInput.m_Hook = Hook;
							const vec2 NormalAim = normalize(EnsureValidAim(AimDir));
							TryInput.m_TargetX = (int)(NormalAim.x * 200.0f);
							TryInput.m_TargetY = (int)(NormalAim.y * 200.0f);

							const int CandidateDangerTick = GameClient()->m_AvoidFreeze.SimulateBlatantFreezeTick(TryInput, PredictTicks, DetectFreeze, DetectDeath, DetectTele, DetectUnfreeze);
							if(CandidateDangerTick < 0)
							{
								float Score = 0.0f;
								if(Hook)
									Score += 8.0f;
								if(AimDir.y < -0.15f)
									Score += 4.0f;
								if(InBlatantFov(AimDir))
									Score += 2.0f;
								if(!FullSweep360)
									Score += absolute((float)Offset) * 0.01f;
								if(!FoundImmediate || Score > BestDangerTick)
								{
									BestDangerTick = (int)(Score * 1000.0f);
									BestImmediateInput = TryInput;
									FoundImmediate = true;
								}
							}
						}
					}
					if(FoundImmediate)
					{
						m_aInputData[Dummy] = BestImmediateInput;
						goto BlatantFreezeReset;
					}

					if(g_Config.m_PastaAvoidBlatantCanSetDir)
					{
						int BestCombinedScore = -1000000000;
						CNetObj_PlayerInput BestCombinedInput = TempInput;
						bool FoundCombined = false;
						for(int Dir = -1; Dir <= 1; Dir += 2)
						{
							for(int Hook = 0; Hook <= (g_Config.m_PastaAvoidBlatantHook ? 1 : 0); Hook++)
							{
								for(int Offset = 0; Offset < 360; Offset += 15)
								{
									const int AngleDeg = FullSweep360 ? Offset : (BaseDeg + Offset) % 360;
									const float Angle = AngleDeg * pi / 180.0f;
									const vec2 AimDir = vec2(cosf(Angle), sinf(Angle));

									CNetObj_PlayerInput TryInput = TempInput;
									TryInput.m_Direction = Dir;
									TryInput.m_Hook = Hook;
									const vec2 NormalAim = normalize(EnsureValidAim(AimDir));
									TryInput.m_TargetX = (int)(NormalAim.x * 200.0f);
									TryInput.m_TargetY = (int)(NormalAim.y * 200.0f);

									const int CandidateDangerTick = GameClient()->m_AvoidFreeze.SimulateBlatantFreezeTick(TryInput, PredictTicks, DetectFreeze, DetectDeath, DetectTele, DetectUnfreeze);
									if(CandidateDangerTick < 0)
									{
										int Score = 0;
										if(Hook)
											Score += 8000;
										if(AimDir.y < -0.15f)
											Score += 4000;
										if(InBlatantFov(AimDir))
											Score += 2000;
										if(Dir == 0 || (LocalVel.x > 0.0f ? 1 : (LocalVel.x < 0.0f ? -1 : 0)) == Dir)
											Score += 1000;
										if(!FullSweep360)
											Score += Offset;
										if(!FoundCombined || Score > BestCombinedScore)
										{
											BestCombinedScore = Score;
											BestCombinedInput = TryInput;
											FoundCombined = true;
										}
									}
								}
							}
						}
						if(FoundCombined)
						{
							m_aInputData[Dummy] = BestCombinedInput;
							goto BlatantFreezeReset;
						}
					}

					if(TempInput.m_Hook)
					{
						CNetObj_PlayerInput Tick1 = TempInput;
						Tick1.m_Hook = 0;
						if(GameClient()->m_AvoidFreeze.SimulateBlatantFreezeTick(Tick1, 1, DetectFreeze, DetectDeath, DetectTele, DetectUnfreeze) < 0)
						{
							m_aInputData[Dummy].m_Hook = 0;
							goto BlatantFreezeReset;
						}

						int BestReleaseScore = -1000000000;
						CNetObj_PlayerInput BestReleaseInput = TempInput;
						bool FoundRelease = false;
						for(int AngleDeg = 0; AngleDeg < 360; AngleDeg += 10)
						{
							const float Angle = AngleDeg * pi / 180.0f;
							const vec2 AimDir = vec2(cosf(Angle), sinf(Angle));

							CNetObj_PlayerInput Tick2 = TempInput;
							Tick2.m_Hook = 1;
							const vec2 NormalAim = normalize(EnsureValidAim(AimDir));
							Tick2.m_TargetX = (int)(NormalAim.x * 200.0f);
							Tick2.m_TargetY = (int)(NormalAim.y * 200.0f);

							if(GameClient()->m_AvoidFreeze.SimulateBlatantFreezeTick(Tick2, 1, DetectFreeze, DetectDeath, DetectTele, DetectUnfreeze) < 0)
							{
								int Score = 0;
								if(AimDir.y < -0.15f)
									Score += 4000;
								if(InBlatantFov(AimDir))
									Score += 2000;
								Score += AngleDeg;
								if(!FoundRelease || Score > BestReleaseScore)
								{
									BestReleaseScore = Score;
									BestReleaseInput = Tick2;
									FoundRelease = true;
								}
							}
						}
						if(FoundRelease)
						{
							m_aInputData[Dummy].m_TargetX = BestReleaseInput.m_TargetX;
							m_aInputData[Dummy].m_TargetY = BestReleaseInput.m_TargetY;
							goto BlatantFreezeReset;
						}
					}

					int BestFreezeTick = -1;
					CNetObj_PlayerInput BestInput = TempInput;
					for(int Dir = g_Config.m_PastaAvoidBlatantCanSetDir ? -1 : 0; Dir <= (g_Config.m_PastaAvoidBlatantCanSetDir ? 1 : 0); Dir += g_Config.m_PastaAvoidBlatantCanSetDir ? 1 : 1)
					{
						for(int Hook = 0; Hook <= 1; Hook++)
						{
							for(int Offset = 0; Offset < 360; Offset += 15)
							{
								const int AngleDeg = (BaseDeg + Offset) % 360;
								const float Angle = AngleDeg * pi / 180.0f;
								const vec2 AimDir = vec2(cosf(Angle), sinf(Angle));

								CNetObj_PlayerInput TryInput = TempInput;
								if(g_Config.m_PastaAvoidBlatantCanSetDir)
									TryInput.m_Direction = Dir;
								TryInput.m_Hook = Hook;
								const vec2 NormalAim = normalize(EnsureValidAim(AimDir));
								TryInput.m_TargetX = (int)(NormalAim.x * 200.0f);
								TryInput.m_TargetY = (int)(NormalAim.y * 200.0f);

								const int FreezeTick = GameClient()->m_AvoidFreeze.SimulateBlatantFreezeTick(TryInput, PredictTicks, DetectFreeze, DetectDeath, DetectTele, DetectUnfreeze);
								if(InBlatantFov(AimDir) && FreezeTick > BestFreezeTick)
								{
									BestFreezeTick = FreezeTick;
									BestInput = TryInput;
								}
							}
						}
					}

					if(BestFreezeTick > 0)
					{
						m_aInputData[Dummy] = BestInput;
					}
				}
			}

BlatantFreezeReset:
			if(g_Config.m_PastaAvoidBlatantFreezeReset)
			{
				if(FrozenNow)
				{
					bool HasFriendNearby = false;
					for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
					{
						if(!GameClient()->m_Snap.m_aCharacters[ClientId].m_Active || ClientId == LocalId)
							continue;
						const auto &Client = GameClient()->m_aClients[ClientId];
						if(GameClient()->Friends()->IsFriend(Client.m_aName, Client.m_aClan, true) &&
							distance(Client.m_Predicted.m_Pos, LocalPos) < g_Config.m_PastaAvoidBlatantFreezeResetFriendDist * 32.0f)
						{
							HasFriendNearby = true;
							break;
						}
					}

					if(!HasFriendNearby)
					{
						if(!GameClient()->m_AvoidFreeze.m_BlatantFreezeResetFrozenLast[Dummy])
							GameClient()->m_AvoidFreeze.m_BlatantFreezeResetSince[Dummy] = Now;
						else if(Now > GameClient()->m_AvoidFreeze.m_BlatantFreezeResetSince[Dummy] + (int64_t)g_Config.m_PastaAvoidBlatantFreezeResetWait * time_freq())
							GameClient()->SendKill();
					}
				}
				GameClient()->m_AvoidFreeze.m_BlatantFreezeResetFrozenLast[Dummy] = FrozenNow;
			}
		}
		else
		{
			GameClient()->m_AvoidFreeze.m_BlatantTrackPointInit[Dummy] = false;
			GameClient()->m_AvoidFreeze.m_BlatantHookHoldTicks[Dummy] = 0;
			GameClient()->m_AvoidFreeze.m_BlatantHookTarget[Dummy] = vec2(0.0f, 0.0f);
		}

		if(g_Config.m_PastaQuickStop && !LeftHeld && !RightHeld && pLocalCharacter != nullptr)
		{
			const bool GroundedOkay = !g_Config.m_PastaQuickStopGrounded || pLocalCharacter->IsGrounded();
			if(GroundedOkay && absolute(pLocalCharacter->Core()->m_Vel.x) > 0.6f)
				m_aInputData[Dummy].m_Direction = pLocalCharacter->Core()->m_Vel.x > 0.0f ? -1 : 1;
		}

		if(g_Config.m_PastaAutoJumpSave && !JumpHeld && pLocalCharacter != nullptr && GameClient()->m_AvoidFreeze.ShouldAutoJumpSave(pLocalCharacter))
			m_aInputData[Dummy].m_Jump = 1;

		if(g_Config.m_PastaAutoRehook && pLocalCharacter != nullptr)
		{
			const bool TargetedPlayer = GameClient()->m_AvoidFreeze.HasPlayerNearAim(pLocalCharacter, RealAimPos, 380.0f, 0.18f);
			const int64_t Cooldown = gs_PastaRehookCooldownMs * time_freq() / 1000;
			if(HookHeld && HookState <= HOOK_IDLE && m_aPastaLastHookState[Dummy] > HOOK_IDLE &&
				TargetedPlayer && Now > m_aPastaLastRehook[Dummy] + Cooldown)
			{
				m_aPastaPendingRehookRelease[Dummy] = true;
				m_aPastaLastRehook[Dummy] = Now;
			}

			if(m_aPastaPendingRehookRelease[Dummy])
			{
				m_aInputData[Dummy].m_Hook = 0;
				if(!HookHeld)
					m_aPastaPendingRehookRelease[Dummy] = false;
				else if(HookState <= HOOK_IDLE)
					m_aPastaPendingRehookRelease[Dummy] = false;
			}
		}

		if(g_Config.m_PastaGhostMove)
		{
			if(!m_aPastaGhostMoveWarned[Dummy])
			{
				m_aPastaGhostMoveWarned[Dummy] = true;
				Client()->AddWarning(SWarning("Ghost Move", "Packet-level ghost move is not available in this build yet."));
			}
		}
		else
		{
			m_aPastaGhostMoveWarned[Dummy] = false;
		}

		if(g_Config.m_PastaAntiAfk && !LeftHeld && !RightHeld && !JumpHeld && !HookHeld && !FireHeld)
		{
			const int64_t Interval = (int64_t)maximum(1, g_Config.m_PastaAntiAfkSeconds) * time_freq();
			if(Now > m_aPastaInputActivity[Dummy] + Interval && Now > m_aPastaLastAntiAfkPulse[Dummy] + Interval)
			{
				m_aPastaLastAntiAfkPulse[Dummy] = Now;
				m_aPastaAntiAfkPulseEnd[Dummy] = Now + time_freq() / 25;
			}
			if(Now < m_aPastaAntiAfkPulseEnd[Dummy])
				m_aInputData[Dummy].m_Direction = 1;
		}

		if(g_Config.m_PastaAutoFire && FireHeld)
			GameClient()->m_Aimbot.PulseFire(m_aInputData[Dummy]);

		if(g_Config.m_PastaAutoAled && pLocalCharacter != nullptr)
		{
			const bool ValidAled = GameClient()->m_AutoAled.FindAutoAledTarget(pLocalCharacter, RealAimPos);
			const int64_t Cooldown = gs_PastaAutoAledCooldownMs * time_freq() / 1000;
			if(ValidAled && !GameClient()->m_AutoAled.m_AutoAledLatched[Dummy] && Now > GameClient()->m_AutoAled.m_LastAutoAled[Dummy] + Cooldown)
			{
				GameClient()->m_Aimbot.PulseFire(m_aInputData[Dummy]);
				GameClient()->m_AutoAled.m_LastAutoAled[Dummy] = Now;
			}
			GameClient()->m_AutoAled.m_AutoAledLatched[Dummy] = ValidAled;
		}

		const bool ActionFrame = m_aInputData[Dummy].m_Hook != 0 || HookState > HOOK_IDLE || m_aInputData[Dummy].m_Fire != m_aLastData[Dummy].m_Fire;
		if(g_Config.m_PastaFakeAim)
		{
			const bool RefreshAim = g_Config.m_PastaFakeAimMode == 1 ? ActionFrame :
				(g_Config.m_PastaFakeAimSendAlways != 0 || ActionFrame || length(m_aPastaSentMousePos[Dummy]) < 0.001f);
			if(RefreshAim)
				m_aPastaSentMousePos[Dummy] = GameClient()->m_Aimbot.BuildFakeAimPos(Dummy, RealAimPos, pLocalCharacter, Now);

			SentAimPos = ActionFrame ? EnsureValidAim(RealAimPos) : EnsureValidAim(m_aPastaSentMousePos[Dummy]);
			if(g_Config.m_PastaFakeAimVisible)
			{
				VisibleAimPos = EnsureValidAim(m_aPastaSentMousePos[Dummy]);
				m_aPastaRenderMouseOverride[Dummy] = true;
				m_aPastaRenderMouseTargetId[Dummy] = -1;
			}
		}

		if(g_Config.m_PastaAimbot && pLocalCharacter != nullptr)
		{
			const int ActiveWeapon = pLocalCharacter->GetActiveWeapon();
			const int WeaponSlot =
				ActiveWeapon == WEAPON_HAMMER ? static_cast<int>(CAimbot::AimbotSlot::HAMMER) :
				ActiveWeapon == WEAPON_GUN ? static_cast<int>(CAimbot::AimbotSlot::GUN) :
				ActiveWeapon == WEAPON_SHOTGUN ? static_cast<int>(CAimbot::AimbotSlot::SHOTGUN) :
				ActiveWeapon == WEAPON_GRENADE ? static_cast<int>(CAimbot::AimbotSlot::GRENADE) :
				ActiveWeapon == WEAPON_LASER ? static_cast<int>(CAimbot::AimbotSlot::LASER) : -999;

			CAimbot::SPastaAimbotTarget HookTarget;
			const bool WantsAutoHook = g_Config.m_PastaAimbotAutoHookKey || g_Config.m_PastaAimbotAutoShoot || g_Config.m_PastaAimbotAutoShootKey;
			const bool HasHookTarget = GameClient()->m_Aimbot.GetEnabled(static_cast<int>(CAimbot::AimbotSlot::HOOK)) &&
				GameClient()->m_Aimbot.FindTarget(pLocalCharacter, static_cast<int>(CAimbot::AimbotSlot::HOOK), RealAimPos, &HookTarget);
			if(HasHookTarget)
			{
				GameClient()->m_Aimbot.m_HookTargetId[Dummy] = HookTarget.m_ClientId;
				GameClient()->m_Aimbot.m_HookAimPos[Dummy] = HookTarget.m_AimPos;
				GameClient()->m_Aimbot.m_HookWorldPos[Dummy] = HookTarget.m_WorldPos;
				if(HookHeld || WantsAutoHook)
				{
					SentAimPos = HookTarget.m_AimPos;
					if(!GameClient()->m_Aimbot.GetSilent(static_cast<int>(CAimbot::AimbotSlot::HOOK)))
					{
						VisibleAimPos = HookTarget.m_AimPos;
						m_aPastaRenderMouseOverride[Dummy] = true;
						m_aPastaRenderMouseTargetId[Dummy] = HookTarget.m_ClientId;
					}
					if(WantsAutoHook)
						m_aInputData[Dummy].m_Hook = 1;
				}
			}

			CAimbot::SPastaAimbotTarget WeaponTarget;
			const bool HasWeaponTarget = WeaponSlot != -999 && GameClient()->m_Aimbot.GetEnabled(WeaponSlot) &&
						     GameClient()->m_Aimbot.FindTarget(pLocalCharacter, WeaponSlot, RealAimPos, &WeaponTarget);
			const bool WantsWeaponAim = WeaponSlot != -999 && GameClient()->m_Aimbot.GetEnabled(WeaponSlot) &&
				(FireHeld || g_Config.m_PastaAimbotAutoShoot || g_Config.m_PastaAimbotAutoShootKey);
			if(HasWeaponTarget)
			{
				GameClient()->m_Aimbot.m_WeaponTargetId[Dummy] = WeaponTarget.m_ClientId;
				GameClient()->m_Aimbot.m_WeaponAimPos[Dummy] = WeaponTarget.m_AimPos;
				GameClient()->m_Aimbot.m_WeaponWorldPos[Dummy] = WeaponTarget.m_WorldPos;
			}
			if(WantsWeaponAim && HasWeaponTarget)
			{
				SentAimPos = WeaponTarget.m_AimPos;
				if(!GameClient()->m_Aimbot.GetSilent(WeaponSlot))
				{
					VisibleAimPos = WeaponTarget.m_AimPos;
					m_aPastaRenderMouseOverride[Dummy] = true;
					m_aPastaRenderMouseTargetId[Dummy] = WeaponTarget.m_ClientId;
				}

				if(g_Config.m_PastaAimbotAutoShoot || g_Config.m_PastaAimbotAutoShootKey)
				{
					const bool TargetChanged = GameClient()->m_Aimbot.m_ScheduledTargetId[Dummy] != WeaponTarget.m_ClientId;
					if(TargetChanged || GameClient()->m_Aimbot.m_NextAutoShoot[Dummy] == 0)
					{
						const int MinDelay = minimum(g_Config.m_PastaAimbotMinShootDelay, g_Config.m_PastaAimbotMaxShootDelay);
						const int MaxDelay = maximum(g_Config.m_PastaAimbotMinShootDelay, g_Config.m_PastaAimbotMaxShootDelay);
						const int DelayTicks = MinDelay + (MaxDelay > MinDelay ? rand() % (MaxDelay - MinDelay + 1) : 0);
						GameClient()->m_Aimbot.m_NextAutoShoot[Dummy] = Now + DelayTicks * time_freq() / Client()->GameTickSpeed();
						GameClient()->m_Aimbot.m_ScheduledTargetId[Dummy] = WeaponTarget.m_ClientId;
					}

					if(GameClient()->m_LaserUnfreeze.IsWeaponReadyForSelfHit(pLocalCharacter, ActiveWeapon) &&
						Now >= GameClient()->m_Aimbot.m_NextAutoShoot[Dummy] &&
						!GameClient()->m_LaserUnfreeze.m_LastSelfHitFire[Dummy])
					{
						TriggerFireTap(Dummy);
						GameClient()->m_Aimbot.m_NextAutoShoot[Dummy] = 0;
					}
				}
				else
				{
					GameClient()->m_Aimbot.m_NextAutoShoot[Dummy] = 0;
					GameClient()->m_Aimbot.m_ScheduledTargetId[Dummy] = -1;
				}
			}
			else
			{
				GameClient()->m_Aimbot.m_NextAutoShoot[Dummy] = 0;
				GameClient()->m_Aimbot.m_ScheduledTargetId[Dummy] = -1;
			}
		}

		const bool NeedUnfreeze = pLocalCharacter != nullptr &&
			(pLocalCharacter->m_FreezeTime > 0 || GameClient()->m_AvoidFreeze.HasImpendingFreeze(pLocalCharacter, maximum(2, g_Config.m_PastaUnfreezeBotCurDirTicks)));
		if(NeedUnfreeze)
		{
			vec2 SilentAimPos = SentAimPos;
			float TimingError = 999.0f;
			const int64_t SelfHitCooldown = gs_PastaSelfHitCooldownMs * time_freq() / 1000;
			const int LaserLookTicks = maximum(maximum(2, g_Config.m_PastaUnfreezeBotTicks), pLocalCharacter != nullptr ? pLocalCharacter->m_FreezeTime + 3 : 3);
			if(g_Config.m_PastaUnfreezeBot && GameClient()->m_LaserUnfreeze.FindBestSelfHitAim(pLocalCharacter, RealAimPos, WEAPON_LASER, LaserLookTicks, &SilentAimPos, &TimingError))
			{
				m_aPastaAssistAimActive[Dummy] = true;
				m_aPastaAssistAimWeapon[Dummy] = WEAPON_LASER;
				m_aPastaAssistAimPos[Dummy] = SilentAimPos;
				m_aInputData[Dummy].m_WantedWeapon = WEAPON_LASER + 1;
				if(pLocalCharacter->GetActiveWeapon() == WEAPON_LASER && GameClient()->m_LaserUnfreeze.IsWeaponReadyForSelfHit(pLocalCharacter, WEAPON_LASER) &&
					!GameClient()->m_LaserUnfreeze.m_PendingSelfHitFireRelease[Dummy] &&
					Now > GameClient()->m_LaserUnfreeze.m_LastSelfHitFire[Dummy] + SelfHitCooldown)
				{
					SentAimPos = SilentAimPos;
					if(!g_Config.m_PastaUnfreezeBotSilent)
						VisibleAimPos = SilentAimPos;
					TriggerFireTap(Dummy);
					GameClient()->m_LaserUnfreeze.m_LastSelfHitFire[Dummy] = Now;
				}
			}
		}

		if(g_Config.m_PastaAutoShotgun && pLocalCharacter != nullptr)
		{
			vec2 SilentAimPos = SentAimPos;
			const int64_t SelfHitCooldown = gs_PastaSelfHitCooldownMs * time_freq() / 1000;
			if(GameClient()->m_LaserUnfreeze.FindBestSelfHitAim(pLocalCharacter, RealAimPos, WEAPON_SHOTGUN, maximum(2, g_Config.m_PastaAutoShotgunTicks), &SilentAimPos, nullptr))
			{
				m_aPastaAssistAimActive[Dummy] = true;
				m_aPastaAssistAimWeapon[Dummy] = WEAPON_SHOTGUN;
				m_aPastaAssistAimPos[Dummy] = SilentAimPos;
				m_aInputData[Dummy].m_WantedWeapon = WEAPON_SHOTGUN + 1;
				if(pLocalCharacter->GetActiveWeapon() == WEAPON_SHOTGUN && GameClient()->m_LaserUnfreeze.IsWeaponReadyForSelfHit(pLocalCharacter, WEAPON_SHOTGUN) &&
					GameClient()->m_LaserUnfreeze.m_PendingSelfHitFireRelease[Dummy] && Now > GameClient()->m_LaserUnfreeze.m_LastSelfHitFire[Dummy] + SelfHitCooldown)
				{
					SentAimPos = SilentAimPos;
					if(!g_Config.m_PastaAutoShotgunSilent)
						VisibleAimPos = SilentAimPos;
					TriggerFireTap(Dummy);
					GameClient()->m_LaserUnfreeze.m_LastSelfHitFire[Dummy] = Now;
				}
			}
		}

		if(g_Config.m_PastaGhostMove && (g_Config.m_PastaGhostMoveHook || g_Config.m_PastaGhostMoveHookClosest))
		{
			// Same as above: don't inject fake hook packets until a real packet-level implementation exists.
		}

		GameClient()->m_AutoEdge.UpdateAutoEdgeState(Dummy, pLocalCharacter);
		if(g_Config.m_PastaAutoEdge && pLocalCharacter != nullptr && GameClient()->m_AutoEdge.m_Locked[Dummy] && pLocalCharacter->Core()->m_Vel.y >= -0.5f)
		{
			const float Delta = GameClient()->m_AutoEdge.m_LockedPos[Dummy].x - pLocalCharacter->GetPos().x;
			const float VelX = pLocalCharacter->Core()->m_Vel.x;
			const bool LeavingLeft = Delta < 0.0f && (m_aInputData[Dummy].m_Direction < 0 || VelX < -0.35f);
			const bool LeavingRight = Delta > 0.0f && (m_aInputData[Dummy].m_Direction > 0 || VelX > 0.35f);
			if(absolute(Delta) < 14.0f && (LeavingLeft || LeavingRight))
				m_aInputData[Dummy].m_Direction = Delta > 0.0f ? -1 : 1;
			else if(absolute(Delta) < 8.0f && absolute(VelX) > 0.08f)
				m_aInputData[Dummy].m_Direction = VelX > 0.0f ? -1 : 1;
			else if(absolute(Delta) < 3.0f)
				m_aInputData[Dummy].m_Direction = 0;
		}

		CNetObj_PlayerInput TasPlaybackInput;
		if(GameClient()->m_PastaTas.GetPlaybackInput(TasPlaybackInput))
		{
			const int PlayerFlags = m_aInputData[Dummy].m_PlayerFlags;
			m_aInputData[Dummy] = TasPlaybackInput;
			m_aInputData[Dummy].m_PlayerFlags = PlayerFlags;
			SentAimPos = EnsureValidAim(vec2((float)TasPlaybackInput.m_TargetX, (float)TasPlaybackInput.m_TargetY));
			if(g_Config.m_PastaTasShowAim)
			{
				VisibleAimPos = SentAimPos;
				m_aPastaRenderMouseOverride[Dummy] = true;
				m_aPastaRenderMouseTargetId[Dummy] = -1;
			}
			else
			{
				VisibleAimPos = Pos;
				m_aPastaRenderMouseOverride[Dummy] = false;
				m_aPastaRenderMouseTargetId[Dummy] = -1;
			}
		}

		CNetObj_PlayerInput PilotInput;
		vec2 PilotMouse(0.0f, 0.0f);
		if(GameClient()->m_PastaPilot.GetRealtimeInput(PilotInput, PilotMouse))
		{
			const int PlayerFlags = m_aInputData[Dummy].m_PlayerFlags;
			m_aInputData[Dummy] = PilotInput;
			m_aInputData[Dummy].m_PlayerFlags = PlayerFlags;
			SentAimPos = EnsureValidAim(PilotMouse);
			VisibleAimPos = EnsureValidAim(m_aMousePos[Dummy]);
			m_aPastaRenderMouseOverride[Dummy] = false;
			m_aPastaRenderMouseTargetId[Dummy] = -1;
		}

		m_aPastaVisibleMousePos[Dummy] = VisibleAimPos;
		m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)SentAimPos.x;
		m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)SentAimPos.y;
		GameClient()->m_PastaTas.ObserveInput(m_aInputData[Dummy], pLocalCharacter != nullptr ? pLocalCharacter->GetPos() : GameClient()->m_LocalCharacterPos);

		m_aPastaLastHookState[Dummy] = HookState;

		// dummy copy moves
		if(g_Config.m_ClDummyCopyMoves)
		{
			CNetObj_PlayerInput *pDummyInput = &GameClient()->m_DummyInput;

			// Don't copy any input to dummy when spectating others
			if(!GameClient()->m_Snap.m_SpecInfo.m_Active || GameClient()->m_Snap.m_SpecInfo.m_SpectatorId < 0)
			{
				pDummyInput->m_Direction = m_aInputData[g_Config.m_ClDummy].m_Direction;
				pDummyInput->m_Hook = m_aInputData[g_Config.m_ClDummy].m_Hook;
				pDummyInput->m_Jump = m_aInputData[g_Config.m_ClDummy].m_Jump;
				pDummyInput->m_PlayerFlags = m_aInputData[g_Config.m_ClDummy].m_PlayerFlags;
				pDummyInput->m_TargetX = m_aInputData[g_Config.m_ClDummy].m_TargetX;
				pDummyInput->m_TargetY = m_aInputData[g_Config.m_ClDummy].m_TargetY;
				pDummyInput->m_WantedWeapon = m_aInputData[g_Config.m_ClDummy].m_WantedWeapon;

				if(!g_Config.m_ClDummyControl)
					pDummyInput->m_Fire += m_aInputData[g_Config.m_ClDummy].m_Fire - m_aLastData[g_Config.m_ClDummy].m_Fire;

				pDummyInput->m_NextWeapon += m_aInputData[g_Config.m_ClDummy].m_NextWeapon - m_aLastData[g_Config.m_ClDummy].m_NextWeapon;
				pDummyInput->m_PrevWeapon += m_aInputData[g_Config.m_ClDummy].m_PrevWeapon - m_aLastData[g_Config.m_ClDummy].m_PrevWeapon;
			}

			m_aInputData[!g_Config.m_ClDummy] = *pDummyInput;
		}

		if(g_Config.m_ClDummyControl)
		{
			CNetObj_PlayerInput *pDummyInput = &GameClient()->m_DummyInput;
			pDummyInput->m_Jump = g_Config.m_ClDummyJump;

			if(g_Config.m_ClDummyFire)
				pDummyInput->m_Fire = g_Config.m_ClDummyFire;
			else if((pDummyInput->m_Fire & 1) != 0)
				pDummyInput->m_Fire++;

			pDummyInput->m_Hook = g_Config.m_ClDummyHook;
		}

		// stress testing
		if(g_Config.m_DbgStress)
		{
			float t = Client()->LocalTime();
			mem_zero(&m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));

			m_aInputData[g_Config.m_ClDummy].m_Direction = ((int)t / 2) & 1;
			m_aInputData[g_Config.m_ClDummy].m_Jump = ((int)t);
			m_aInputData[g_Config.m_ClDummy].m_Fire = ((int)(t * 10));
			m_aInputData[g_Config.m_ClDummy].m_Hook = ((int)(t * 2)) & 1;
			m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = ((int)t) % NUM_WEAPONS;
			m_aInputData[g_Config.m_ClDummy].m_TargetX = (int)(std::sin(t * 3) * 100.0f);
			m_aInputData[g_Config.m_ClDummy].m_TargetY = (int)(std::cos(t * 3) * 100.0f);
		}

		// check if we need to send input
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Direction != m_aLastData[g_Config.m_ClDummy].m_Direction;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Jump != m_aLastData[g_Config.m_ClDummy].m_Jump;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Fire != m_aLastData[g_Config.m_ClDummy].m_Fire;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_Hook != m_aLastData[g_Config.m_ClDummy].m_Hook;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_WantedWeapon != m_aLastData[g_Config.m_ClDummy].m_WantedWeapon;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_NextWeapon != m_aLastData[g_Config.m_ClDummy].m_NextWeapon;
		Send = Send || m_aInputData[g_Config.m_ClDummy].m_PrevWeapon != m_aLastData[g_Config.m_ClDummy].m_PrevWeapon;
		Send = Send || time_get() > m_LastSendTime + time_freq() / 25; // send at least 25 Hz
		Send = Send || (GameClient()->m_Snap.m_pLocalCharacter && GameClient()->m_Snap.m_pLocalCharacter->m_Weapon == WEAPON_NINJA && (m_aInputData[g_Config.m_ClDummy].m_Direction || m_aInputData[g_Config.m_ClDummy].m_Jump || m_aInputData[g_Config.m_ClDummy].m_Hook));
	}

	// copy and return size
	m_aLastData[g_Config.m_ClDummy] = m_aInputData[g_Config.m_ClDummy];

	if(!Send)
		return 0;

	if(GameClient()->m_PastaTas.IsRecording() && GameClient()->m_PastaTas.IsWorldInitialized() && !GameClient()->m_PastaTas.IsPlaying())
	{
		CNetObj_PlayerInput Neutral = m_aInputData[g_Config.m_ClDummy];
		Neutral.m_Direction = 0;
		Neutral.m_Jump = 0;
		Neutral.m_Hook = 0;
		Neutral.m_Fire = 0;
		Neutral.m_WantedWeapon = 0;
		Neutral.m_NextWeapon = 0;
		Neutral.m_PrevWeapon = 0;
		m_aLastData[g_Config.m_ClDummy] = Neutral;
		m_LastSendTime = time_get();
		mem_copy(pData, &Neutral, sizeof(Neutral));
		return sizeof(Neutral);
	}

	m_LastSendTime = time_get();
	mem_copy(pData, &m_aInputData[g_Config.m_ClDummy], sizeof(m_aInputData[0]));
	return sizeof(m_aInputData[0]);
}

void CControls::OnRender()
{
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;

	if(g_Config.m_ClAutoswitchWeaponsOutOfAmmo && !GameClient()->m_GameInfo.m_UnlimitedAmmo && GameClient()->m_Snap.m_pLocalCharacter)
	{
		// Keep track of ammo count, we know weapon ammo only when we switch to that weapon, this is tracked on server and protocol does not track that
		m_aAmmoCount[maximum(0, GameClient()->m_Snap.m_pLocalCharacter->m_Weapon % NUM_WEAPONS)] = GameClient()->m_Snap.m_pLocalCharacter->m_AmmoCount;
		// Autoswitch weapon if we're out of ammo
		if(m_aInputData[g_Config.m_ClDummy].m_Fire % 2 != 0 &&
			GameClient()->m_Snap.m_pLocalCharacter->m_AmmoCount == 0 &&
			GameClient()->m_Snap.m_pLocalCharacter->m_Weapon != WEAPON_HAMMER &&
			GameClient()->m_Snap.m_pLocalCharacter->m_Weapon != WEAPON_NINJA)
		{
			int Weapon;
			for(Weapon = WEAPON_LASER; Weapon > WEAPON_GUN; Weapon--)
			{
				if(Weapon == GameClient()->m_Snap.m_pLocalCharacter->m_Weapon)
					continue;
				if(m_aAmmoCount[Weapon] > 0)
					break;
			}
			if(Weapon != GameClient()->m_Snap.m_pLocalCharacter->m_Weapon)
				m_aInputData[g_Config.m_ClDummy].m_WantedWeapon = Weapon + 1;
		}
	}

	// update target pos
	if(GameClient()->m_Snap.m_pGameInfoObj && !GameClient()->m_Snap.m_SpecInfo.m_Active)
	{
		// make sure to compensate for smooth dyncam to ensure the cursor stays still in world space if zoomed
		vec2 DyncamOffsetDelta = GameClient()->m_Camera.m_DyncamTargetCameraOffset - GameClient()->m_Camera.m_aDyncamCurrentCameraOffset[g_Config.m_ClDummy];
		float Zoom = GameClient()->m_Camera.m_Zoom;
		const vec2 BasePos = (GameClient()->m_PastaTas.IsRecording() && GameClient()->m_PastaTas.IsWorldInitialized()) ? GameClient()->m_PastaTas.GetInterpolatedPlayerPos() : GameClient()->m_LocalCharacterPos;
		m_aTargetPos[g_Config.m_ClDummy] = BasePos + GetRenderMousePos(g_Config.m_ClDummy) - DyncamOffsetDelta + DyncamOffsetDelta / Zoom;
	}
	else if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_UsePosition)
	{
		m_aTargetPos[g_Config.m_ClDummy] = GameClient()->m_Snap.m_SpecInfo.m_Position + GetRenderMousePos(g_Config.m_ClDummy);
	}
	else
	{
		m_aTargetPos[g_Config.m_ClDummy] = GetRenderMousePos(g_Config.m_ClDummy);
	}
}

bool CControls::OnCursorMove(float x, float y, IInput::ECursorType CursorType)
{
	if(GameClient()->m_Snap.m_pGameInfoObj && (GameClient()->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_PAUSED))
		return false;

	if(CursorType == IInput::CURSOR_JOYSTICK && g_Config.m_InpControllerAbsolute && GameClient()->m_Snap.m_pGameInfoObj && !GameClient()->m_Snap.m_SpecInfo.m_Active)
	{
		vec2 AbsoluteDirection;
		if(Input()->GetActiveJoystick()->Absolute(&AbsoluteDirection.x, &AbsoluteDirection.y))
		{
			m_aMousePos[g_Config.m_ClDummy] = AbsoluteDirection * GetMaxMouseDistance();
			GameClient()->m_Controls.m_aMouseInputType[g_Config.m_ClDummy] = CControls::EMouseInputType::ABSOLUTE;
		}
		return true;
	}

	float Factor = 1.0f;
	if(g_Config.m_ClDyncam && g_Config.m_ClDyncamMousesens)
	{
		Factor = g_Config.m_ClDyncamMousesens / 100.0f;
	}
	else
	{
		switch(CursorType)
		{
		case IInput::CURSOR_MOUSE:
			Factor = g_Config.m_InpMousesens / 100.0f;
			break;
		case IInput::CURSOR_JOYSTICK:
			Factor = g_Config.m_InpControllerSens / 100.0f;
			break;
		default:
			dbg_assert_failed("CControls::OnCursorMove CursorType %d", (int)CursorType);
		}
	}

	if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId < 0)
		Factor *= GameClient()->m_Camera.m_Zoom;

	m_aMousePos[g_Config.m_ClDummy] += vec2(x, y) * Factor;
	GameClient()->m_Controls.m_aMouseInputType[g_Config.m_ClDummy] = CControls::EMouseInputType::RELATIVE;
	m_aPastaInputActivity[g_Config.m_ClDummy] = time_get();
	ClampMousePos();
	return true;
}

void CControls::ClampMousePos()
{
	if(GameClient()->m_Snap.m_SpecInfo.m_Active && GameClient()->m_Snap.m_SpecInfo.m_SpectatorId < 0)
	{
		m_aMousePos[g_Config.m_ClDummy].x = std::clamp(m_aMousePos[g_Config.m_ClDummy].x, -201.0f * 32, (Collision()->GetWidth() + 201.0f) * 32.0f);
		m_aMousePos[g_Config.m_ClDummy].y = std::clamp(m_aMousePos[g_Config.m_ClDummy].y, -201.0f * 32, (Collision()->GetHeight() + 201.0f) * 32.0f);
	}
	else
	{
		const float MouseMin = GetMinMouseDistance();
		const float MouseMax = GetMaxMouseDistance();

		float MouseDistance = length(m_aMousePos[g_Config.m_ClDummy]);
		if(MouseDistance < 0.001f)
		{
			m_aMousePos[g_Config.m_ClDummy].x = 0.001f;
			m_aMousePos[g_Config.m_ClDummy].y = 0;
			MouseDistance = 0.001f;
		}
		if(MouseDistance < MouseMin)
			m_aMousePos[g_Config.m_ClDummy] = normalize_pre_length(m_aMousePos[g_Config.m_ClDummy], MouseDistance) * MouseMin;
		MouseDistance = length(m_aMousePos[g_Config.m_ClDummy]);
		if(MouseDistance > MouseMax)
			m_aMousePos[g_Config.m_ClDummy] = normalize_pre_length(m_aMousePos[g_Config.m_ClDummy], MouseDistance) * MouseMax;

		if(g_Config.m_TcLimitMouseToScreen)
		{
			float Width, Height;
			Graphics()->CalcScreenParams(Graphics()->ScreenAspect(), 1.0f, &Width, &Height);
			Height /= 2.0f;
			Width /= 2.0f;
			if(g_Config.m_TcLimitMouseToScreen == 2)
				Width = Height;
			m_aMousePos[g_Config.m_ClDummy].y = std::clamp(m_aMousePos[g_Config.m_ClDummy].y, -Height, Height);
			m_aMousePos[g_Config.m_ClDummy].x = std::clamp(m_aMousePos[g_Config.m_ClDummy].x, -Width, Width);
		}
	}
}

float CControls::GetMinMouseDistance() const
{
	return g_Config.m_ClDyncam ? g_Config.m_ClDyncamMinDistance : g_Config.m_ClMouseMinDistance;
}

float CControls::GetMaxMouseDistance() const
{
	float CameraMaxDistance = 200.0f;
	float FollowFactor = (g_Config.m_ClDyncam ? g_Config.m_ClDyncamFollowFactor : g_Config.m_ClMouseFollowfactor) / 100.0f;
	float DeadZone = g_Config.m_ClDyncam ? g_Config.m_ClDyncamDeadzone : g_Config.m_ClMouseDeadzone;
	float MaxDistance = g_Config.m_ClDyncam ? g_Config.m_ClDyncamMaxDistance : g_Config.m_ClMouseMaxDistance;
	return minimum((FollowFactor != 0 ? CameraMaxDistance / FollowFactor + DeadZone : MaxDistance), MaxDistance);
}

bool CControls::CheckNewInput()
{
	bool NewInput[2] = {};
	for(int Dummy = 0; Dummy < NUM_DUMMIES; Dummy++)
	{
		CNetObj_PlayerInput TestInput = m_aInputData[Dummy];
		if(Dummy == g_Config.m_ClDummy)
		{
			TestInput.m_Direction = 0;
			if(m_aInputDirectionLeft[Dummy] && !m_aInputDirectionRight[Dummy])
				TestInput.m_Direction = -1;
			if(!m_aInputDirectionLeft[Dummy] && m_aInputDirectionRight[Dummy])
				TestInput.m_Direction = 1;
		}

		if(m_aFastInput[Dummy].m_Direction != TestInput.m_Direction)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_Hook != TestInput.m_Hook)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_Fire != TestInput.m_Fire)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_Jump != TestInput.m_Jump)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_NextWeapon != TestInput.m_NextWeapon)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_PrevWeapon != TestInput.m_PrevWeapon)
			NewInput[Dummy] = true;
		if(m_aFastInput[Dummy].m_WantedWeapon != TestInput.m_WantedWeapon)
			NewInput[Dummy] = true;

		bool SetMousePos = false;
		// We need to be careful about how we manage the mouse position to avoid mispredicted hooks and fires
		// on the first tick that they activate before we know what mouse position we actually sent to the server
		if(Dummy == g_Config.m_ClDummy)
		{
			if(m_aFastInput[Dummy].m_Hook == 0 && TestInput.m_Hook == 1)
			{
				m_FastInputHookAction = true;
				SetMousePos = true;
			}
			if(m_aFastInput[Dummy].m_Fire != TestInput.m_Fire && TestInput.m_Fire % 2 == 1)
			{
				m_FastInputFireAction = true;
				SetMousePos = true;
			}
			if(!m_FastInputHookAction && !m_FastInputFireAction)
			{
				SetMousePos = true;
			}
		}

		if(SetMousePos)
		{
			TestInput.m_TargetX = (int)m_aMousePos[Dummy].x;
			TestInput.m_TargetY = (int)m_aMousePos[Dummy].y;
		}
		else
		{
			TestInput.m_TargetX = m_aFastInput[Dummy].m_TargetX;
			TestInput.m_TargetY = m_aFastInput[Dummy].m_TargetY;
		}

		m_aFastInput[Dummy] = TestInput;
	}

	if(NewInput[0] || NewInput[1])
		return true;
	else
		return false;
}

vec2 CControls::GetRenderMousePos(int Dummy)
{
	if(Dummy < 0 || Dummy >= NUM_DUMMIES)
		return vec2(0.0f, 0.0f);
	if(Dummy == g_Config.m_ClDummy && GameClient()->m_PastaTas.HasRenderMouseOverride())
		return EnsureValidAim(GameClient()->m_PastaTas.GetRenderMousePos());
	if(m_aPastaRenderMouseOverride[Dummy])
	{
		const int TargetId = m_aPastaRenderMouseTargetId[Dummy];
		if(TargetId >= 0 && GameClient()->m_Snap.m_LocalClientId >= 0 &&
			TargetId < MAX_CLIENTS &&
			GameClient()->m_Snap.m_aCharacters[TargetId].m_Active)
		{
			const vec2 LocalRenderPos = GameClient()->m_aClients[GameClient()->m_Snap.m_LocalClientId].m_RenderPos;
			const vec2 TargetRenderPos = GameClient()->m_aClients[TargetId].m_RenderPos;
			return EnsureValidAim(TargetRenderPos - LocalRenderPos);
		}
		return EnsureValidAim(m_aPastaVisibleMousePos[Dummy]);
	}
	return EnsureValidAim(m_aMousePos[Dummy]);
}