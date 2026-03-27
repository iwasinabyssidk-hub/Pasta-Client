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

namespace
{
constexpr float gs_PastaTileSize = 32.0f;
constexpr int64_t gs_PastaRehookCooldownMs = 2800;
constexpr int64_t gs_PastaAutoAledCooldownMs = 250;
constexpr int64_t gs_PastaSelfHitCooldownMs = 300;
constexpr int gs_PastaAvoidForceMs = 120;
int64_t gs_PastaLastAvoidTime = 0;
float DistancePointToSegment(vec2 Point, vec2 From, vec2 To)
{
	const vec2 Delta = To - From;
	const float DeltaLenSq = length_squared(Delta);
	if(DeltaLenSq <= 0.0001f)
		return distance(Point, From);
	const float T = std::clamp(dot(Point - From, Delta) / DeltaLenSq, 0.0f, 1.0f);
	return distance(Point, From + Delta * T);
}

void TriggerFireTap(CControls *pControls, int Dummy)
{
	pControls->m_aInputData[Dummy].m_Fire = (pControls->m_aInputData[Dummy].m_Fire + 1) & INPUT_STATE_MASK;
	pControls->m_aPastaPendingSelfHitFireRelease[Dummy] = true;
}

vec2 EnsureValidAim(vec2 Pos)
{
	if(length(Pos) < 0.001f)
		return vec2(1.0f, 0.0f);
	return Pos;
}

bool IsFreezeTile(int Tile)
{
	return Tile == TILE_FREEZE || Tile == TILE_DFREEZE || Tile == TILE_LFREEZE;
}

bool HasFreezeTile(const CCollision *pCollision, vec2 Pos)
{
	const int MapIndex = pCollision->GetPureMapIndex(Pos);
	if(MapIndex < 0)
		return false;

	return IsFreezeTile(pCollision->GetTileIndex(MapIndex)) || IsFreezeTile(pCollision->GetFrontTileIndex(MapIndex));
}

bool HasSingleFreezeGapBetween(const CCollision *pCollision, vec2 From, vec2 To)
{
	const int Samples = 16;
	bool WasFreeze = false;
	bool SeenFreeze = false;
	int FreezeRuns = 0;

	for(int Index = 1; Index < Samples; ++Index)
	{
		const float T = Index / (float)Samples;
		const vec2 Pos = mix(From, To, T);
		const bool Freeze = HasFreezeTile(pCollision, Pos);
		if(Freeze && !WasFreeze)
		{
			++FreezeRuns;
			SeenFreeze = true;
		}
		WasFreeze = Freeze;
	}

	return SeenFreeze && FreezeRuns == 1;
}

bool IsDangerousMapIndex(const CCollision *pCollision, int MapIndex, bool DetectFreeze, bool DetectDeath, bool DetectTele)
{
	if(MapIndex < 0)
		return false;

	if(DetectFreeze)
	{
		const int Tile = pCollision->GetTileIndex(MapIndex);
		const int FrontTile = pCollision->GetFrontTileIndex(MapIndex);
		if(IsFreezeTile(Tile) || IsFreezeTile(FrontTile))
			return true;
	}

	if(DetectDeath)
	{
		if(pCollision->GetTileIndex(MapIndex) == TILE_DEATH || pCollision->GetFrontTileIndex(MapIndex) == TILE_DEATH)
			return true;
	}

	if(DetectTele)
	{
		if(pCollision->IsTeleport(MapIndex) || pCollision->IsEvilTeleport(MapIndex) || pCollision->IsCheckTeleport(MapIndex) ||
			pCollision->IsCheckEvilTeleport(MapIndex) || pCollision->IsTeleportWeapon(MapIndex) || pCollision->IsTeleportHook(MapIndex) ||
			pCollision->IsTeleCheckpoint(MapIndex))
			return true;
	}

	return false;
}

bool HasDangerousTile(const CCollision *pCollision, vec2 Pos, bool DetectFreeze, bool DetectDeath, bool DetectTele)
{
	return IsDangerousMapIndex(pCollision, pCollision->GetPureMapIndex(Pos), DetectFreeze, DetectDeath, DetectTele);
}

bool HasFreezeAbove(const CCollision *pCollision, vec2 Pos, int Tiles)
{
	for(int Tile = 1; Tile <= Tiles; ++Tile)
	{
		if(HasFreezeTile(pCollision, Pos + vec2(0.0f, -Tile * gs_PastaTileSize)))
			return true;
	}
	return false;
}

int DetectFreezeSide(const CCollision *pCollision, vec2 Pos)
{
	const float SideOffset = CCharacterCore::PhysicalSizeVec2().x * 0.52f + 1.5f;
	constexpr float aSamplesY[] = {-8.0f, 0.0f, 8.0f};
	bool FreezeRight = false;
	bool FreezeLeft = false;
	for(const float SampleY : aSamplesY)
	{
		FreezeRight |= HasFreezeTile(pCollision, Pos + vec2(SideOffset, SampleY));
		FreezeLeft |= HasFreezeTile(pCollision, Pos + vec2(-SideOffset, SampleY));
	}

	if(FreezeRight == FreezeLeft)
		return 0;
	return FreezeRight ? 1 : -1;
}

bool ShouldAutoJumpSave(const CCollision *pCollision, CCharacter *pCharacter)
{
	if(pCharacter == nullptr || pCharacter->IsGrounded() || pCharacter->Core()->m_Vel.y <= 4.4f)
		return false;
	if(pCharacter->GetJumped() > 1)
		return false;

	const vec2 Pos = pCharacter->GetPos();
	if(HasFreezeAbove(pCollision, Pos, 4))
		return false;

	const float FootOffset = CCharacterCore::PhysicalSizeVec2().y * 0.5f + 2.0f;
	for(int Step = 0; Step < 2; ++Step)
	{
		const float Lookahead = FootOffset + (Step + 1) * 4.0f + pCharacter->Core()->m_Vel.y * (0.18f + 0.06f * Step);
		if(HasFreezeTile(pCollision, Pos + vec2(0.0f, Lookahead)))
			return true;
	}

	return false;
}

bool HasPlayerNearAim(CGameClient *pGameClient, const CCharacter *pLocalCharacter, vec2 AimDir, float MaxDistance, float MaxAngleRadians)
{
	if(pLocalCharacter == nullptr || length(AimDir) < 0.001f)
		return false;

	const vec2 Origin = pLocalCharacter->GetPos();
	const vec2 NormalizedAim = normalize(AimDir);
	const float CosThreshold = std::cos(MaxAngleRadians);
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		if(ClientId == pGameClient->m_Snap.m_LocalClientId || !pGameClient->m_Snap.m_aCharacters[ClientId].m_Active)
			continue;

		const CCharacter *pTarget = pGameClient->m_PredictedWorld.GetCharacterById(ClientId);
		const vec2 TargetPos = pTarget != nullptr ? pTarget->GetPos() : pGameClient->m_aClients[ClientId].m_RenderPos;
		const vec2 ToTarget = TargetPos - Origin;
		const float Dist = length(ToTarget);
		if(Dist < 1.0f || Dist > MaxDistance)
			continue;
		if(dot(normalize(ToTarget), NormalizedAim) >= CosThreshold)
			return true;
	}
	return false;
}

bool HasImmediateSideFreeze(const CCollision *pCollision, const CCharacter *pCharacter, int Direction)
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
			if(HasFreezeTile(pCollision, vec2(SampleX, Pos.y + OffsetY)))
				return true;
		}
	}
	return false;
}

bool IsSolidTileAt(const CCollision *pCollision, int TileX, int TileY)
{
	if(pCollision == nullptr || TileX < 0 || TileY < 0 || TileX >= pCollision->GetWidth() || TileY >= pCollision->GetHeight())
		return false;
	const int Index = TileY * pCollision->GetWidth() + TileX;
	const int Tile = pCollision->GetTileIndex(Index);
	const int FrontTile = pCollision->GetFrontTileIndex(Index);
	return Tile == TILE_SOLID || FrontTile == TILE_SOLID;
}

bool FindBlatantHookTarget(const CCollision *pCollision, vec2 Pos, vec2 Vel, vec2 GoalPos, float HookLength, bool DangerAhead, vec2 &OutTarget)
{
	if(pCollision == nullptr)
		return false;

	const int TileRadius = maximum(2, (int)std::ceil(HookLength / gs_PastaTileSize) + 1);
	const int CenterTileX = std::clamp((int)std::floor(Pos.x / gs_PastaTileSize), 0, pCollision->GetWidth() - 1);
	const int CenterTileY = std::clamp((int)std::floor(Pos.y / gs_PastaTileSize), 0, pCollision->GetHeight() - 1);
	const vec2 ToGoal = length(GoalPos - Pos) > 0.001f ? normalize(GoalPos - Pos) : vec2(1.0f, 0.0f);
	const float GoalSign = GoalPos.x >= Pos.x ? 1.0f : -1.0f;
	const float Speed = length(Vel);

	float BestScore = -std::numeric_limits<float>::infinity();
	vec2 BestTarget(0.0f, 0.0f);

	for(int TileY = CenterTileY - TileRadius; TileY <= CenterTileY + TileRadius; ++TileY)
	{
		for(int TileX = CenterTileX - TileRadius; TileX <= CenterTileX + TileRadius; ++TileX)
		{
			if(!IsSolidTileAt(pCollision, TileX, TileY))
				continue;

			const vec2 TilePos(TileX * gs_PastaTileSize + gs_PastaTileSize * 0.5f, TileY * gs_PastaTileSize + gs_PastaTileSize * 0.5f);
			const vec2 Delta = TilePos - Pos;
			const float Dist = length(Delta);
			if(Dist < 20.0f || Dist > HookLength)
				continue;

			vec2 CollisionPos;
			vec2 BeforeCollision;
			if(pCollision->IntersectLine(Pos, TilePos, &CollisionPos, &BeforeCollision) == TILE_NOHOOK)
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

bool HasImpendingFreeze(const CCollision *pCollision, const CCharacter *pCharacter, int Ticks)
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
		if(HasFreezeTile(pCollision, FuturePos + vec2(0.0f, FootOffset)) ||
			HasFreezeTile(pCollision, FuturePos + vec2(0.0f, 0.0f)))
			return true;
	}
	return false;
}

bool HasBlatantDangerAhead(const CCollision *pCollision, const CCharacter *pCharacter, int Direction, int CheckTicks, bool DetectFreeze, bool DetectDeath, bool DetectTele, bool DetectUnfreeze)
{
	if(pCollision == nullptr || pCharacter == nullptr)
		return false;

	const vec2 Pos = pCharacter->GetPos();
	const vec2 Vel = pCharacter->Core()->m_Vel;
	for(int Tick = 1; Tick <= maximum(1, CheckTicks); ++Tick)
	{
		vec2 FuturePos = Pos + Vel * (0.65f * Tick);
		FuturePos.x += Direction * Tick * 4.0f;
		const int MapIndex = pCollision->GetPureMapIndex(FuturePos);
		if(MapIndex < 0)
			return true;
		if(IsDangerousMapIndex(pCollision, MapIndex, DetectFreeze || DetectUnfreeze, DetectDeath, DetectTele))
			return true;
	}

	return false;
}

int GetBlatantDangerTick(const CCollision *pCollision, const CCharacter *pCharacter, int Direction, int CheckTicks, bool DetectFreeze, bool DetectDeath, bool DetectTele, bool DetectUnfreeze)
{
	if(pCollision == nullptr || pCharacter == nullptr)
		return -1;

	const vec2 Pos = pCharacter->GetPos();
	const vec2 Vel = pCharacter->Core()->m_Vel;
	for(int Tick = 1; Tick <= maximum(1, CheckTicks); ++Tick)
	{
		vec2 FuturePos = Pos + Vel * (0.65f * Tick);
		FuturePos.x += Direction * Tick * 4.0f;
		const int MapIndex = pCollision->GetPureMapIndex(FuturePos);
		if(MapIndex < 0)
			return Tick;
		if(IsDangerousMapIndex(pCollision, MapIndex, DetectFreeze || DetectUnfreeze, DetectDeath, DetectTele))
			return Tick;
	}

	return -1;
}

int SimulateBlatantFreezeTick(CGameClient *pGameClient, int LocalId, const CNetObj_PlayerInput &BaseInput, int Ticks, bool DetectFreeze, bool DetectDeath, bool DetectTele, bool DetectUnfreeze)
{
	if(pGameClient == nullptr || LocalId < 0 || Ticks <= 0)
		return -1;

	CGameWorld SimWorld;
	SimWorld.CopyWorldClean(&pGameClient->m_PredictedWorld);
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
		if(IsDangerousMapIndex(SimWorld.Collision(), MapIndex, DetectFreeze || DetectUnfreeze, DetectDeath, DetectTele))
			return Tick;
	}

	return -1;
}

bool IsBlatantFrozenAtPos(const CCollision *pCollision, vec2 Pos)
{
	if(pCollision == nullptr)
		return false;
	const int Index = pCollision->GetPureMapIndex(Pos);
	if(Index < 0)
		return false;
	const int aTiles[] = {
		pCollision->GetTileIndex(Index),
		pCollision->GetFrontTileIndex(Index),
		pCollision->GetSwitchType(Index)};
	for(const int Tile : aTiles)
	{
		if(Tile == TILE_FREEZE || Tile == TILE_DFREEZE || Tile == TILE_LFREEZE)
			return true;
	}
	return false;
}

} // namespace

bool CControls::PastaAutomationAllowed() const
{
	if(!GameClient() || !Collision() || !GameClient()->Predict())
		return false;
	if(GameClient()->m_Snap.m_SpecInfo.m_Active || GameClient()->m_Chat.IsActive() || GameClient()->m_Menus.IsActive())
		return false;
	return true;
}

int CControls::PastaAvoidPredictTicks() const
{
	return std::clamp(g_Config.m_PastaAvoidLegitCheckTicks, 1, 100);
}

int CControls::PastaHookAssistTicks() const
{
	return std::clamp(g_Config.m_PastaAvoidLegitUnfreeze ? g_Config.m_PastaAvoidLegitUnfreezeTicks : g_Config.m_PastaAvoidLegitCheckTicks, 1, 100);
}

int CControls::PastaDirectionSensitivityStep() const
{
	return g_Config.m_PastaAvoidLegitDirectionPriority >= 667 ? 2 : 1;
}

float CControls::PastaDirectionSensitivityFactor() const
{
	return std::clamp(g_Config.m_PastaAvoidLegitDirectionPriority / 1000.0f, 0.10f, 1.0f);
}

int CControls::PastaMaxAvoidAttempts() const
{
	return std::clamp(g_Config.m_PastaAvoidLegitQuality, 1, 100);
}

int CControls::PastaMaxAvoidAttemptsPerDirection() const
{
	return std::clamp(maximum(1, g_Config.m_PastaAvoidLegitRandomness), 1, 100);
}

bool CControls::PastaGetFreeze(vec2 Pos, int FreezeTime) const
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

bool CControls::PastaIsAvoidCooldownElapsed(int64_t CurrentTime) const
{
	const int64_t ConfiguredDelay = static_cast<int64_t>(maximum(0, g_Config.m_PastaAvoidLegitAfkProtection ? g_Config.m_PastaAvoidLegitAfkTime * 100 : 0)) * time_freq() / 1000;
	if(gs_PastaLastAvoidTime == 0)
		return true;
	return CurrentTime - gs_PastaLastAvoidTime >= ConfiguredDelay;
}

void CControls::PastaUpdateAvoidCooldown(int64_t CurrentTime)
{
	gs_PastaLastAvoidTime = CurrentTime + static_cast<int64_t>(gs_PastaAvoidForceMs) * time_freq() / 1000;
}

bool CControls::PastaPredictFreeze(const CNetObj_PlayerInput &Input, int Ticks, int *pDangerTick, float *pDangerDistance) const
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

bool CControls::PastaTryMove(const CNetObj_PlayerInput &BaseInput, int Direction, int CheckTicks)
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
		m_aPastaAvoidForcing[Local] = true;
		m_aPastaAvoidForcedDir[Local] = ModifiedInput.m_Direction;
		m_aPastaAvoidForceUntil[Local] = time_get() + static_cast<int64_t>(gs_PastaAvoidForceMs) * time_freq() / 1000;
		return true;
	}
	return false;
}

bool CControls::PastaTryAvoidFreeze(int LocalPlayerId)
{
	const int CheckTicks = PastaAvoidPredictTicks();
	const int MaxAttempts = PastaMaxAvoidAttempts();
	const int MaxAttemptsPerDirection = PastaMaxAvoidAttemptsPerDirection();
	const CNetObj_PlayerInput BaseInput = m_aInputData[LocalPlayerId];
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

				m_aPastaAvoidForcing[LocalPlayerId] = false;
			}
		}

		if(!TriedAny)
			break;
	}

	return false;
}

bool CControls::PastaIsMouseMoved(int LocalPlayerId) const
{
	const bool HasMoved = m_aMousePos[LocalPlayerId] != m_aPastaAvoidLastMousePos[LocalPlayerId];
	m_aPastaAvoidLastMousePos[LocalPlayerId] = m_aMousePos[LocalPlayerId];
	return HasMoved;
}

bool CControls::PastaIsPlayerActive(int LocalPlayerId) const
{
	const CNetObj_PlayerInput &Input = m_aInputData[LocalPlayerId];
	return Input.m_Direction != 0 || Input.m_Jump != 0 || Input.m_Hook != 0 || PastaIsMouseMoved(LocalPlayerId);
}

void CControls::PastaAvoidFreeze()
{
	if(!g_Config.m_PastaAvoidfreeze || g_Config.m_PastaAvoidMode != 1 || !g_Config.m_PastaAvoidLegitDirection)
		return;
	if(!PastaAutomationAllowed())
		return;

	const int64_t CurrentTime = time_get();
	if(!PastaIsAvoidCooldownElapsed(CurrentTime))
		return;

	const int LocalPlayerId = g_Config.m_ClDummy;
	if(m_aPastaAvoidForcing[LocalPlayerId] && CurrentTime <= m_aPastaAvoidForceUntil[LocalPlayerId])
		return;
	if(!PastaIsPlayerActive(LocalPlayerId))
		return;

	int DangerTick = -1;
	float DangerDistance = 0.0f;
	const bool InDanger = PastaPredictFreeze(m_aInputData[LocalPlayerId], PastaAvoidPredictTicks(), &DangerTick, &DangerDistance);
	if(!InDanger)
	{
		m_aPastaAvoidWasInDanger[LocalPlayerId] = false;
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
		m_aPastaAvoidWasInDanger[LocalPlayerId] = false;
		return;
	}

	if(m_aPastaAvoidWasInDanger[LocalPlayerId])
		return;

	if(PastaTryAvoidFreeze(LocalPlayerId))
	{
		m_aPastaAvoidWasInDanger[LocalPlayerId] = true;
		PastaUpdateAvoidCooldown(CurrentTime);
	}
}

void CControls::PastaHookAssist()
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

	const bool AimingUp = m_aInputData[Local].m_TargetY < 0;
	const int BaseCheckTicks = PastaHookAssistTicks();
	const int CheckTicks = maximum(1, BaseCheckTicks + ((BaseCheckTicks >= 1 && BaseCheckTicks <= 3 && AimingUp) ? 1 : 0));
	const float SensitivityFactor = std::clamp(BaseCheckTicks / 10.0f, 0.2f, 1.0f);
	const bool CurrentlyHoldingHook = m_aInputData[Local].m_Hook != 0;
	const float Speed = length(pCore->m_Vel);

	if(CurrentlyHoldingHook)
	{
		CNetObj_PlayerInput TestInput = m_aInputData[Local];
		TestInput.m_Hook = 1;

		int DangerTick = -1;
		float DangerDistance = 0.0f;
		if(PastaPredictFreeze(TestInput, CheckTicks, &DangerTick, &DangerDistance))
		{
			CNetObj_PlayerInput ReleaseInput = m_aInputData[Local];
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
				m_aInputData[Local].m_Hook = 0;
		}
	}
	else
	{
		CNetObj_PlayerInput TestInput = m_aInputData[Local];
		TestInput.m_Hook = 1;

		int DangerTick = -1;
		float DangerDistance = 0.0f;
		if(PastaPredictFreeze(TestInput, CheckTicks, &DangerTick, &DangerDistance))
		{
			CNetObj_PlayerInput NoHookInput = m_aInputData[Local];
			NoHookInput.m_Hook = 0;
			const bool CanMoveWithoutHook = !PastaPredictFreeze(NoHookInput, CheckTicks);
			const float UrgencyMultiplier = 0.2f + (0.6f * SensitivityFactor);
			const int HookUrgencyTicks = maximum(1, (int)(CheckTicks * UrgencyMultiplier));
			if(DangerTick <= HookUrgencyTicks && DangerDistance < 48.0f && CanMoveWithoutHook)
				m_aInputData[Local].m_Hook = 0;
		}
	}
}

namespace
{

bool FindAutoAledTarget(CGameClient *pGameClient, CCharacter *pLocalCharacter, vec2 RealAimPos)
{
	if(pLocalCharacter == nullptr || pLocalCharacter->GetActiveWeapon() != WEAPON_HAMMER)
		return false;

	const vec2 LocalPos = pLocalCharacter->GetPos();
	const vec2 AimDir = normalize(EnsureValidAim(RealAimPos));
	const float Proximity = pLocalCharacter->GetProximityRadius();
	const vec2 ProjStartPos = LocalPos + AimDir * Proximity * 0.75f;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		if(ClientId == pGameClient->m_Snap.m_LocalClientId || !pGameClient->m_Snap.m_aCharacters[ClientId].m_Active)
			continue;

		CCharacter *pTarget = pGameClient->m_PredictedWorld.GetCharacterById(ClientId);
		if(pTarget == nullptr)
			continue;
		if(!(pTarget->m_FreezeTime > 0 || pTarget->Core()->m_DeepFrozen || pTarget->Core()->m_LiveFrozen))
			continue;
		const vec2 ToTarget = pTarget->GetPos() - LocalPos;
		const float Dist = length(ToTarget);
		if(Dist < 12.0f || Dist > 70.0f || absolute(LocalPos.y - pTarget->GetPos().y) > 28.0f)
			continue;
		if(dot(normalize(ToTarget), AimDir) < 0.55f)
			continue;
		if(distance(ProjStartPos, pTarget->GetPos()) > Proximity)
			continue;

		const vec2 StartCheck = LocalPos + normalize(ToTarget) * 8.0f;
		const vec2 EndCheck = pTarget->GetPos() - normalize(ToTarget) * 8.0f;
		const bool SingleFreezeGap = HasSingleFreezeGapBetween(pGameClient->Collision(), StartCheck, EndCheck);
		if(SingleFreezeGap)
			return true;
	}

	return false;
}

bool EvaluateSelfHitLine(CCollision *pCollision, vec2 StartPos, vec2 Direction, float Reach, const CTuningParams *pTuning, vec2 TargetPos, int DesiredBounces, bool PreferMostBounces, float VelocityPreference, float *pOutScore, int *pOutBounces, int *pOutHitSegment)
{
	if(length(Direction) < 0.001f || Reach <= 0.0f || pTuning == nullptr)
		return false;

	vec2 Pos = StartPos;
	vec2 Dir = normalize(Direction);
	float Energy = Reach;
	int Bounces = 0;
	bool ZeroEnergyBounceInLastTick = false;
	float BestDistance = std::numeric_limits<float>::max();
	float Traveled = 0.0f;
	vec2 LastSegmentDir = normalize(Direction);
	int BestHitSegment = -1;

	while(Energy > 0.0f)
	{
		vec2 Coltile;
		vec2 To = Pos + Dir * Energy;
		vec2 BeforeCollision = To;
		const int Res = pCollision->IntersectLineTeleWeapon(Pos, To, &Coltile, &BeforeCollision);
		const float SegmentDistance = distance(Pos, BeforeCollision);
		if(SegmentDistance > 0.001f)
			LastSegmentDir = normalize(BeforeCollision - Pos);
		if(Traveled > 24.0f)
		{
			const float SegmentDistanceToTarget = DistancePointToSegment(TargetPos, Pos, BeforeCollision);
			if(SegmentDistanceToTarget < BestDistance)
			{
				BestDistance = SegmentDistanceToTarget;
				BestHitSegment = Bounces;
			}
		}
		Traveled += SegmentDistance;
		if(Res == 0)
			break;

		vec2 TempPos = BeforeCollision;
		vec2 TempDir = Dir * 4.0f;
		int OriginalTile = 0;
		if(Res == -1)
		{
			OriginalTile = pCollision->GetTile(round_to_int(Coltile.x), round_to_int(Coltile.y));
			pCollision->SetCollisionAt(round_to_int(Coltile.x), round_to_int(Coltile.y), TILE_SOLID);
		}
		pCollision->MovePoint(&TempPos, &TempDir, 1.0f, nullptr);
		if(Res == -1)
			pCollision->SetCollisionAt(round_to_int(Coltile.x), round_to_int(Coltile.y), OriginalTile);

		const float DistanceTraveled = distance(Pos, TempPos);
		if(DistanceTraveled == 0.0f && ZeroEnergyBounceInLastTick)
			break;

		Energy -= DistanceTraveled + pTuning->m_LaserBounceCost;
		ZeroEnergyBounceInLastTick = DistanceTraveled == 0.0f;
		Pos = TempPos;
		if(length(TempDir) < 0.001f)
			break;
		Dir = normalize(TempDir);

		++Bounces;
		if(Bounces > pTuning->m_LaserBounceNum)
			break;
	}

	if(pOutBounces != nullptr)
		*pOutBounces = Bounces;
	if(pOutHitSegment != nullptr)
		*pOutHitSegment = BestHitSegment;

	if(BestDistance > 28.0f || Traveled < 40.0f || BestHitSegment < 1)
		return false;

	const float BounceBias = PreferMostBounces ? -Bounces * 3.5f : Bounces * 3.5f;
	const float BouncePenalty = absolute(Bounces - DesiredBounces) * 0.5f;
	if(pOutScore != nullptr)
	{
		const vec2 VelocityPrefDir = EnsureValidAim(TargetPos - StartPos);
		const float VelocityAlignment = dot(normalize(VelocityPrefDir), -LastSegmentDir);
		if(VelocityPreference > 0.0f && VelocityAlignment < 0.15f)
			return false;
		*pOutScore = BestDistance + BouncePenalty + BounceBias - absolute(VelocityPreference) * VelocityAlignment * 18.0f;
	}
	return true;
}

vec2 BuildFutureSelfHitTarget(const CCharacter *pLocalCharacter, int FutureTick, bool LiftForUnfreeze)
{
	const vec2 StartPos = pLocalCharacter->GetPos();
	const vec2 Vel = pLocalCharacter->Core()->m_Vel;
	vec2 TargetPos = StartPos + Vel * (float)FutureTick;
	if(LiftForUnfreeze)
		TargetPos.y -= CCharacterCore::PhysicalSizeVec2().y * 0.35f;
	return TargetPos;
}

bool FindBestSelfHitAim(CGameClient *pGameClient, CCharacter *pLocalCharacter, vec2 RealAimPos, int Weapon, int ExtraLookTicks, vec2 *pOutAimPos, float *pOutTimingError = nullptr)
{
	if(pLocalCharacter == nullptr || pOutAimPos == nullptr)
		return false;
	if(Weapon != WEAPON_LASER && Weapon != WEAPON_SHOTGUN)
		return false;
	if(!pLocalCharacter->GetWeaponGot(Weapon))
		return false;
	if((Weapon == WEAPON_LASER && pLocalCharacter->LaserHitDisabled()) || (Weapon == WEAPON_SHOTGUN && pLocalCharacter->ShotgunHitDisabled()))
		return false;

	const CTuningParams *pTuning = pGameClient->GetTuning(pLocalCharacter->GetOverriddenTuneZone());
	if(pTuning == nullptr)
		return false;

	const vec2 RealAim = normalize(EnsureValidAim(RealAimPos));
	const vec2 StartPos = pLocalCharacter->GetPos();
	const int LookTicks = maximum(2, ExtraLookTicks);
	const float Fov = (Weapon == WEAPON_LASER ? g_Config.m_PastaUnfreezeBotFov : g_Config.m_PastaAutoShotgunFov) * pi / 180.0f;
	const int Samples = maximum(10, Weapon == WEAPON_LASER ? g_Config.m_PastaUnfreezeBotPoints : g_Config.m_PastaAutoShotgunPoints);
	const bool PreferMostBounces = Weapon == WEAPON_LASER ? g_Config.m_PastaUnfreezeBotMostBounces != 0 : g_Config.m_PastaAutoShotgunMostBounces != 0;
	const float VelocityPreference = Weapon == WEAPON_SHOTGUN ? (g_Config.m_PastaAutoShotgunHighestVel ? 1.0f : -1.0f) : 0.0f;
	const float BounceDelayTicks = pTuning->m_LaserBounceDelay * pGameClient->Client()->GameTickSpeed() / 1000.0f;
	int DesiredBounces = 2;
	int TargetTickStart = maximum(1, LookTicks - 2);
	int TargetTickEnd = LookTicks + 4;
	if(Weapon == WEAPON_LASER)
	{
		if(BounceDelayTicks > 0.01f && pLocalCharacter->m_FreezeTime > 0)
			DesiredBounces = maximum(1, (int)std::ceil((pLocalCharacter->m_FreezeTime + 2) / BounceDelayTicks));
		TargetTickStart = maximum(TargetTickStart, pLocalCharacter->m_FreezeTime + 1);
		TargetTickEnd = maximum(TargetTickStart, TargetTickStart + 5);
	}
	else if(Weapon == WEAPON_SHOTGUN)
	{
		DesiredBounces = maximum(1, g_Config.m_PastaAutoShotgunMostBounces ? 3 : 1);
		TargetTickStart = 1;
		TargetTickEnd = maximum(3, LookTicks);
	}

	float BestScore = std::numeric_limits<float>::max();
	vec2 BestAim = RealAim * maximum(96.0f, length(RealAimPos));
	float BestTimingError = std::numeric_limits<float>::max();
	bool Found = false;

	for(int Sample = 0; Sample < Samples; ++Sample)
	{
		const float T = Samples == 1 ? 0.5f : Sample / (float)(Samples - 1);
		const float AngleOffset = (T - 0.5f) * Fov;
		const vec2 Dir = direction(angle(RealAim) + AngleOffset);
		for(int TargetTick = TargetTickStart; TargetTick <= TargetTickEnd; ++TargetTick)
		{
			const vec2 TargetPos = BuildFutureSelfHitTarget(pLocalCharacter, TargetTick, Weapon == WEAPON_LASER);
			float Score = 0.0f;
			int Bounces = 0;
			int HitSegment = -1;
			if(!EvaluateSelfHitLine(pGameClient->Collision(), StartPos, Dir, pTuning->m_LaserReach, pTuning, TargetPos, DesiredBounces, PreferMostBounces, VelocityPreference, &Score, &Bounces, &HitSegment))
				continue;

			const float ArrivalTick = HitSegment * maximum(0.01f, BounceDelayTicks);
			const float TimingError = absolute(ArrivalTick - TargetTick);
			const float TimingWeight = Weapon == WEAPON_LASER ? 4.0f : 1.25f;
			Score += TimingError * TimingWeight;
			Score += TargetTick * (Weapon == WEAPON_LASER ? 0.18f : 0.05f);
			if(Score < BestScore)
			{
				BestScore = Score;
				BestAim = Dir * maximum(96.0f, length(RealAimPos));
				BestTimingError = TimingError;
				Found = true;
			}
		}
	}

	if(Found)
	{
		*pOutAimPos = BestAim;
		if(pOutTimingError != nullptr)
			*pOutTimingError = BestTimingError;
	}
	return Found;
}

bool IsWeaponReadyForSelfHit(CGameClient *pGameClient, CCharacter *pLocalCharacter, int Weapon)
{
	if(pGameClient == nullptr || pLocalCharacter == nullptr)
		return false;

	const CTuningParams *pTuning = pGameClient->GetTuning(pLocalCharacter->GetOverriddenTuneZone());
	if(pTuning == nullptr)
		return false;

	const int FireDelayTicks = maximum(1, round_to_int(pTuning->GetWeaponFireDelay(Weapon) * pGameClient->Client()->GameTickSpeed()));
	return pGameClient->Client()->PredGameTick(g_Config.m_ClDummy) - pLocalCharacter->GetAttackTick() >= FireDelayTicks;
}

bool HasSupportTile(const CCollision *pCollision, int TileX, float FootY)
{
	if(TileX < 0 || TileX >= pCollision->GetWidth())
		return false;
	return pCollision->CheckPoint((TileX + 0.5f) * gs_PastaTileSize, FootY);
}

bool HasHazardOrVoidBeyondPlatform(const CCollision *pCollision, int TileX, int Direction, float FootY)
{
	const int BeyondTileX = TileX + Direction;
	if(BeyondTileX < 0 || BeyondTileX >= pCollision->GetWidth())
		return true;
	if(HasSupportTile(pCollision, BeyondTileX, FootY))
		return false;

	const vec2 CheckPos((BeyondTileX + 0.5f) * gs_PastaTileSize, FootY + 6.0f);
	return HasFreezeTile(pCollision, CheckPos) || !pCollision->CheckPoint(CheckPos.x, CheckPos.y);
}

void PulseFire(CNetObj_PlayerInput &Input)
{
	Input.m_Fire = (Input.m_Fire + 1) & INPUT_STATE_MASK;
}

int FindClosestTargetClientId(CGameClient *pGameClient, const CCharacter *pLocalCharacter)
{
	if(pLocalCharacter == nullptr)
		return -1;

	int ClosestClientId = -1;
	float ClosestDistance = 1e9f;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		if(ClientId == pGameClient->m_Snap.m_LocalClientId || !pGameClient->m_Snap.m_aCharacters[ClientId].m_Active)
			continue;

		const CCharacter *pTarget = pGameClient->m_PredictedWorld.GetCharacterById(ClientId);
		const vec2 TargetPos = pTarget != nullptr ? pTarget->GetPos() : pGameClient->m_aClients[ClientId].m_RenderPos;
		const float Distance = distance(pLocalCharacter->GetPos(), TargetPos);
		if(Distance < ClosestDistance)
		{
			ClosestDistance = Distance;
			ClosestClientId = ClientId;
		}
	}

	return ClosestClientId;
}

vec2 GetAimToClient(CGameClient *pGameClient, const CCharacter *pLocalCharacter, int ClientId, vec2 Fallback)
{
	if(pLocalCharacter == nullptr || ClientId < 0 || ClientId >= MAX_CLIENTS || !pGameClient->m_Snap.m_aCharacters[ClientId].m_Active)
		return EnsureValidAim(Fallback);

	const CCharacter *pTarget = pGameClient->m_PredictedWorld.GetCharacterById(ClientId);
	const vec2 TargetPos = pTarget != nullptr ? pTarget->GetPos() : pGameClient->m_aClients[ClientId].m_RenderPos;
	return EnsureValidAim(TargetPos - pLocalCharacter->GetPos());
}

vec2 BuildFakeAimPos(CGameClient *pGameClient, int Dummy, vec2 RealPos, CCharacter *pLocalCharacter, int64_t Now)
{
	const float Radius = maximum(96.0f, length(RealPos));
	const float AngleSpeed = 0.9f + g_Config.m_PastaFakeAimSpeed * 0.22f;

	switch(g_Config.m_PastaFakeAimMode)
	{
	case 0:
		return EnsureValidAim(RealPos);
	case 1:
		return EnsureValidAim(RealPos);
	case 2:
	{
		const float Angle = pGameClient->Client()->LocalTime() * AngleSpeed * 2.0f * pi;
		return vec2(std::cos(Angle), std::sin(Angle)) * Radius;
	}
	case 3:
	{
		const int64_t Interval = maximum<int64_t>(1, time_freq() / maximum(2, g_Config.m_PastaFakeAimSpeed * 5));
		if(Now > pGameClient->m_Controls.m_aPastaFakeAimLastRandomUpdate[Dummy] + Interval)
		{
			pGameClient->m_Controls.m_aPastaFakeAimLastRandomUpdate[Dummy] = Now;
			pGameClient->m_Controls.m_aPastaFakeAimAngle[Dummy] = (rand() / (float)RAND_MAX) * 2.0f * pi;
		}
		const float Angle = pGameClient->m_Controls.m_aPastaFakeAimAngle[Dummy];
		return vec2(std::cos(Angle), std::sin(Angle)) * Radius;
	}
	case 4:
		return EnsureValidAim(-RealPos);
	case 5:
		return GetAimToClient(pGameClient, pLocalCharacter, FindClosestTargetClientId(pGameClient, pLocalCharacter), -RealPos);
	default:
		return EnsureValidAim(RealPos);
	}
}

enum
{
	PASTA_AIMBOT_SLOT_HOOK = -1,
	PASTA_AIMBOT_SLOT_HAMMER = WEAPON_HAMMER,
	PASTA_AIMBOT_SLOT_GUN = WEAPON_GUN,
	PASTA_AIMBOT_SLOT_SHOTGUN = WEAPON_SHOTGUN,
	PASTA_AIMBOT_SLOT_GRENADE = WEAPON_GRENADE,
	PASTA_AIMBOT_SLOT_LASER = WEAPON_LASER,
};

struct SPastaAimbotTarget
{
	int m_ClientId = -1;
	vec2 m_AimPos = vec2(0.0f, 0.0f);
	vec2 m_WorldPos = vec2(0.0f, 0.0f);
	float m_Score = 1e9f;
	bool m_Visible = false;
};

float GetPastaAimbotFov(int Slot)
{
	switch(Slot)
	{
	case PASTA_AIMBOT_SLOT_HOOK: return (float)g_Config.m_PastaAimbotHookFov;
	case PASTA_AIMBOT_SLOT_HAMMER: return (float)g_Config.m_PastaAimbotHammerFov;
	case PASTA_AIMBOT_SLOT_GUN: return (float)g_Config.m_PastaAimbotPistolFov;
	case PASTA_AIMBOT_SLOT_SHOTGUN: return (float)g_Config.m_PastaAimbotShotgunFov;
	case PASTA_AIMBOT_SLOT_GRENADE: return (float)g_Config.m_PastaAimbotGrenadeFov;
	case PASTA_AIMBOT_SLOT_LASER: return (float)g_Config.m_PastaAimbotLaserFov;
	default: return 90.0f;
	}
}

int GetPastaAimbotPriority(int Slot)
{
	switch(Slot)
	{
	case PASTA_AIMBOT_SLOT_HOOK: return g_Config.m_PastaAimbotHookTargetPriority;
	case PASTA_AIMBOT_SLOT_HAMMER: return g_Config.m_PastaAimbotHammerTargetPriority;
	case PASTA_AIMBOT_SLOT_GUN: return g_Config.m_PastaAimbotPistolTargetPriority;
	case PASTA_AIMBOT_SLOT_SHOTGUN: return g_Config.m_PastaAimbotShotgunTargetPriority;
	case PASTA_AIMBOT_SLOT_GRENADE: return g_Config.m_PastaAimbotGrenadeTargetPriority;
	case PASTA_AIMBOT_SLOT_LASER: return g_Config.m_PastaAimbotLaserTargetPriority;
	default: return g_Config.m_PastaAimbotTargetPriority;
	}
}

bool GetPastaAimbotSilent(int Slot)
{
	switch(Slot)
	{
	case PASTA_AIMBOT_SLOT_HOOK: return g_Config.m_PastaAimbotHookSilent != 0;
	case PASTA_AIMBOT_SLOT_HAMMER: return g_Config.m_PastaAimbotHammerSilent != 0;
	case PASTA_AIMBOT_SLOT_GUN: return g_Config.m_PastaAimbotPistolSilent != 0;
	case PASTA_AIMBOT_SLOT_SHOTGUN: return g_Config.m_PastaAimbotShotgunSilent != 0;
	case PASTA_AIMBOT_SLOT_GRENADE: return g_Config.m_PastaAimbotGrenadeSilent != 0;
	case PASTA_AIMBOT_SLOT_LASER: return g_Config.m_PastaAimbotLaserSilent != 0;
	default: return false;
	}
}

bool GetPastaAimbotIgnoreFriends(int Slot)
{
	switch(Slot)
	{
	case PASTA_AIMBOT_SLOT_HOOK: return g_Config.m_PastaAimbotHookIgnoreFriends != 0;
	case PASTA_AIMBOT_SLOT_HAMMER: return g_Config.m_PastaAimbotHammerIgnoreFriends != 0;
	case PASTA_AIMBOT_SLOT_GUN: return g_Config.m_PastaAimbotPistolIgnoreFriends != 0;
	case PASTA_AIMBOT_SLOT_SHOTGUN: return g_Config.m_PastaAimbotShotgunIgnoreFriends != 0;
	case PASTA_AIMBOT_SLOT_GRENADE: return g_Config.m_PastaAimbotGrenadeIgnoreFriends != 0;
	case PASTA_AIMBOT_SLOT_LASER: return g_Config.m_PastaAimbotLaserIgnoreFriends != 0;
	default: return false;
	}
}

bool GetPastaAimbotEnabled(int Slot)
{
	switch(Slot)
	{
	case PASTA_AIMBOT_SLOT_HOOK: return g_Config.m_PastaAimbotHook != 0;
	case PASTA_AIMBOT_SLOT_HAMMER: return g_Config.m_PastaAimbotHammer != 0;
	case PASTA_AIMBOT_SLOT_GUN: return g_Config.m_PastaAimbotPistol != 0;
	case PASTA_AIMBOT_SLOT_SHOTGUN: return g_Config.m_PastaAimbotShotgun != 0;
	case PASTA_AIMBOT_SLOT_GRENADE: return g_Config.m_PastaAimbotGrenade != 0;
	case PASTA_AIMBOT_SLOT_LASER: return g_Config.m_PastaAimbotLaser != 0;
	default: return false;
	}
}

vec2 NormalizePastaAim(vec2 Pos)
{
	constexpr float CameraMaxDistance = 200.0f;
	const float FollowFactor = (g_Config.m_ClDyncam ? g_Config.m_ClDyncamFollowFactor : g_Config.m_ClMouseFollowfactor) / 100.0f;
	const float DeadZone = g_Config.m_ClDyncam ? g_Config.m_ClDyncamDeadzone : g_Config.m_ClMouseDeadzone;
	const float MaxDistance = g_Config.m_ClMouseMaxDistance;
	const float MouseMax = minimum((FollowFactor != 0.0f ? CameraMaxDistance / FollowFactor + DeadZone : MaxDistance), MaxDistance);
	const float AimDistance = length(Pos);
	return normalize_pre_length(EnsureValidAim(Pos), AimDistance) * MouseMax;
}

float PastaAngleDiff(vec2 From, vec2 To)
{
	return absolute(atan2(std::sin(angle(EnsureValidAim(From)) - angle(EnsureValidAim(To))),
		std::cos(angle(EnsureValidAim(From)) - angle(EnsureValidAim(To))))) * 180.0f / pi;
}

bool IsPastaTargetAllowed(CGameClient *pGameClient, const CCharacter *pLocalCharacter, int ClientId, int Slot)
{
	if(pLocalCharacter == nullptr || ClientId < 0 || ClientId >= MAX_CLIENTS || ClientId == pGameClient->m_Snap.m_LocalClientId)
		return false;
	if(!pGameClient->m_Snap.m_aCharacters[ClientId].m_Active)
		return false;
	if(GetPastaAimbotIgnoreFriends(Slot))
	{
		const bool FriendByFlag = pGameClient->m_aClients[ClientId].m_Friend;
		const bool FriendByNameClan = pGameClient->Friends()->IsFriend(pGameClient->m_aClients[ClientId].m_aName, pGameClient->m_aClients[ClientId].m_aClan, true);
		const bool FriendByName = pGameClient->Friends()->IsFriend(pGameClient->m_aClients[ClientId].m_aName, "", false);
		if(FriendByFlag || FriendByNameClan || FriendByName)
			return false;
	}
	return true;
}

bool HasPastaDirectLineOfSight(CGameClient *pGameClient, vec2 From, vec2 To)
{
	vec2 Col;
	vec2 NewPos;
	return !pGameClient->Collision()->IntersectLine(From, To, &Col, &NewPos);
}

vec2 PredictPastaWeaponTarget(CGameClient *pGameClient, const CCharacter *pLocalCharacter, vec2 TargetPos, vec2 TargetVel, int Slot)
{
	if(pLocalCharacter == nullptr)
		return TargetPos;

	const vec2 StartPos = pLocalCharacter->GetPos();
	const float Distance = distance(StartPos, TargetPos);
	switch(Slot)
	{
	case PASTA_AIMBOT_SLOT_GUN:
		TargetPos += TargetVel * (Distance / maximum(1.0f, (float)pGameClient->m_aTuning[g_Config.m_ClDummy].m_GunSpeed));
		break;
	case PASTA_AIMBOT_SLOT_SHOTGUN:
		TargetPos += TargetVel * (Distance / maximum(1.0f, (float)pGameClient->m_aTuning[g_Config.m_ClDummy].m_ShotgunSpeed));
		break;
	case PASTA_AIMBOT_SLOT_GRENADE:
		TargetPos += TargetVel * (Distance / maximum(1.0f, (float)pGameClient->m_aTuning[g_Config.m_ClDummy].m_GrenadeSpeed));
		break;
	case PASTA_AIMBOT_SLOT_LASER:
		TargetPos += TargetVel * 0.18f;
		break;
	default:
		break;
	}
	return TargetPos;
}

bool PastaPredictHook(CGameClient *pGameClient, vec2 MyPos, vec2 MyVel, vec2 &TargetPos, vec2 TargetVel)
{
	const vec2 Delta = TargetPos - MyPos;
	const vec2 DeltaVel = TargetVel - MyVel;
	const float HookSpeed = length(TargetVel) + pGameClient->m_aTuning[g_Config.m_ClDummy].m_HookFireSpeed;
	const float A = dot(DeltaVel, DeltaVel) - HookSpeed * HookSpeed;
	const float B = 2.0f * dot(DeltaVel, Delta);
	const float C = dot(Delta, Delta);
	const float Solution = B * B - 4.0f * A * C;
	if(Solution <= 0.0f)
		return false;

	const float Time = absolute(2.0f * C / (std::sqrt(Solution) - B)) + (float)pGameClient->Client()->GetPredictionTime() / 100.0f;
	TargetPos += TargetVel * Time;
	return true;
}

bool PastaIntersectCharacter(vec2 HookPos, vec2 TargetPos, vec2 &NewPos)
{
	vec2 ClosestPoint;
	if(closest_point_on_line(HookPos, NewPos, TargetPos, ClosestPoint) && distance(TargetPos, ClosestPoint) < CCharacterCore::PhysicalSize() + 2.0f)
	{
		NewPos = ClosestPoint;
		return true;
	}
	return false;
}

bool PastaHookLineHits(CGameClient *pGameClient, vec2 InitPos, vec2 TargetPos, vec2 ScanDir)
{
	vec2 ExDirection = normalize(ScanDir);
	vec2 FinishPos = InitPos + ExDirection * ((float)pGameClient->m_aTuning[g_Config.m_ClDummy].m_HookLength - CCharacterCore::PhysicalSize() * 1.5f);
	vec2 OldPos = InitPos + ExDirection * CCharacterCore::PhysicalSize() * 1.5f;
	vec2 NewPos = OldPos;
	bool DoBreak = false;

	do
	{
		OldPos = NewPos;
		NewPos = OldPos + ExDirection * pGameClient->m_aTuning[g_Config.m_ClDummy].m_HookFireSpeed;
		if(distance(InitPos, NewPos) > pGameClient->m_aTuning[g_Config.m_ClDummy].m_HookLength)
		{
			NewPos = InitPos + normalize(NewPos - InitPos) * pGameClient->m_aTuning[g_Config.m_ClDummy].m_HookLength;
			DoBreak = true;
		}

		int TeleNr = 0;
		const int Hit = pGameClient->Collision()->IntersectLineTeleHook(OldPos, NewPos, &FinishPos, nullptr, &TeleNr);
		if(PastaIntersectCharacter(OldPos, TargetPos, FinishPos))
			return true;
		if(Hit)
			break;

		NewPos.x = round_to_int(NewPos.x);
		NewPos.y = round_to_int(NewPos.y);
		if(OldPos == NewPos)
			break;

		ExDirection.x = round_to_int(ExDirection.x * 256.0f) / 256.0f;
		ExDirection.y = round_to_int(ExDirection.y * 256.0f) / 256.0f;
	} while(!DoBreak);
	return false;
}

vec2 PastaHookEdgeScan(CGameClient *pGameClient, const CCharacter *pLocalCharacter, vec2 TargetPos, vec2 TargetVel)
{
	if(pLocalCharacter == nullptr)
		return vec2(0.0f, 0.0f);

	vec2 MyPos = pLocalCharacter->GetPos();
	vec2 MyVel = pLocalCharacter->Core()->m_Vel;
	if(!PastaPredictHook(pGameClient, MyPos, MyVel, TargetPos, TargetVel))
		return vec2(0.0f, 0.0f);
	if(PastaHookLineHits(pGameClient, MyPos, TargetPos, TargetPos - MyPos))
		return TargetPos - MyPos;

	const float VisibleAngle = atan2(TargetPos.y - MyPos.y, TargetPos.x - MyPos.x) + pi * 0.5f;
	const float Accuracy = maximum(1.0f, (float)g_Config.m_PastaAimbotHookAccuracy);
	vec2 aHitPoints[128];
	int HitPointsCount = 0;
	for(float Angle = VisibleAngle; Angle < pi + VisibleAngle; Angle += 1.0f / Accuracy)
	{
		if(HitPointsCount >= 128)
			break;
		const vec2 Pos = vec2(round_to_int(TargetPos.x + std::cos(Angle) * CCharacterCore::PhysicalSize()), round_to_int(TargetPos.y + std::sin(Angle) * CCharacterCore::PhysicalSize()));
		const vec2 Dir = Pos - MyPos;
		if(PastaHookLineHits(pGameClient, MyPos, TargetPos, Dir))
			aHitPoints[HitPointsCount++] = Dir;
	}

	if(HitPointsCount <= 0)
		return vec2(0.0f, 0.0f);
	return aHitPoints[(HitPointsCount - 1) / 2];
}

bool FindPastaAimbotTarget(CGameClient *pGameClient, const CCharacter *pLocalCharacter, int Slot, vec2 RealAimPos, SPastaAimbotTarget *pOutTarget)
{
	if(pGameClient == nullptr || pLocalCharacter == nullptr || pOutTarget == nullptr || !GetPastaAimbotEnabled(Slot))
		return false;

	const vec2 LocalPos = pLocalCharacter->GetPos();
	const float Fov = GetPastaAimbotFov(Slot);
	const int Priority = GetPastaAimbotPriority(Slot);
	SPastaAimbotTarget BestTarget;

	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		if(!IsPastaTargetAllowed(pGameClient, pLocalCharacter, ClientId, Slot))
			continue;

		CCharacter *pTarget = pGameClient->m_PredictedWorld.GetCharacterById(ClientId);
		if(pTarget == nullptr)
			continue;

		vec2 WorldPos = pTarget->GetPos();
		vec2 AimPos = WorldPos - LocalPos;
		if(Slot == PASTA_AIMBOT_SLOT_HOOK)
			AimPos = PastaHookEdgeScan(pGameClient, pLocalCharacter, WorldPos, pTarget->Core()->m_Vel);
		else if(Slot == PASTA_AIMBOT_SLOT_HAMMER)
		{
			if(distance(LocalPos, WorldPos) > pLocalCharacter->GetProximityRadius())
				continue;
			if(!HasPastaDirectLineOfSight(pGameClient, LocalPos, WorldPos))
				continue;
		}
		else
		{
			WorldPos = PredictPastaWeaponTarget(pGameClient, pLocalCharacter, WorldPos, pTarget->Core()->m_Vel, Slot);
			if(!HasPastaDirectLineOfSight(pGameClient, LocalPos, WorldPos))
				continue;
			AimPos = WorldPos - LocalPos;
		}

		if(length(AimPos) < 0.001f)
			continue;
		if(PastaAngleDiff(RealAimPos, AimPos) > Fov * 0.5f)
			continue;

		const float Score = Priority == 0 ? PastaAngleDiff(RealAimPos, AimPos) : distance(LocalPos, pTarget->GetPos());
		if(BestTarget.m_ClientId == -1 || Score < BestTarget.m_Score)
		{
			BestTarget.m_ClientId = ClientId;
			BestTarget.m_AimPos = NormalizePastaAim(AimPos);
			BestTarget.m_WorldPos = WorldPos;
			BestTarget.m_Score = Score;
			BestTarget.m_Visible = true;
		}
	}

	if(BestTarget.m_ClientId < 0)
		return false;
	*pOutTarget = BestTarget;
	return true;
}

void UpdateAutoEdgeState(CControls *pControls, const CCollision *pCollision, int Dummy, CCharacter *pLocalCharacter)
{
	pControls->m_aPastaAutoEdgeFoundCount[Dummy] = 0;
	pControls->m_aPastaAutoEdgeLocked[Dummy] = false;
	pControls->m_aPastaAutoEdgeLockedPos[Dummy] = vec2(0.0f, 0.0f);

	if(!g_Config.m_PastaAutoEdge || pLocalCharacter == nullptr)
		return;

	const vec2 Pos = pLocalCharacter->GetPos();
	const float FootY = Pos.y + CCharacterCore::PhysicalSizeVec2().y * 0.5f + 4.0f;
	const float MarkerY = FootY - 8.0f;
	const int BaseTileX = (int)std::floor(Pos.x / gs_PastaTileSize);
	constexpr int SearchOffsets[] = {0, -1, 1};
	const int MaxPlatformScan = maximum(1, pCollision->GetWidth());
	for(const int Offset : SearchOffsets)
	{
		const int TileX = BaseTileX + Offset;
		if(!HasSupportTile(pCollision, TileX, FootY))
			continue;

		int LeftMost = TileX;
		int RightMost = TileX;
		int Safety = 0;
		while(Safety++ < MaxPlatformScan && HasSupportTile(pCollision, LeftMost - 1, FootY))
			--LeftMost;
		Safety = 0;
		while(Safety++ < MaxPlatformScan && HasSupportTile(pCollision, RightMost + 1, FootY))
			++RightMost;

		int FoundCount = 0;
		if(HasHazardOrVoidBeyondPlatform(pCollision, LeftMost, -1, FootY))
			pControls->m_aaPastaAutoEdgeFoundPos[Dummy][FoundCount++] = vec2(LeftMost * gs_PastaTileSize + 1.0f, MarkerY);
		if(HasHazardOrVoidBeyondPlatform(pCollision, RightMost, 1, FootY))
			pControls->m_aaPastaAutoEdgeFoundPos[Dummy][FoundCount++] = vec2((RightMost + 1) * gs_PastaTileSize - 1.0f, MarkerY);

		if(FoundCount <= 0)
			return;

		pControls->m_aPastaAutoEdgeFoundCount[Dummy] = FoundCount;
		const float DistLeft = absolute(Pos.x - pControls->m_aaPastaAutoEdgeFoundPos[Dummy][0].x);
		if(FoundCount == 1)
			pControls->m_aPastaAutoEdgeLockedPos[Dummy] = pControls->m_aaPastaAutoEdgeFoundPos[Dummy][0];
		else
		{
			const float DistRight = absolute(Pos.x - pControls->m_aaPastaAutoEdgeFoundPos[Dummy][1].x);
			pControls->m_aPastaAutoEdgeLockedPos[Dummy] = DistLeft < DistRight ? pControls->m_aaPastaAutoEdgeFoundPos[Dummy][0] : pControls->m_aaPastaAutoEdgeFoundPos[Dummy][1];
		}
		pControls->m_aPastaAutoEdgeLocked[Dummy] = true;
		return;
	}
}
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
	std::fill(std::begin(m_aPastaLastAutoAled), std::end(m_aPastaLastAutoAled), 0);
	std::fill(std::begin(m_aPastaAutoAledLatched), std::end(m_aPastaAutoAledLatched), false);
	std::fill(std::begin(m_aPastaLastSelfHitFire), std::end(m_aPastaLastSelfHitFire), 0);
	std::fill(std::begin(m_aPastaPendingSelfHitFireRelease), std::end(m_aPastaPendingSelfHitFireRelease), false);
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
	std::fill(std::begin(m_aPastaAimbotWeaponTargetId), std::end(m_aPastaAimbotWeaponTargetId), -1);
	std::fill(std::begin(m_aPastaAimbotHookTargetId), std::end(m_aPastaAimbotHookTargetId), -1);
	std::fill(std::begin(m_aPastaAimbotWeaponAimPos), std::end(m_aPastaAimbotWeaponAimPos), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aPastaAimbotHookAimPos), std::end(m_aPastaAimbotHookAimPos), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aPastaAimbotWeaponWorldPos), std::end(m_aPastaAimbotWeaponWorldPos), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aPastaAimbotHookWorldPos), std::end(m_aPastaAimbotHookWorldPos), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aPastaAimbotNextAutoShoot), std::end(m_aPastaAimbotNextAutoShoot), 0);
	std::fill(std::begin(m_aPastaAimbotScheduledTargetId), std::end(m_aPastaAimbotScheduledTargetId), -1);
	std::fill(std::begin(m_aPastaAutoEdgeFoundCount), std::end(m_aPastaAutoEdgeFoundCount), 0);
	std::fill(std::begin(m_aPastaAutoEdgeLocked), std::end(m_aPastaAutoEdgeLocked), false);
	std::fill(std::begin(m_aPastaAutoEdgeLockedPos), std::end(m_aPastaAutoEdgeLockedPos), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aPastaBlatantTrackPointInit), std::end(m_aPastaBlatantTrackPointInit), false);
	std::fill(std::begin(m_aPastaBlatantTrackPoint), std::end(m_aPastaBlatantTrackPoint), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aPastaBlatantHookTarget), std::end(m_aPastaBlatantHookTarget), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aPastaBlatantHookHoldTicks), std::end(m_aPastaBlatantHookHoldTicks), 0);
	std::fill(std::begin(m_aPastaBlatantFreezeResetFrozenLast), std::end(m_aPastaBlatantFreezeResetFrozenLast), false);
	std::fill(std::begin(m_aPastaBlatantFreezeResetSince), std::end(m_aPastaBlatantFreezeResetSince), 0);
	std::fill(std::begin(m_aPastaAvoidLastMousePos), std::end(m_aPastaAvoidLastMousePos), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_aPastaAvoidForcing), std::end(m_aPastaAvoidForcing), false);
	std::fill(std::begin(m_aPastaAvoidForcedDir), std::end(m_aPastaAvoidForcedDir), 0);
	std::fill(std::begin(m_aPastaAvoidForceUntil), std::end(m_aPastaAvoidForceUntil), 0);
	std::fill(std::begin(m_aPastaAvoidWasInDanger), std::end(m_aPastaAvoidWasInDanger), false);
	for(auto &aFoundPos : m_aaPastaAutoEdgeFoundPos)
		std::fill(std::begin(aFoundPos), std::end(aFoundPos), vec2(0.0f, 0.0f));
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
	m_aPastaLastAutoAled[Dummy] = 0;
	m_aPastaAutoAledLatched[Dummy] = false;
	m_aPastaLastSelfHitFire[Dummy] = 0;
	m_aPastaPendingSelfHitFireRelease[Dummy] = false;
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
	m_aPastaAimbotWeaponTargetId[Dummy] = -1;
	m_aPastaAimbotHookTargetId[Dummy] = -1;
	m_aPastaAimbotWeaponAimPos[Dummy] = vec2(0.0f, 0.0f);
	m_aPastaAimbotHookAimPos[Dummy] = vec2(0.0f, 0.0f);
	m_aPastaAimbotWeaponWorldPos[Dummy] = vec2(0.0f, 0.0f);
	m_aPastaAimbotHookWorldPos[Dummy] = vec2(0.0f, 0.0f);
	m_aPastaAimbotNextAutoShoot[Dummy] = 0;
	m_aPastaAimbotScheduledTargetId[Dummy] = -1;
	m_aPastaAutoEdgeFoundCount[Dummy] = 0;
	m_aPastaAutoEdgeLocked[Dummy] = false;
	m_aPastaAutoEdgeLockedPos[Dummy] = vec2(0.0f, 0.0f);
	m_aPastaBlatantTrackPointInit[Dummy] = false;
	m_aPastaBlatantTrackPoint[Dummy] = vec2(0.0f, 0.0f);
	m_aPastaBlatantHookTarget[Dummy] = vec2(0.0f, 0.0f);
	m_aPastaBlatantHookHoldTicks[Dummy] = 0;
	m_aPastaBlatantFreezeResetFrozenLast[Dummy] = false;
	m_aPastaBlatantFreezeResetSince[Dummy] = 0;
	m_aPastaAvoidLastMousePos[Dummy] = vec2(0.0f, 0.0f);
	m_aPastaAvoidForcing[Dummy] = false;
	m_aPastaAvoidForcedDir[Dummy] = 0;
	m_aPastaAvoidForceUntil[Dummy] = 0;
	m_aPastaAvoidWasInDanger[Dummy] = false;
	std::fill(std::begin(m_aaPastaAutoEdgeFoundPos[Dummy]), std::end(m_aaPastaAutoEdgeFoundPos[Dummy]), vec2(0.0f, 0.0f));
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
		m_aPastaAimbotWeaponTargetId[Dummy] = -1;
		m_aPastaAimbotHookTargetId[Dummy] = -1;
		m_aPastaAimbotWeaponAimPos[Dummy] = vec2(0.0f, 0.0f);
		m_aPastaAimbotHookAimPos[Dummy] = vec2(0.0f, 0.0f);
		m_aPastaAimbotWeaponWorldPos[Dummy] = vec2(0.0f, 0.0f);
		m_aPastaAimbotHookWorldPos[Dummy] = vec2(0.0f, 0.0f);
		m_aPastaRenderMouseOverride[Dummy] = false;
		m_aPastaRenderMouseTargetId[Dummy] = -1;
		m_aPastaAssistAimActive[Dummy] = false;
		m_aPastaAssistAimWeapon[Dummy] = -1;
		m_aPastaAssistAimPos[Dummy] = vec2(0.0f, 0.0f);
		if(m_aPastaPendingSelfHitFireRelease[Dummy])
		{
			PulseFire(m_aInputData[Dummy]);
			m_aPastaPendingSelfHitFireRelease[Dummy] = false;
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
			const int FreezeSide = DetectFreezeSide(Collision(), pLocalCharacter->GetPos());
			const bool MovingRight = m_aInputData[Dummy].m_Direction > 0 || pLocalCharacter->Core()->m_Vel.x > 0.35f;
			const bool MovingLeft = m_aInputData[Dummy].m_Direction < 0 || pLocalCharacter->Core()->m_Vel.x < -0.35f;
			if(FreezeSide > 0 && MovingRight && HasImmediateSideFreeze(Collision(), pLocalCharacter, 1))
				m_aInputData[Dummy].m_Direction = -1;
			else if(FreezeSide < 0 && MovingLeft && HasImmediateSideFreeze(Collision(), pLocalCharacter, -1))
				m_aInputData[Dummy].m_Direction = 1;
		}

		if(g_Config.m_PastaAvoidfreeze && g_Config.m_PastaAvoidMode == 1)
		{
			PastaAvoidFreeze();
			PastaHookAssist();
			if(m_aPastaAvoidForcing[Dummy] && Now <= m_aPastaAvoidForceUntil[Dummy])
				m_aInputData[Dummy].m_Direction = m_aPastaAvoidForcedDir[Dummy];
			else
				m_aPastaAvoidForcing[Dummy] = false;
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
			const bool FrozenNow = (pData && pData->m_FreezeEnd > 0) || IsBlatantFrozenAtPos(Collision(), LocalPos);

			auto InBlatantFov = [&](vec2 AimDir) {
				const vec2 BaseAim = normalize(EnsureValidAim(RealAimPos));
				if(g_Config.m_PastaAvoidBlatantAimbotFov >= 360)
					return true;
				const float FovCos = cosf(g_Config.m_PastaAvoidBlatantAimbotFov * pi / 360.0f);
				return dot(BaseAim, normalize(AimDir)) >= FovCos;
			};

			if(!FrozenNow && SimulateBlatantFreezeTick(GameClient(), LocalId, m_aInputData[Dummy], PredictTicks, DetectFreeze, DetectDeath, DetectTele, DetectUnfreeze) >= 0)
			{
				if(g_Config.m_PastaAvoidBlatantOnlyHorizontally)
				{
					for(int Dir = -1; Dir <= 1; Dir += 2)
					{
						CNetObj_PlayerInput Input = m_aInputData[Dummy];
						Input.m_Direction = Dir;
						if(SimulateBlatantFreezeTick(GameClient(), LocalId, Input, PredictTicks, DetectFreeze, DetectDeath, DetectTele, DetectUnfreeze) < 0)
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
							if(SimulateBlatantFreezeTick(GameClient(), LocalId, TryInput, PredictTicks, DetectFreeze, DetectDeath, DetectTele, DetectUnfreeze) < 0)
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

							const int CandidateDangerTick = SimulateBlatantFreezeTick(GameClient(), LocalId, TryInput, PredictTicks, DetectFreeze, DetectDeath, DetectTele, DetectUnfreeze);
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

									const int CandidateDangerTick = SimulateBlatantFreezeTick(GameClient(), LocalId, TryInput, PredictTicks, DetectFreeze, DetectDeath, DetectTele, DetectUnfreeze);
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
						if(SimulateBlatantFreezeTick(GameClient(), LocalId, Tick1, 1, DetectFreeze, DetectDeath, DetectTele, DetectUnfreeze) < 0)
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

							if(SimulateBlatantFreezeTick(GameClient(), LocalId, Tick2, 1, DetectFreeze, DetectDeath, DetectTele, DetectUnfreeze) < 0)
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

								const int FreezeTick = SimulateBlatantFreezeTick(GameClient(), LocalId, TryInput, PredictTicks, DetectFreeze, DetectDeath, DetectTele, DetectUnfreeze);
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
						if(!m_aPastaBlatantFreezeResetFrozenLast[Dummy])
							m_aPastaBlatantFreezeResetSince[Dummy] = Now;
						else if(Now > m_aPastaBlatantFreezeResetSince[Dummy] + (int64_t)g_Config.m_PastaAvoidBlatantFreezeResetWait * time_freq())
							GameClient()->SendKill();
					}
				}
				m_aPastaBlatantFreezeResetFrozenLast[Dummy] = FrozenNow;
			}
		}
		else
		{
			m_aPastaBlatantTrackPointInit[Dummy] = false;
			m_aPastaBlatantHookHoldTicks[Dummy] = 0;
			m_aPastaBlatantHookTarget[Dummy] = vec2(0.0f, 0.0f);
		}

		if(g_Config.m_PastaQuickStop && !LeftHeld && !RightHeld && pLocalCharacter != nullptr)
		{
			const bool GroundedOkay = !g_Config.m_PastaQuickStopGrounded || pLocalCharacter->IsGrounded();
			if(GroundedOkay && absolute(pLocalCharacter->Core()->m_Vel.x) > 0.6f)
				m_aInputData[Dummy].m_Direction = pLocalCharacter->Core()->m_Vel.x > 0.0f ? -1 : 1;
		}

		if(g_Config.m_PastaAutoJumpSave && !JumpHeld && pLocalCharacter != nullptr && ShouldAutoJumpSave(Collision(), pLocalCharacter))
			m_aInputData[Dummy].m_Jump = 1;

		if(g_Config.m_PastaAutoRehook && pLocalCharacter != nullptr)
		{
			const bool TargetedPlayer = HasPlayerNearAim(GameClient(), pLocalCharacter, RealAimPos, 380.0f, 0.18f);
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
			PulseFire(m_aInputData[Dummy]);

		if(g_Config.m_PastaAutoAled && pLocalCharacter != nullptr)
		{
			const bool ValidAled = FindAutoAledTarget(GameClient(), pLocalCharacter, RealAimPos);
			const int64_t Cooldown = gs_PastaAutoAledCooldownMs * time_freq() / 1000;
			if(ValidAled && !m_aPastaAutoAledLatched[Dummy] && Now > m_aPastaLastAutoAled[Dummy] + Cooldown)
			{
				PulseFire(m_aInputData[Dummy]);
				m_aPastaLastAutoAled[Dummy] = Now;
			}
			m_aPastaAutoAledLatched[Dummy] = ValidAled;
		}

		const bool ActionFrame = m_aInputData[Dummy].m_Hook != 0 || HookState > HOOK_IDLE || m_aInputData[Dummy].m_Fire != m_aLastData[Dummy].m_Fire;
		if(g_Config.m_PastaFakeAim)
		{
			const bool RefreshAim = g_Config.m_PastaFakeAimMode == 1 ? ActionFrame :
				(g_Config.m_PastaFakeAimSendAlways != 0 || ActionFrame || length(m_aPastaSentMousePos[Dummy]) < 0.001f);
			if(RefreshAim)
				m_aPastaSentMousePos[Dummy] = BuildFakeAimPos(GameClient(), Dummy, RealAimPos, pLocalCharacter, Now);

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
				ActiveWeapon == WEAPON_HAMMER ? PASTA_AIMBOT_SLOT_HAMMER :
				ActiveWeapon == WEAPON_GUN ? PASTA_AIMBOT_SLOT_GUN :
				ActiveWeapon == WEAPON_SHOTGUN ? PASTA_AIMBOT_SLOT_SHOTGUN :
				ActiveWeapon == WEAPON_GRENADE ? PASTA_AIMBOT_SLOT_GRENADE :
				ActiveWeapon == WEAPON_LASER ? PASTA_AIMBOT_SLOT_LASER : -999;

			SPastaAimbotTarget HookTarget;
			const bool WantsAutoHook = g_Config.m_PastaAimbotAutoHookKey || g_Config.m_PastaAimbotAutoShoot || g_Config.m_PastaAimbotAutoShootKey;
			const bool HasHookTarget = GetPastaAimbotEnabled(PASTA_AIMBOT_SLOT_HOOK) &&
				FindPastaAimbotTarget(GameClient(), pLocalCharacter, PASTA_AIMBOT_SLOT_HOOK, RealAimPos, &HookTarget);
			if(HasHookTarget)
			{
				m_aPastaAimbotHookTargetId[Dummy] = HookTarget.m_ClientId;
				m_aPastaAimbotHookAimPos[Dummy] = HookTarget.m_AimPos;
				m_aPastaAimbotHookWorldPos[Dummy] = HookTarget.m_WorldPos;
				if(HookHeld || WantsAutoHook)
				{
					SentAimPos = HookTarget.m_AimPos;
					if(!GetPastaAimbotSilent(PASTA_AIMBOT_SLOT_HOOK))
					{
						VisibleAimPos = HookTarget.m_AimPos;
						m_aPastaRenderMouseOverride[Dummy] = true;
						m_aPastaRenderMouseTargetId[Dummy] = HookTarget.m_ClientId;
					}
					if(WantsAutoHook)
						m_aInputData[Dummy].m_Hook = 1;
				}
			}

			SPastaAimbotTarget WeaponTarget;
			const bool HasWeaponTarget = WeaponSlot != -999 && GetPastaAimbotEnabled(WeaponSlot) &&
				FindPastaAimbotTarget(GameClient(), pLocalCharacter, WeaponSlot, RealAimPos, &WeaponTarget);
			const bool WantsWeaponAim = WeaponSlot != -999 && GetPastaAimbotEnabled(WeaponSlot) &&
				(FireHeld || g_Config.m_PastaAimbotAutoShoot || g_Config.m_PastaAimbotAutoShootKey);
			if(HasWeaponTarget)
			{
				m_aPastaAimbotWeaponTargetId[Dummy] = WeaponTarget.m_ClientId;
				m_aPastaAimbotWeaponAimPos[Dummy] = WeaponTarget.m_AimPos;
				m_aPastaAimbotWeaponWorldPos[Dummy] = WeaponTarget.m_WorldPos;
			}
			if(WantsWeaponAim && HasWeaponTarget)
			{
				SentAimPos = WeaponTarget.m_AimPos;
				if(!GetPastaAimbotSilent(WeaponSlot))
				{
					VisibleAimPos = WeaponTarget.m_AimPos;
					m_aPastaRenderMouseOverride[Dummy] = true;
					m_aPastaRenderMouseTargetId[Dummy] = WeaponTarget.m_ClientId;
				}

				if(g_Config.m_PastaAimbotAutoShoot || g_Config.m_PastaAimbotAutoShootKey)
				{
					const bool TargetChanged = m_aPastaAimbotScheduledTargetId[Dummy] != WeaponTarget.m_ClientId;
					if(TargetChanged || m_aPastaAimbotNextAutoShoot[Dummy] == 0)
					{
						const int MinDelay = minimum(g_Config.m_PastaAimbotMinShootDelay, g_Config.m_PastaAimbotMaxShootDelay);
						const int MaxDelay = maximum(g_Config.m_PastaAimbotMinShootDelay, g_Config.m_PastaAimbotMaxShootDelay);
						const int DelayTicks = MinDelay + (MaxDelay > MinDelay ? rand() % (MaxDelay - MinDelay + 1) : 0);
						m_aPastaAimbotNextAutoShoot[Dummy] = Now + DelayTicks * time_freq() / Client()->GameTickSpeed();
						m_aPastaAimbotScheduledTargetId[Dummy] = WeaponTarget.m_ClientId;
					}

					if(IsWeaponReadyForSelfHit(GameClient(), pLocalCharacter, ActiveWeapon) &&
						Now >= m_aPastaAimbotNextAutoShoot[Dummy] &&
						!m_aPastaPendingSelfHitFireRelease[Dummy])
					{
						TriggerFireTap(this, Dummy);
						m_aPastaAimbotNextAutoShoot[Dummy] = 0;
					}
				}
				else
				{
					m_aPastaAimbotNextAutoShoot[Dummy] = 0;
					m_aPastaAimbotScheduledTargetId[Dummy] = -1;
				}
			}
			else
			{
				m_aPastaAimbotNextAutoShoot[Dummy] = 0;
				m_aPastaAimbotScheduledTargetId[Dummy] = -1;
			}
		}

		const bool NeedUnfreeze = pLocalCharacter != nullptr &&
			(pLocalCharacter->m_FreezeTime > 0 || HasImpendingFreeze(Collision(), pLocalCharacter, maximum(2, g_Config.m_PastaUnfreezeBotCurDirTicks)));
		if(NeedUnfreeze)
		{
			vec2 SilentAimPos = SentAimPos;
			float TimingError = 999.0f;
			const int64_t SelfHitCooldown = gs_PastaSelfHitCooldownMs * time_freq() / 1000;
			const int LaserLookTicks = maximum(maximum(2, g_Config.m_PastaUnfreezeBotTicks), pLocalCharacter != nullptr ? pLocalCharacter->m_FreezeTime + 3 : 3);
			if(g_Config.m_PastaUnfreezeBot && FindBestSelfHitAim(GameClient(), pLocalCharacter, RealAimPos, WEAPON_LASER, LaserLookTicks, &SilentAimPos, &TimingError))
			{
				m_aPastaAssistAimActive[Dummy] = true;
				m_aPastaAssistAimWeapon[Dummy] = WEAPON_LASER;
				m_aPastaAssistAimPos[Dummy] = SilentAimPos;
				m_aInputData[Dummy].m_WantedWeapon = WEAPON_LASER + 1;
				if(pLocalCharacter->GetActiveWeapon() == WEAPON_LASER && IsWeaponReadyForSelfHit(GameClient(), pLocalCharacter, WEAPON_LASER) &&
					!m_aPastaPendingSelfHitFireRelease[Dummy] &&
					Now > m_aPastaLastSelfHitFire[Dummy] + SelfHitCooldown)
				{
					SentAimPos = SilentAimPos;
					if(!g_Config.m_PastaUnfreezeBotSilent)
						VisibleAimPos = SilentAimPos;
					TriggerFireTap(this, Dummy);
					m_aPastaLastSelfHitFire[Dummy] = Now;
				}
			}
		}

		if(g_Config.m_PastaAutoShotgun && pLocalCharacter != nullptr)
		{
			vec2 SilentAimPos = SentAimPos;
			const int64_t SelfHitCooldown = gs_PastaSelfHitCooldownMs * time_freq() / 1000;
			if(FindBestSelfHitAim(GameClient(), pLocalCharacter, RealAimPos, WEAPON_SHOTGUN, maximum(2, g_Config.m_PastaAutoShotgunTicks), &SilentAimPos))
			{
				m_aPastaAssistAimActive[Dummy] = true;
				m_aPastaAssistAimWeapon[Dummy] = WEAPON_SHOTGUN;
				m_aPastaAssistAimPos[Dummy] = SilentAimPos;
				m_aInputData[Dummy].m_WantedWeapon = WEAPON_SHOTGUN + 1;
				if(pLocalCharacter->GetActiveWeapon() == WEAPON_SHOTGUN && IsWeaponReadyForSelfHit(GameClient(), pLocalCharacter, WEAPON_SHOTGUN) &&
					!m_aPastaPendingSelfHitFireRelease[Dummy] && Now > m_aPastaLastSelfHitFire[Dummy] + SelfHitCooldown)
				{
					SentAimPos = SilentAimPos;
					if(!g_Config.m_PastaAutoShotgunSilent)
						VisibleAimPos = SilentAimPos;
					TriggerFireTap(this, Dummy);
					m_aPastaLastSelfHitFire[Dummy] = Now;
				}
			}
		}

		if(g_Config.m_PastaGhostMove && (g_Config.m_PastaGhostMoveHook || g_Config.m_PastaGhostMoveHookClosest))
		{
			// Same as above: don't inject fake hook packets until a real packet-level implementation exists.
		}

		UpdateAutoEdgeState(this, Collision(), Dummy, pLocalCharacter);
		if(g_Config.m_PastaAutoEdge && pLocalCharacter != nullptr && m_aPastaAutoEdgeLocked[Dummy] && pLocalCharacter->Core()->m_Vel.y >= -0.5f)
		{
			const float Delta = m_aPastaAutoEdgeLockedPos[Dummy].x - pLocalCharacter->GetPos().x;
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

vec2 CControls::GetRenderMousePos(int Dummy) const
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
