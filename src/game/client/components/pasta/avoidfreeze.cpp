#include "avoidfreeze.h"

#include <engine/engine.h>
#include <engine/shared/config.h>

#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>
#include <game/client/prediction/gameworld.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>
#include <vector>

constexpr int gs_PastaAvoidForceMs = 120;
int64_t gs_PastaLastAvoidTime = 0;

void CAvoidFreeze::OnReset()
{
	std::fill(std::begin(m_BlatantTrackPointInit), std::end(m_BlatantTrackPointInit), false);
	std::fill(std::begin(m_BlatantTrackPoint), std::end(m_BlatantTrackPoint), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_BlatantHookTarget), std::end(m_BlatantHookTarget), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_BlatantHookHoldTicks), std::end(m_BlatantHookHoldTicks), 0);
	std::fill(std::begin(m_BlatantFreezeResetFrozenLast), std::end(m_BlatantFreezeResetFrozenLast), false);
	std::fill(std::begin(m_BlatantFreezeResetSince), std::end(m_BlatantFreezeResetSince), 0);
	std::fill(std::begin(m_AvoidLastMousePos), std::end(m_AvoidLastMousePos), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_AvoidForcing), std::end(m_AvoidForcing), false);
	std::fill(std::begin(m_AvoidForcedDir), std::end(m_AvoidForcedDir), 0);
	std::fill(std::begin(m_AvoidForceUntil), std::end(m_AvoidForceUntil), 0);
	std::fill(std::begin(m_AvoidWasInDanger), std::end(m_AvoidWasInDanger), false);
}

bool CAvoidFreeze::IsFreezeTile(int Tile) const
{
	return Tile == TILE_FREEZE || Tile == TILE_DFREEZE || Tile == TILE_LFREEZE;
}

bool CAvoidFreeze::HasFreezeTile(vec2 Pos) const
{
	const int MapIndex = GameClient()->Collision()->GetPureMapIndex(Pos);
	if(MapIndex < 0)
		return false;

	return IsFreezeTile(GameClient()->Collision()->GetTileIndex(MapIndex)) || IsFreezeTile(GameClient()->Collision()->GetFrontTileIndex(MapIndex));
}

bool CAvoidFreeze::IsDangerousMapIndex(int MapIndex, bool DetectFreeze, bool DetectDeath, bool DetectTele)
{
	if(MapIndex < 0)
		return false;

	if(DetectFreeze)
	{
		const int Tile = GameClient()->Collision()->GetTileIndex(MapIndex);
		const int FrontTile = GameClient()->Collision()->GetFrontTileIndex(MapIndex);
		if(IsFreezeTile(Tile) || IsFreezeTile(FrontTile))
			return true;
	}

	if(DetectDeath)
	{
		if(GameClient()->Collision()->GetTileIndex(MapIndex) == TILE_DEATH || GameClient()->Collision()->GetFrontTileIndex(MapIndex) == TILE_DEATH)
			return true;
	}

	if(DetectTele)
	{
		if(GameClient()->Collision()->IsTeleport(MapIndex) || GameClient()->Collision()->IsEvilTeleport(MapIndex) || GameClient()->Collision()->IsCheckTeleport(MapIndex) ||
			GameClient()->Collision()->IsCheckEvilTeleport(MapIndex) || GameClient()->Collision()->IsTeleportWeapon(MapIndex) || GameClient()->Collision()->IsTeleportHook(MapIndex) ||
			GameClient()->Collision()->IsTeleCheckpoint(MapIndex))
			return true;
	}

	return false;
}

bool CAvoidFreeze::PastaTryMove(const CNetObj_PlayerInput &BaseInput, int Direction, int CheckTicks)
{
	CNetObj_PlayerInput ModifiedInput = BaseInput;
	if(BaseInput.m_Direction != Direction)
	{
		const int Sensitivity = PastaDirectionSensitivityStep();
		if(Direction > BaseInput.m_Direction)
			ModifiedInput.m_Direction = minimum(BaseInput.m_Direction + Sensitivity, Direction);
		else
			ModifiedInput.m_Direction = maximum(BaseInput.m_Direction - Sensitivity, Direction);
	}

	if(!PastaPredictFreeze(ModifiedInput, CheckTicks))
	{
		const int Local = g_Config.m_ClDummy;
		GameClient()->m_AvoidFreeze.m_AvoidForcing[Local] = true;
		GameClient()->m_AvoidFreeze.m_AvoidForcedDir[Local] = ModifiedInput.m_Direction;
		GameClient()->m_AvoidFreeze.m_AvoidForceUntil[Local] = time_get() + static_cast<int64_t>(gs_PastaAvoidForceMs) * time_freq() / 1000;
		return true;
	}
	return false;
}

bool CAvoidFreeze::PastaTryAvoidFreeze(int LocalPlayerId)
{
	const int CheckTicks = PastaAvoidPredictTicks();
	const int MaxAttempts = PastaMaxAvoidAttempts();
	const int MaxAttemptsPerDirection = PastaMaxAvoidAttemptsPerDirection();
	const CNetObj_PlayerInput BaseInput = GameClient()->m_Controls.m_aInputData[LocalPlayerId];
	const int ClientId = GameClient()->m_aLocalIds[LocalPlayerId];

	const CCharacter *pChar = GameClient()->m_PredictedWorld.GetCharacterById(ClientId);
	vec2 DangerDir(0.0f, 0.0f);
	if(pChar && pChar->Core())
	{
		int DangerTick = -1;
		float DangerDistance = 0.0f;
		if(PastaPredictFreeze(BaseInput, CheckTicks, &DangerTick, &DangerDistance))
		{
			static CGameWorld s_World;
			s_World.CopyWorldClean(&GameClient()->m_PredictedWorld);
			CCharacter *pTestChar = s_World.GetCharacterById(ClientId);
			if(pTestChar)
			{
				pTestChar->OnDirectInput(&BaseInput);
				pTestChar->OnDirectInput(&BaseInput);
				for(int i = 0; i < minimum(DangerTick, CheckTicks); i++)
				{
					pTestChar->OnPredictedInput(&BaseInput);
					s_World.m_GameTick++;
					s_World.Tick();
				}
				const vec2 DangerPos = pTestChar->m_Pos;
				const vec2 CurrentPos = pChar->Core()->m_Pos;
				const vec2 DirVec = DangerPos - CurrentPos;
				const float DirLen = length(DirVec);
				if(DirLen > 0.1f)
					DangerDir = DirVec / DirLen;
			}
		}
	}

	std::array<int, 3> aDirections = {-1, 1, 0};
	if(std::fabs(DangerDir.x) > 0.1f)
	{
		const int AwayDir = DangerDir.x > 0.0f ? -1 : 1;
		const int TowardDir = DangerDir.x > 0.0f ? 1 : -1;
		aDirections = {AwayDir, TowardDir, 0};
	}

	std::array<int, 3> aDirectionAttempts = {0, 0, 0};
	int Attempts = 0;
	while(Attempts < MaxAttempts)
	{
		bool TriedAny = false;
		for(size_t i = 0; i < aDirections.size() && Attempts < MaxAttempts; i++)
		{
			if(aDirectionAttempts[i] >= MaxAttemptsPerDirection)
				continue;

			const int Direction = aDirections[i];
			if(Direction == BaseInput.m_Direction && aDirectionAttempts[i] == 0)
			{
				aDirectionAttempts[i]++;
				continue;
			}

			CNetObj_PlayerInput AttemptInput = BaseInput;
			if(aDirectionAttempts[i] > 0 && AttemptInput.m_Hook != 0)
				AttemptInput.m_Hook = 0;

			const int AttemptCheckTicks = maximum(1, CheckTicks - aDirectionAttempts[i]);
			aDirectionAttempts[i]++;
			Attempts++;
			TriedAny = true;

			if(PastaTryMove(AttemptInput, Direction, AttemptCheckTicks))
			{
				CNetObj_PlayerInput VerifyInput = AttemptInput;
				if(VerifyInput.m_Direction != Direction)
				{
					const int Sensitivity = PastaDirectionSensitivityStep();
					if(Direction > VerifyInput.m_Direction)
						VerifyInput.m_Direction = minimum(VerifyInput.m_Direction + Sensitivity, Direction);
					else
						VerifyInput.m_Direction = maximum(VerifyInput.m_Direction - Sensitivity, Direction);
				}

				if(!PastaPredictFreeze(VerifyInput, minimum(AttemptCheckTicks + 2, 15)))
					return true;

				GameClient()->m_AvoidFreeze.m_AvoidForcing[LocalPlayerId] = false;
			}
		}

		if(!TriedAny)
			break;
	}

	return false;
}

int CAvoidFreeze::PastaAvoidPredictTicks() const
{
	return std::clamp(g_Config.m_PastaAvoidLegitCheckTicks, 1, 100);
}

int CAvoidFreeze::PastaHookAssistTicks() const
{
	return std::clamp(g_Config.m_PastaAvoidLegitUnfreeze ? g_Config.m_PastaAvoidLegitUnfreezeTicks : g_Config.m_PastaAvoidLegitCheckTicks, 1, 100);
}

int CAvoidFreeze::PastaDirectionSensitivityStep() const
{
	return g_Config.m_PastaAvoidLegitDirectionPriority >= 667 ? 2 : 1;
}

float CAvoidFreeze::PastaDirectionSensitivityFactor() const
{
	return std::clamp(g_Config.m_PastaAvoidLegitDirectionPriority / 1000.0f, 0.10f, 1.0f);
}

int CAvoidFreeze::PastaMaxAvoidAttempts() const
{
	return std::clamp(g_Config.m_PastaAvoidLegitQuality, 1, 100);
}

int CAvoidFreeze::PastaMaxAvoidAttemptsPerDirection() const
{
	return std::clamp(maximum(1, g_Config.m_PastaAvoidLegitRandomness), 1, 100);
}

bool CAvoidFreeze::PastaGetFreeze(vec2 Pos, int FreezeTime) const
{
	if(FreezeTime > 0)
		return true;

	const float CheckOffset = CCharacterCore::PhysicalSize() / 3.0f;
	const vec2 aCheckPositions[] = {
		Pos,
		{Pos.x + CheckOffset, Pos.y - CheckOffset},
		{Pos.x + CheckOffset, Pos.y + CheckOffset},
		{Pos.x - CheckOffset, Pos.y - CheckOffset},
		{Pos.x - CheckOffset, Pos.y + CheckOffset},
	};

	for(const vec2 &CheckPos : aCheckPositions)
	{
		const int MapIndex = Collision()->GetPureMapIndex(CheckPos.x, CheckPos.y);
		if(MapIndex < 0)
			continue;

		if(IsFreezeTile(Collision()->GetTileIndex(MapIndex)) || IsFreezeTile(Collision()->GetFrontTileIndex(MapIndex)))
			return true;
		if(g_Config.m_PastaAvoidLegitDeath &&
			(Collision()->GetTileIndex(MapIndex) == TILE_DEATH || Collision()->GetFrontTileIndex(MapIndex) == TILE_DEATH))
			return true;
		if(g_Config.m_PastaAvoidLegitTeles &&
			(Collision()->IsTeleport(MapIndex) || Collision()->IsCheckEvilTeleport(MapIndex) || Collision()->IsCheckTeleport(MapIndex) ||
				Collision()->IsEvilTeleport(MapIndex) || Collision()->IsTeleportWeapon(MapIndex) || Collision()->IsTeleportHook(MapIndex) ||
				Collision()->IsTeleCheckpoint(MapIndex)))
			return true;
	}

	return false;
}

bool CAvoidFreeze::PastaAutomationAllowed() const
{
	if(!GameClient() || !GameClient()->Predict())
		return false;
	if(GameClient()->m_Snap.m_SpecInfo.m_Active || GameClient()->m_Chat.IsActive() || GameClient()->m_Menus.IsActive())
		return false;
	return true;
}

void CAvoidFreeze::PastaAvoidFreeze()
{
	if(!g_Config.m_PastaAvoidfreeze || g_Config.m_PastaAvoidMode != 1 || !g_Config.m_PastaAvoidLegitDirection)
		return;
	if(!PastaAutomationAllowed())
		return;

	const int64_t CurrentTime = time_get();
	if(!PastaIsAvoidCooldownElapsed(CurrentTime))
		return;

	const int LocalPlayerId = g_Config.m_ClDummy;
	if(GameClient()->m_AvoidFreeze.m_AvoidForcing[LocalPlayerId] && CurrentTime <= GameClient()->m_AvoidFreeze.m_AvoidForceUntil[LocalPlayerId])
		return;
	if(!GameClient()->m_Controls.IsPlayerActive(LocalPlayerId))
		return;

	int DangerTick = -1;
	float DangerDistance = 0.0f;
	const bool InDanger = PastaPredictFreeze(GameClient()->m_Controls.m_aInputData[LocalPlayerId], PastaAvoidPredictTicks(), &DangerTick, &DangerDistance);
	if(!InDanger)
	{
		GameClient()->m_AvoidFreeze.m_AvoidWasInDanger[LocalPlayerId] = false;
		return;
	}

	const int Level = std::clamp(PastaAvoidPredictTicks(), 1, 5);
	const float SensitivityFactor = PastaDirectionSensitivityFactor();
	const float BaseTickLimit = Level <= 2 ? (PastaAvoidPredictTicks() / 4.0f) :
						 (Level <= 4 ? (PastaAvoidPredictTicks() / 3.0f) : (PastaAvoidPredictTicks() / 2.0f));
	const float SensitivityAdjustment = 0.5f + (0.5f * SensitivityFactor);
	const int CloseTickLimit = maximum(1, (int)(BaseTickLimit * SensitivityAdjustment));

	float BaseDistanceThreshold = 48.0f;
	if(Level == 2)
		BaseDistanceThreshold = 64.0f;
	else if(Level == 3)
		BaseDistanceThreshold = 80.0f;
	else if(Level == 4)
		BaseDistanceThreshold = 96.0f;
	else if(Level >= 5)
		BaseDistanceThreshold = 112.0f;

	const float DistanceThreshold = BaseDistanceThreshold * (0.6f + (0.8f * SensitivityFactor));
	if(DangerTick > CloseTickLimit && DangerDistance > DistanceThreshold)
	{
		GameClient()->m_AvoidFreeze.m_AvoidWasInDanger[LocalPlayerId] = false;
		return;
	}

	if(GameClient()->m_AvoidFreeze.m_AvoidWasInDanger[LocalPlayerId])
		return;

	if(PastaTryAvoidFreeze(LocalPlayerId))
	{
		m_AvoidWasInDanger[LocalPlayerId] = true;
		PastaUpdateAvoidCooldown(CurrentTime);
	}
}

void CAvoidFreeze::PastaHookAssist()
{
	if(!g_Config.m_PastaAvoidfreeze || g_Config.m_PastaAvoidMode != 1 || !g_Config.m_PastaAvoidLegitHook)
		return;
	if(!PastaAutomationAllowed())
		return;

	static int s_aLastHookTick[NUM_DUMMIES] = {-1, -1};
	const int Local = g_Config.m_ClDummy;
	const int CurrentPredTick = GameClient()->m_PredictedWorld.GameTick();
	if(CurrentPredTick == s_aLastHookTick[Local])
		return;
	s_aLastHookTick[Local] = CurrentPredTick;

	const int ClientId = GameClient()->m_aLocalIds[Local];
	const CCharacter *pChar = ClientId >= 0 ? GameClient()->m_PredictedWorld.GetCharacterById(ClientId) : nullptr;
	const CCharacterCore *pCore = pChar ? pChar->Core() : nullptr;
	if(!pChar || !pCore)
		return;

	const bool AimingUp = GameClient()->m_Controls.m_aInputData[Local].m_TargetY < 0;
	const int BaseCheckTicks = PastaHookAssistTicks();
	const int CheckTicks = maximum(1, BaseCheckTicks + ((BaseCheckTicks >= 1 && BaseCheckTicks <= 3 && AimingUp) ? 1 : 0));
	const float SensitivityFactor = std::clamp(BaseCheckTicks / 10.0f, 0.2f, 1.0f);
	const bool CurrentlyHoldingHook = GameClient()->m_Controls.m_aInputData[Local].m_Hook != 0;
	const float Speed = length(pCore->m_Vel);

	if(CurrentlyHoldingHook)
	{
		CNetObj_PlayerInput TestInput = GameClient()->m_Controls.m_aInputData[Local];
		TestInput.m_Hook = 1;

		int DangerTick = -1;
		float DangerDistance = 0.0f;
		if(PastaPredictFreeze(TestInput, CheckTicks, &DangerTick, &DangerDistance))
		{
			CNetObj_PlayerInput ReleaseInput = GameClient()->m_Controls.m_aInputData[Local];
			ReleaseInput.m_Hook = 0;
			const bool CanAvoidByReleasing = !PastaPredictFreeze(ReleaseInput, CheckTicks);
			if(!CanAvoidByReleasing && DangerTick > CheckTicks / 2)
				return;

			const float SpeedFactor = minimum(1.0f, Speed / 10.0f);
			const float UrgencyMultiplier = 0.3f + (0.7f * SensitivityFactor) - (0.2f * SpeedFactor);
			const float DistanceMultiplier = 0.5f + (1.5f * SensitivityFactor) + (0.5f * SpeedFactor);
			const int HookUrgencyTicks = maximum(1, (int)(CheckTicks * UrgencyMultiplier));
			const float HookDistanceThreshold = 32.0f * (1.5f + BaseCheckTicks * DistanceMultiplier);
			if((DangerTick <= HookUrgencyTicks || DangerDistance <= HookDistanceThreshold) && CanAvoidByReleasing)
				GameClient()->m_Controls.m_aInputData[Local].m_Hook = 0;
		}
	}
	else
	{
		CNetObj_PlayerInput TestInput = GameClient()->m_Controls.m_aInputData[Local];
		TestInput.m_Hook = 1;

		int DangerTick = -1;
		float DangerDistance = 0.0f;
		if(PastaPredictFreeze(TestInput, CheckTicks, &DangerTick, &DangerDistance))
		{
			CNetObj_PlayerInput NoHookInput = GameClient()->m_Controls.m_aInputData[Local];
			NoHookInput.m_Hook = 0;
			const bool CanMoveWithoutHook = !PastaPredictFreeze(NoHookInput, CheckTicks);
			const float UrgencyMultiplier = 0.2f + (0.6f * SensitivityFactor);
			const int HookUrgencyTicks = maximum(1, (int)(CheckTicks * UrgencyMultiplier));
			if(DangerTick <= HookUrgencyTicks && DangerDistance < 48.0f && CanMoveWithoutHook)
				GameClient()->m_Controls.m_aInputData[Local].m_Hook = 0;
		}
	}
}

bool CAvoidFreeze::PastaIsAvoidCooldownElapsed(int64_t CurrentTime) const
{
	const int64_t ConfiguredDelay = static_cast<int64_t>(maximum(0, g_Config.m_PastaAvoidLegitAfkProtection ? g_Config.m_PastaAvoidLegitAfkTime * 100 : 0)) * time_freq() / 1000;
	if(gs_PastaLastAvoidTime == 0)
		return true;
	return CurrentTime - gs_PastaLastAvoidTime >= ConfiguredDelay;
}

void CAvoidFreeze::PastaUpdateAvoidCooldown(int64_t CurrentTime)
{
	gs_PastaLastAvoidTime = CurrentTime + static_cast<int64_t>(gs_PastaAvoidForceMs) * time_freq() / 1000;
}

bool CAvoidFreeze::PastaPredictFreeze(const CNetObj_PlayerInput &Input, int Ticks, int *pDangerTick, float *pDangerDistance) const
{
	if(!GameClient()->Predict())
	{
		if(pDangerTick)
			*pDangerTick = -1;
		if(pDangerDistance)
			*pDangerDistance = 0.0f;
		return false;
	}

	static CGameWorld s_World;
	s_World.CopyWorldClean(&GameClient()->m_PredictedWorld);

	const int Local = g_Config.m_ClDummy;
	const int ClientId = GameClient()->m_aLocalIds[Local];
	CCharacter *pChar = s_World.GetCharacterById(ClientId);
	if(!pChar)
	{
		if(pDangerTick)
			*pDangerTick = -1;
		if(pDangerDistance)
			*pDangerDistance = 0.0f;
		return false;
	}

	const vec2 StartPos = pChar->m_Pos;
	pChar->OnDirectInput(&Input);
	pChar->OnDirectInput(&Input);

	const int Steps = maximum(1, Ticks);
	for(int i = 0; i < Steps; i++)
	{
		pChar->OnPredictedInput(&Input);
		s_World.m_GameTick++;
		s_World.Tick();
		if(PastaGetFreeze(pChar->m_Pos, pChar->m_FreezeTime))
		{
			if(pDangerTick)
				*pDangerTick = i + 1;
			if(pDangerDistance)
				*pDangerDistance = distance(StartPos, pChar->m_Pos);
			return true;
		}
	}

	if(pDangerTick)
		*pDangerTick = -1;
	if(pDangerDistance)
		*pDangerDistance = 0.0f;
	return false;
}

bool CAvoidFreeze::PastaPredictFreezeMT(const std::vector<CNetObj_PlayerInput> &vInputs, int Ticks,
	int *pOutDangerTick, float *pOutDangerDistance, int *pOutInputIndex)
{
	if(!GameClient()->Predict())
	{
		if(pOutDangerTick)
			*pOutDangerTick = -1;
		if(pOutDangerDistance)
			*pOutDangerDistance = 0.0f;
		if(pOutInputIndex)
			*pOutInputIndex = -1;
		return false;
	}

	if(vInputs.empty())
	{
		if(pOutDangerTick)
			*pOutDangerTick = -1;
		if(pOutDangerDistance)
			*pOutDangerDistance = 0.0f;
		if(pOutInputIndex)
			*pOutInputIndex = -1;
		return false;
	}

	std::vector<std::shared_ptr<CPredictFreezeJob>> vJobs;
	std::vector<int> vDangerTicks(vInputs.size(), -1);
	std::vector<float> vDangerDistances(vInputs.size(), 0.0f);
	vJobs.reserve(vInputs.size());

	for(size_t i = 0; i < vInputs.size(); ++i)
	{
		auto pJob = std::make_shared<CPredictFreezeJob>(
			this, GameClient(), vInputs[i], Ticks,
			&vDangerTicks[i], &vDangerDistances[i], &m_Mutex);
		vJobs.push_back(pJob);
		Kernel()->RequestInterface<IEngine>()->AddJob(std::shared_ptr<IJob>(pJob));
	}

	// Wait for all jobs to complete
	for(auto &pJob : vJobs)
	{
		while(!pJob->IsFinished())
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	for(size_t i = 0; i < vInputs.size(); ++i)
	{
		if(vDangerTicks[i] >= 0)
		{
			if(pOutDangerTick)
				*pOutDangerTick = vDangerTicks[i];
			if(pOutDangerDistance)
				*pOutDangerDistance = vDangerDistances[i];
			if(pOutInputIndex)
				*pOutInputIndex = (int)i;
			return true;
		}
	}

	if(pOutDangerTick)
		*pOutDangerTick = -1;
	if(pOutDangerDistance)
		*pOutDangerDistance = 0.0f;
	if(pOutInputIndex)
		*pOutInputIndex = -1;
	return false;
}

bool CAvoidFreeze::HasDangerousTile(vec2 Pos, bool DetectFreeze, bool DetectDeath, bool DetectTele)
{
	return IsDangerousMapIndex(GameClient()->Collision()->GetPureMapIndex(Pos), DetectFreeze, DetectDeath, DetectTele);
}

bool CAvoidFreeze::HasFreezeAbove(vec2 Pos, int Tiles)
{
	for(int Tile = 1; Tile <= Tiles; ++Tile)
	{
		if(HasFreezeTile(Pos + vec2(0.0f, -Tile * 32.0f)))
			return true;
	}
	return false;
}

int CAvoidFreeze::DetectFreezeSide(vec2 Pos)
{
	const float SideOffset = CCharacterCore::PhysicalSizeVec2().x * 0.52f + 1.5f;
	constexpr float aSamplesY[] = {-8.0f, 0.0f, 8.0f};
	bool FreezeRight = false;
	bool FreezeLeft = false;
	for(const float SampleY : aSamplesY)
	{
		FreezeRight |= HasFreezeTile(Pos + vec2(SideOffset, SampleY));
		FreezeLeft |= HasFreezeTile(Pos + vec2(-SideOffset, SampleY));
	}

	if(FreezeRight == FreezeLeft)
		return 0;
	return FreezeRight ? 1 : -1;
}

bool CAvoidFreeze::ShouldAutoJumpSave(CCharacter *pCharacter)
{
	if(pCharacter == nullptr || pCharacter->IsGrounded() || pCharacter->Core()->m_Vel.y <= 4.4f)
		return false;
	if(pCharacter->GetJumped() > 1)
		return false;

	const vec2 Pos = pCharacter->GetPos();
	if(HasFreezeAbove(Pos, 4))
		return false;

	const float FootOffset = CCharacterCore::PhysicalSizeVec2().y * 0.5f + 2.0f;
	for(int Step = 0; Step < 2; ++Step)
	{
		const float Lookahead = FootOffset + (Step + 1) * 4.0f + pCharacter->Core()->m_Vel.y * (0.18f + 0.06f * Step);
		if(HasFreezeTile(Pos + vec2(0.0f, Lookahead)))
			return true;
	}

	return false;
}

bool CAvoidFreeze::HasPlayerNearAim(const CCharacter *pLocalCharacter, vec2 AimDir, float MaxDistance, float MaxAngleRadians)
{
	if(pLocalCharacter == nullptr || length(AimDir) < 0.001f)
		return false;

	const vec2 Origin = pLocalCharacter->GetPos();
	const vec2 NormalizedAim = normalize(AimDir);
	const float CosThreshold = std::cos(MaxAngleRadians);
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		if(ClientId == GameClient()->m_Snap.m_LocalClientId || !GameClient()->m_Snap.m_aCharacters[ClientId].m_Active)
			continue;

		const CCharacter *pTarget = GameClient()->m_PredictedWorld.GetCharacterById(ClientId);
		const vec2 TargetPos = pTarget != nullptr ? pTarget->GetPos() : GameClient()->m_aClients[ClientId].m_RenderPos;
		const vec2 ToTarget = TargetPos - Origin;
		const float Dist = length(ToTarget);
		if(Dist < 1.0f || Dist > MaxDistance)
			continue;
		if(dot(normalize(ToTarget), NormalizedAim) >= CosThreshold)
			return true;
	}
	return false;
}

bool CAvoidFreeze::HasImmediateSideFreeze(const CCharacter *pCharacter, int Direction)
{
	if(pCharacter == nullptr || Direction == 0)
		return false;

	const vec2 Pos = pCharacter->GetPos();
	const float SideOffset = CCharacterCore::PhysicalSizeVec2().x * 0.5f + 1.5f;
	constexpr float aSampleY[] = {-8.0f, 0.0f, 8.0f};
	for(int Step = 0; Step < 3; ++Step)
	{
		const float SampleX = Pos.x + Direction * (SideOffset + Step * 4.0f);
		for(const float OffsetY : aSampleY)
		{
			if(HasFreezeTile(vec2(SampleX, Pos.y + OffsetY)))
				return true;
		}
	}
	return false;
}

bool CAvoidFreeze::IsSolidTileAt(int TileX, int TileY)
{
	if(TileX < 0 || TileY < 0 || TileX >= GameClient()->Collision()->GetWidth() || TileY >= GameClient()->Collision()->GetHeight())
		return false;
	const int Index = TileY * GameClient()->Collision()->GetWidth() + TileX;
	const int Tile = GameClient()->Collision()->GetTileIndex(Index);
	const int FrontTile = GameClient()->Collision()->GetFrontTileIndex(Index);
	return Tile == TILE_SOLID || FrontTile == TILE_SOLID;
}

bool CAvoidFreeze::FindBlatantHookTarget(vec2 Pos, vec2 Vel, vec2 GoalPos, float HookLength, bool DangerAhead, vec2 &OutTarget)
{
	const int TileRadius = maximum(2, (int)std::ceil(HookLength / 32.0f) + 1);
	const int CenterTileX = std::clamp((int)std::floor(Pos.x / 32.0f), 0, GameClient()->Collision()->GetWidth() - 1);
	const int CenterTileY = std::clamp((int)std::floor(Pos.y / 32.0f), 0, GameClient()->Collision()->GetHeight() - 1);
	const vec2 ToGoal = length(GoalPos - Pos) > 0.001f ? normalize(GoalPos - Pos) : vec2(1.0f, 0.0f);
	const float GoalSign = GoalPos.x >= Pos.x ? 1.0f : -1.0f;
	const float Speed = length(Vel);

	float BestScore = -std::numeric_limits<float>::infinity();
	vec2 BestTarget(0.0f, 0.0f);

	for(int TileY = CenterTileY - TileRadius; TileY <= CenterTileY + TileRadius; ++TileY)
	{
		for(int TileX = CenterTileX - TileRadius; TileX <= CenterTileX + TileRadius; ++TileX)
		{
			if(!IsSolidTileAt(TileX, TileY))
				continue;

			const vec2 TilePos(TileX * 32.0f + 32.0f * 0.5f, TileY * 32.0f + 32.0f * 0.5f);
			const vec2 Delta = TilePos - Pos;
			const float Dist = length(Delta);
			if(Dist < 20.0f || Dist > HookLength)
				continue;

			vec2 CollisionPos;
			vec2 BeforeCollision;
			if(GameClient()->Collision()->IntersectLine(Pos, TilePos, &CollisionPos, &BeforeCollision) == TILE_NOHOOK)
				continue;
			if(distance(CollisionPos, TilePos) > 24.0f && distance(BeforeCollision, TilePos) > 24.0f)
				continue;

			const vec2 Dir = normalize(Delta);
			const float PullUp = maximum(0.0f, -Dir.y);
			const float PullDown = maximum(0.0f, Dir.y);
			const float HorizontalTowardGoal = Dir.x * GoalSign;
			const float GoalAlignment = dot(Dir, ToGoal);
			float Score =
				GoalAlignment * 5.5f +
				HorizontalTowardGoal * 7.0f +
				PullUp * 7.5f -
				PullDown * 8.5f -
				(Dist / HookLength) * 1.75f;
			if(TilePos.y < Pos.y - 12.0f)
				Score += 2.5f;
			if(TilePos.y > Pos.y + 12.0f)
				Score -= 3.0f;
			if(std::abs(Dir.x) < 0.10f)
				Score -= 1.5f;
			if(DangerAhead && PullUp > 0.15f)
				Score += 4.5f;
			if(Speed > 2.5f)
			{
				const vec2 VelDir = normalize(Vel);
				Score += dot(VelDir, Dir) * 1.5f;
				if(dot(VelDir, Dir) < -0.25f)
					Score -= 4.0f;
			}

			if(Score > BestScore)
			{
				BestScore = Score;
				BestTarget = TilePos;
			}
		}
	}

	if(BestScore == -std::numeric_limits<float>::infinity())
		return false;

	OutTarget = BestTarget;
	return true;
}

bool CAvoidFreeze::HasImpendingFreeze(const CCharacter *pCharacter, int Ticks)
{
	if(pCharacter == nullptr)
		return false;

	const vec2 Pos = pCharacter->GetPos();
	const vec2 Vel = pCharacter->Core()->m_Vel;
	const float FootOffset = CCharacterCore::PhysicalSizeVec2().y * 0.5f + 4.0f;
	for(int Tick = 1; Tick <= maximum(1, Ticks); ++Tick)
	{
		const float Scale = Tick * 0.95f;
		const vec2 FuturePos = Pos + Vel * Scale;
		if(HasFreezeTile(FuturePos + vec2(0.0f, FootOffset)) ||
			HasFreezeTile(FuturePos + vec2(0.0f, 0.0f)))
			return true;
	}
	return false;
}

bool CAvoidFreeze::HasBlatantDangerAhead(const CCharacter *pCharacter, int Direction, int CheckTicks, bool DetectFreeze, bool DetectDeath, bool DetectTele, bool DetectUnfreeze)
{
	if(pCharacter == nullptr)
		return false;

	const vec2 Pos = pCharacter->GetPos();
	const vec2 Vel = pCharacter->Core()->m_Vel;
	for(int Tick = 1; Tick <= maximum(1, CheckTicks); ++Tick)
	{
		vec2 FuturePos = Pos + Vel * (0.65f * Tick);
		FuturePos.x += Direction * Tick * 4.0f;
		const int MapIndex = GameClient()->Collision()->GetPureMapIndex(FuturePos);
		if(MapIndex < 0)
			return true;
		if(IsDangerousMapIndex(MapIndex, DetectFreeze || DetectUnfreeze, DetectDeath, DetectTele))
			return true;
	}

	return false;
}

int CAvoidFreeze::GetBlatantDangerTick(const CCharacter *pCharacter, int Direction, int CheckTicks, bool DetectFreeze, bool DetectDeath, bool DetectTele, bool DetectUnfreeze)
{
	if(pCharacter == nullptr)
		return -1;

	const vec2 Pos = pCharacter->GetPos();
	const vec2 Vel = pCharacter->Core()->m_Vel;
	for(int Tick = 1; Tick <= maximum(1, CheckTicks); ++Tick)
	{
		vec2 FuturePos = Pos + Vel * (0.65f * Tick);
		FuturePos.x += Direction * Tick * 4.0f;
		const int MapIndex = GameClient()->Collision()->GetPureMapIndex(FuturePos);
		if(MapIndex < 0)
			return Tick;
		if(IsDangerousMapIndex(MapIndex, DetectFreeze || DetectUnfreeze, DetectDeath, DetectTele))
			return Tick;
	}

	return -1;
}

int CAvoidFreeze::SimulateBlatantFreezeTick(const CNetObj_PlayerInput &BaseInput, int Ticks, bool DetectFreeze, bool DetectDeath, bool DetectTele, bool DetectUnfreeze)
{
	int LocalId = GameClient()->m_Snap.m_LocalClientId;
	CGameWorld SimWorld;
	SimWorld.CopyWorldClean(&GameClient()->m_PredictedWorld);
	CCharacter *pSimChar = SimWorld.GetCharacterById(LocalId);
	if(pSimChar == nullptr)
		return 0;

	for(int Tick = 1; Tick <= Ticks; ++Tick)
	{
		pSimChar->OnDirectInput(&BaseInput);
		pSimChar->OnDirectInput(&BaseInput);
		pSimChar->OnPredictedInput(&BaseInput);
		SimWorld.m_GameTick += 1;
		SimWorld.Tick();
		pSimChar = SimWorld.GetCharacterById(LocalId);
		if(pSimChar == nullptr)
			return Tick;

		const vec2 Pos = pSimChar->GetPos();
		const int MapIndex = SimWorld.Collision()->GetPureMapIndex(Pos);
		if(MapIndex < 0)
			return Tick;
		if((DetectFreeze || DetectUnfreeze) && (pSimChar->m_FreezeTime > 0 || pSimChar->Core()->m_DeepFrozen || pSimChar->Core()->m_LiveFrozen))
			return Tick;
		if(IsDangerousMapIndex(MapIndex, DetectFreeze || DetectUnfreeze, DetectDeath, DetectTele))
			return Tick;
	}

	return -1;
}

int CAvoidFreeze::SimulateBlatantFreezeTickMT(const CNetObj_PlayerInput &BaseInput, int Ticks,
	bool DetectFreeze, bool DetectDeath, bool DetectTele, bool DetectUnfreeze,
	const std::vector<CNetObj_PlayerInput> &vInputs)
{
	int LocalId = GameClient()->m_Snap.m_LocalClientId;
	if(vInputs.empty())
		return SimulateBlatantFreezeTick(BaseInput, Ticks, DetectFreeze, DetectDeath, DetectTele, DetectUnfreeze);

	std::vector<std::shared_ptr<CSimulateBlatantFreezeJob>> vJobs;
	vJobs.reserve(vInputs.size());

	for(const auto &Input : vInputs)
	{
		auto pJob = std::make_shared<CSimulateBlatantFreezeJob>(
			this, GameClient(), LocalId, Input, Ticks,
			DetectFreeze, DetectDeath, DetectTele, DetectUnfreeze, &m_Mutex);
		vJobs.push_back(pJob);
		Kernel()->RequestInterface<IEngine>()->AddJob(std::shared_ptr<IJob>(pJob));
	}

	// Wait for all jobs to complete
	for(auto &pJob : vJobs)
	{
		while(!pJob->IsFinished())
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}

	int BestResult = -1;
	for(auto &pJob : vJobs)
	{
		int Result = pJob->GetResult();
		if(Result < 0)
			return -1; // Found a safe input
		if(BestResult < 0 || Result < BestResult)
			BestResult = Result;
	}

	return BestResult;
}

bool CAvoidFreeze::IsBlatantFrozenAtPos(vec2 Pos)
{
	const int Index = GameClient()->Collision()->GetPureMapIndex(Pos);
	if(Index < 0)
		return false;
	const int aTiles[] = {
		GameClient()->Collision()->GetTileIndex(Index),
		GameClient()->Collision()->GetFrontTileIndex(Index),
		GameClient()->Collision()->GetSwitchType(Index)};
	for(const int Tile : aTiles)
	{
		if(Tile == TILE_FREEZE || Tile == TILE_DFREEZE || Tile == TILE_LFREEZE)
			return true;
	}
	return false;
}

CAvoidFreeze::CPredictFreezeJob::CPredictFreezeJob(CAvoidFreeze *pAvoidFreeze, CGameClient *pGameClient, const CNetObj_PlayerInput &Input, int Ticks,
	int *pOutDangerTick, float *pOutDangerDistance, std::mutex *pMutex) :
	m_pAvoidFreeze(pAvoidFreeze), m_pGameClient(pGameClient), m_Input(Input), m_Ticks(Ticks),
	m_pOutDangerTick(pOutDangerTick), m_pOutDangerDistance(pOutDangerDistance), m_pMutex(pMutex), m_Finished(false)
{
	if(m_pOutDangerTick)
		*m_pOutDangerTick = -1;
	if(m_pOutDangerDistance)
		*m_pOutDangerDistance = 0.0f;
}

void CAvoidFreeze::CPredictFreezeJob::Run()
{
	std::lock_guard<std::mutex> Lock(*m_pMutex);

	if(!m_pGameClient->Predict())
	{
		m_Finished = true;
		return;
	}

	CGameWorld s_World;
	s_World.CopyWorldClean(&m_pGameClient->m_PredictedWorld);

	const int Local = g_Config.m_ClDummy;
	const int ClientId = m_pGameClient->m_aLocalIds[Local];
	CCharacter *pChar = s_World.GetCharacterById(ClientId);
	if(!pChar)
	{
		m_Finished = true;
		return;
	}

	const vec2 StartPos = pChar->m_Pos;
	pChar->OnDirectInput(&m_Input);
	pChar->OnDirectInput(&m_Input);

	const int Steps = maximum(1, m_Ticks);
	for(int i = 0; i < Steps; i++)
	{
		pChar->OnPredictedInput(&m_Input);
		s_World.m_GameTick++;
		s_World.Tick();
		if(m_pAvoidFreeze->PastaGetFreeze(pChar->m_Pos, pChar->m_FreezeTime))
		{
			if(m_pOutDangerTick)
				*m_pOutDangerTick = i + 1;
			if(m_pOutDangerDistance)
				*m_pOutDangerDistance = distance(StartPos, pChar->m_Pos);
			m_Finished = true;
			return;
		}
	}

	if(m_pOutDangerTick)
		*m_pOutDangerTick = -1;
	if(m_pOutDangerDistance)
		*m_pOutDangerDistance = 0.0f;
	m_Finished = true;
}

CAvoidFreeze::CSimulateBlatantFreezeJob::CSimulateBlatantFreezeJob(CAvoidFreeze *pAvoidFreeze, CGameClient *pGameClient, int LocalId, const CNetObj_PlayerInput &Input, int Ticks,
	bool DetectFreeze, bool DetectDeath, bool DetectTele, bool DetectUnfreeze, std::mutex *pMutex) :
	m_pAvoidFreeze(pAvoidFreeze), m_pGameClient(pGameClient), m_LocalId(LocalId), m_Input(Input), m_Ticks(Ticks),
	m_DetectFreeze(DetectFreeze), m_DetectDeath(DetectDeath), m_DetectTele(DetectTele), m_DetectUnfreeze(DetectUnfreeze),
	m_Result(-1), m_pMutex(pMutex), m_Finished(false)
{
}

void CAvoidFreeze::CSimulateBlatantFreezeJob::Run()
{
	std::lock_guard<std::mutex> Lock(*m_pMutex);

	if(m_LocalId < 0 || m_Ticks <= 0)
	{
		m_Result = -1;
		m_Finished = true;
		return;
	}

	CGameWorld SimWorld;
	SimWorld.CopyWorldClean(&m_pGameClient->m_PredictedWorld);
	CCharacter *pSimChar = SimWorld.GetCharacterById(m_LocalId);
	if(pSimChar == nullptr)
	{
		m_Result = 0;
		m_Finished = true;
		return;
	}

	for(int Tick = 1; Tick <= m_Ticks; ++Tick)
	{
		pSimChar->OnDirectInput(&m_Input);
		pSimChar->OnDirectInput(&m_Input);
		pSimChar->OnPredictedInput(&m_Input);
		SimWorld.m_GameTick += 1;
		SimWorld.Tick();
		pSimChar = SimWorld.GetCharacterById(m_LocalId);
		if(pSimChar == nullptr)
		{
			m_Result = Tick;
			m_Finished = true;
			return;
		}

		const vec2 Pos = pSimChar->GetPos();
		const int MapIndex = SimWorld.Collision()->GetPureMapIndex(Pos);
		if(MapIndex < 0)
		{
			m_Result = Tick;
			m_Finished = true;
			return;
		}
		if((m_DetectFreeze || m_DetectUnfreeze) && (pSimChar->m_FreezeTime > 0 || pSimChar->Core()->m_DeepFrozen || pSimChar->Core()->m_LiveFrozen))
		{
			m_Result = Tick;
			m_Finished = true;
			return;
		}
		if(m_pAvoidFreeze->IsDangerousMapIndex(MapIndex, m_DetectFreeze || m_DetectUnfreeze, m_DetectDeath, m_DetectTele))
		{
			m_Result = Tick;
			m_Finished = true;
			return;
		}
	}

	m_Result = -1;
	m_Finished = true;
}
