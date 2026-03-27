#include "pasta_pilot.h"

#include <base/math.h>
#include <base/system.h>
#include <base/vmath.h>

#include <engine/client.h>
#include <engine/shared/config.h>

#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>
#include <game/collision.h>

#include <algorithm>
#include <limits>
#include <queue>

namespace
{
struct SPilotHookAnchor
{
	vec2 m_Pos{};
	float m_Score = -std::numeric_limits<float>::infinity();
};

bool IsFreezeTileIndex(int Tile)
{
	return Tile == TILE_FREEZE || Tile == TILE_DFREEZE || Tile == TILE_LFREEZE;
}

bool IsDeathTileIndex(int Tile)
{
	return Tile == TILE_DEATH;
}

bool IsFinishTileIndex(int Tile)
{
	return Tile == TILE_FINISH;
}

bool IsHazardTile(const CCollision *pCollision, int Index)
{
	if(!pCollision || Index < 0)
		return true;
	const int Tile = pCollision->GetTileIndex(Index);
	const int FrontTile = pCollision->GetFrontTileIndex(Index);
	return IsFreezeTileIndex(Tile) || IsFreezeTileIndex(FrontTile) || IsDeathTileIndex(Tile) || IsDeathTileIndex(FrontTile);
}

bool IsBlockedTile(const CCollision *pCollision, int Index)
{
	if(!pCollision || Index < 0)
		return true;
	const int Tile = pCollision->GetTileIndex(Index);
	const int FrontTile = pCollision->GetFrontTileIndex(Index);
	return Tile == TILE_SOLID || FrontTile == TILE_SOLID || IsHazardTile(pCollision, Index);
}

bool TouchesHazard(const CCollision *pCollision, vec2 Pos)
{
	if(!pCollision)
		return true;

	const vec2 aChecks[] = {
		vec2(0.0f, 0.0f),
		vec2(-12.0f, 0.0f),
		vec2(12.0f, 0.0f),
		vec2(0.0f, -14.0f),
		vec2(0.0f, 14.0f),
		vec2(-12.0f, 14.0f),
		vec2(12.0f, 14.0f),
	};
	for(const vec2 &Offset : aChecks)
	{
		const int Index = pCollision->GetPureMapIndex(Pos + Offset);
		if(Index >= 0 && IsHazardTile(pCollision, Index))
			return true;
	}
	return false;
}

bool HasSolidSupport(const CCollision *pCollision, vec2 Pos)
{
	return pCollision != nullptr && pCollision->TestBox(Pos + vec2(0.0f, 22.0f), vec2(12.0f, 4.0f));
}

void EnsureAimNonZero(CNetObj_PlayerInput &Input)
{
	if(Input.m_TargetX == 0 && Input.m_TargetY == 0)
		Input.m_TargetX = 1;
}

vec2 PickNearestFinish(const std::vector<vec2> &vFinishTiles, vec2 Pos, bool &HasFinish)
{
	HasFinish = false;
	float BestDist = std::numeric_limits<float>::max();
	vec2 Best(0.0f, 0.0f);
	for(const vec2 &FinishPos : vFinishTiles)
	{
		const float Dist = distance(Pos, FinishPos);
		if(Dist < BestDist)
		{
			BestDist = Dist;
			Best = FinishPos;
			HasFinish = true;
		}
	}
	return Best;
}

void BuildFlowField(const CCollision *pCollision, int Width, int Height, const std::vector<int> &vFinishIndices, std::vector<int8_t> &vOutDirX, std::vector<int8_t> &vOutDirY)
{
	const int Total = Width * Height;
	vOutDirX.assign(Total, 0);
	vOutDirY.assign(Total, 0);
	std::vector<int> vDist(Total, -1);
	std::queue<int> Queue;

	for(const int Index : vFinishIndices)
	{
		if(Index < 0 || Index >= Total)
			continue;
		vDist[Index] = 0;
		Queue.push(Index);
	}

	static const int s_aOffsets[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
	while(!Queue.empty())
	{
		const int Cur = Queue.front();
		Queue.pop();
		const int CurX = Cur % Width;
		const int CurY = Cur / Width;
		for(const auto &Offset : s_aOffsets)
		{
			const int Nx = CurX + Offset[0];
			const int Ny = CurY + Offset[1];
			if(Nx < 0 || Ny < 0 || Nx >= Width || Ny >= Height)
				continue;
			const int Next = Ny * Width + Nx;
			if(vDist[Next] != -1 || IsBlockedTile(pCollision, Next))
				continue;
			vDist[Next] = vDist[Cur] + 1;
			vOutDirX[Next] = (int8_t)(CurX - Nx);
			vOutDirY[Next] = (int8_t)(CurY - Ny);
			Queue.push(Next);
		}
	}
}

bool FindNearestFlowDir(const CCollision *pCollision, int Width, int Height, int StartIndex, const std::vector<int8_t> &vDirX, const std::vector<int8_t> &vDirY, int Radius, int &OutDirX, int &OutDirY)
{
	if(!pCollision || Width <= 0 || Height <= 0 || StartIndex < 0)
		return false;

	const int StartX = StartIndex % Width;
	const int StartY = StartIndex / Width;
	int BestDist = std::numeric_limits<int>::max();
	bool Found = false;
	for(int dy = -Radius; dy <= Radius; ++dy)
	{
		for(int dx = -Radius; dx <= Radius; ++dx)
		{
			const int Nx = StartX + dx;
			const int Ny = StartY + dy;
			if(Nx < 0 || Ny < 0 || Nx >= Width || Ny >= Height)
				continue;
			const int Index = Ny * Width + Nx;
			if(IsBlockedTile(pCollision, Index))
				continue;
			const int DirXVal = vDirX[Index];
			const int DirYVal = vDirY[Index];
			if(DirXVal == 0 && DirYVal == 0)
				continue;
			const int Dist = abs(dx) + abs(dy);
			if(Dist < BestDist)
			{
				BestDist = Dist;
				OutDirX = DirXVal;
				OutDirY = DirYVal;
				Found = true;
			}
		}
	}
	return Found;
}

int CollectHookTargets(const CCollision *pCollision, const std::vector<vec2> &vHookTiles, vec2 Pos, vec2 Vel, vec2 GoalPos, SPilotHookAnchor *pOutAnchors, int MaxAnchors)
{
	if(!pCollision || pOutAnchors == nullptr || MaxAnchors <= 0)
		return 0;

	const float HookLength = 380.0f;
	const vec2 ToGoal = length(GoalPos - Pos) > 0.001f ? normalize(GoalPos - Pos) : vec2(1.0f, 0.0f);
	std::vector<SPilotHookAnchor> vCandidates;
	vCandidates.reserve(64);

	for(const vec2 &TilePos : vHookTiles)
	{
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
		const float UpPull = maximum(0.0f, -Dir.y);
		const float DownPull = maximum(0.0f, Dir.y);
		const float Forward = Dir.x * (GoalPos.x >= Pos.x ? 1.0f : -1.0f);
		float Score =
			dot(Dir, ToGoal) * 6.0f +
			Forward * 7.5f +
			UpPull * 8.5f -
			DownPull * 7.0f -
			(Dist / HookLength) * 2.0f;
		if(length(Vel) > 2.0f)
			Score += dot(normalize(Vel), Dir) * 2.0f;
		if(TilePos.y < Pos.y - 8.0f)
			Score += 2.5f;
		if(TilePos.y > Pos.y + 24.0f)
			Score -= 4.0f;
		if(std::abs(Dir.x) < 0.16f)
			Score -= 1.5f;

		vCandidates.push_back({TilePos, Score});
	}

	if(vCandidates.empty())
		return 0;

	std::sort(vCandidates.begin(), vCandidates.end(), [](const SPilotHookAnchor &Left, const SPilotHookAnchor &Right) {
		return Left.m_Score > Right.m_Score;
	});

	int Count = 0;
	for(const SPilotHookAnchor &Candidate : vCandidates)
	{
		bool TooCloseAngle = false;
		for(int i = 0; i < Count; ++i)
		{
			const vec2 A = normalize(Candidate.m_Pos - Pos);
			const vec2 B = normalize(pOutAnchors[i].m_Pos - Pos);
			if(dot(A, B) > 0.94f)
			{
				TooCloseAngle = true;
				break;
			}
		}
		if(TooCloseAngle)
			continue;
		pOutAnchors[Count++] = Candidate;
		if(Count >= MaxAnchors)
			break;
	}

	if(Count == 0)
	{
		pOutAnchors[0] = vCandidates.front();
		return 1;
	}
	return Count;
}

float EvaluateSafetyScore(vec2 StartPos, vec2 EndPos, vec2 GoalPos, float EndSpeed, bool FullSafe, int HookTicks, int DirectionSwitches, int IdleTicks)
{
	const float GoalSign = GoalPos.x >= StartPos.x ? 1.0f : -1.0f;
	const float HorizontalProgress = (EndPos.x - StartPos.x) * GoalSign;
	const float VerticalProgress = StartPos.y - EndPos.y;
	const float DistGain = distance(StartPos, GoalPos) - distance(EndPos, GoalPos);
	float Score =
		(FullSafe ? 25000.0f : 0.0f) +
		DistGain * 12.0f +
		HorizontalProgress * 14.0f +
		VerticalProgress * 4.0f +
		EndSpeed * 8.0f -
		HookTicks * 8.0f -
		DirectionSwitches * 28.0f -
		IdleTicks * 22.0f;
	if(HorizontalProgress < 0.0f)
		Score -= 800.0f;
	return Score;
}
}

void CPastaPilot::OnReset()
{
	Stop();
}

void CPastaPilot::OnStateChange(int NewState, int OldState)
{
	(void)OldState;
	if(NewState != IClient::STATE_ONLINE)
		Stop();
}

void CPastaPilot::Start()
{
	Stop();
	CacheTiles();
	BuildFlowField();
	m_Running = m_TilesCached && m_FieldReady && !m_vFinishTiles.empty();
	m_LastUpdateTime = time_get();
	m_Accumulator = 0.0;
	m_RuntimeHookHoldTicks = 0;
	m_RuntimeHookCooldownTicks = 0;
	m_RuntimeHookAim = vec2(0.0f, 0.0f);
	m_RuntimeStuckTicks = 0;
	m_RuntimeLastPos = vec2(0.0f, 0.0f);
	m_RuntimeLastDir = 0;
}

void CPastaPilot::Stop()
{
	m_Running = false;
	m_HasRealtimeInput = false;
	m_RealtimeInput = {};
	m_RealtimeMouse = vec2(0.0f, 0.0f);
	m_vPlan.clear();
	m_ExecCursor = 0;
	m_ReplanCountdown = 0;
	m_Accumulator = 0.0;
	m_LastUpdateTime = 0;
	m_vFinishTiles.clear();
	m_vFinishTileIndices.clear();
	m_vHookTiles.clear();
	m_vFieldDirX.clear();
	m_vFieldDirY.clear();
	m_FieldWidth = 0;
	m_FieldHeight = 0;
	m_TilesCached = false;
	m_FieldReady = false;
	m_RuntimeHookHoldTicks = 0;
	m_RuntimeHookCooldownTicks = 0;
	m_RuntimeHookAim = vec2(0.0f, 0.0f);
	m_RuntimeStuckTicks = 0;
	m_RuntimeLastPos = vec2(0.0f, 0.0f);
	m_RuntimeLastDir = 0;
}

void CPastaPilot::CacheTiles()
{
	m_vFinishTiles.clear();
	m_vFinishTileIndices.clear();
	m_vHookTiles.clear();
	m_TilesCached = false;

	const CCollision *pCollision = Collision();
	if(!pCollision)
		return;

	m_FieldWidth = pCollision->GetWidth();
	m_FieldHeight = pCollision->GetHeight();
	for(int y = 0; y < m_FieldHeight; ++y)
	{
		for(int x = 0; x < m_FieldWidth; ++x)
		{
			const int Index = y * m_FieldWidth + x;
			const int Tile = pCollision->GetTileIndex(Index);
			const int FrontTile = pCollision->GetFrontTileIndex(Index);
			if(IsFinishTileIndex(Tile) || IsFinishTileIndex(FrontTile))
			{
				m_vFinishTiles.push_back(pCollision->GetPos(Index));
				m_vFinishTileIndices.push_back(Index);
			}
			if(Tile == TILE_SOLID && FrontTile != TILE_NOHOOK)
				m_vHookTiles.push_back(pCollision->GetPos(Index));
		}
	}
	m_TilesCached = true;
}

void CPastaPilot::BuildFlowField()
{
	if(!m_TilesCached || !Collision())
		return;
	::BuildFlowField(Collision(), m_FieldWidth, m_FieldHeight, m_vFinishTileIndices, m_vFieldDirX, m_vFieldDirY);
	const int Total = m_FieldWidth * m_FieldHeight;
	m_FieldReady = m_FieldWidth > 0 && m_FieldHeight > 0 && (int)m_vFieldDirX.size() == Total && (int)m_vFieldDirY.size() == Total;
}

bool CPastaPilot::ResolveGoalPos(vec2 LocalPos, vec2 &OutGoalPos) const
{
	const int Mode = g_Config.m_PastaAvoidPilotMode;
	if(Mode == 1)
	{
		OutGoalPos = GameClient()->m_Controls.m_aTargetPos[g_Config.m_ClDummy];
		return true;
	}
	if(Mode == 2)
	{
		float BestDist = std::numeric_limits<float>::max();
		bool Found = false;
		for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
		{
			if(ClientId == GameClient()->m_Snap.m_LocalClientId || !GameClient()->m_Snap.m_aCharacters[ClientId].m_Active)
				continue;
			const vec2 Pos = GameClient()->m_aClients[ClientId].m_RenderPos;
			const float Dist = distance(LocalPos, Pos);
			if(Dist < BestDist)
			{
				BestDist = Dist;
				OutGoalPos = Pos;
				Found = true;
			}
		}
		if(Found)
			return true;
	}

	bool HasFinish = false;
	OutGoalPos = PickNearestFinish(m_vFinishTiles, LocalPos, HasFinish);
	return HasFinish;
}

bool CPastaPilot::BuildPlan()
{
	m_vPlan.clear();
	m_ExecCursor = 0;
	m_ReplanCountdown = 0;
	m_HasRealtimeInput = false;

	if(!GameClient()->Predict() || !Collision() || !m_FieldReady)
		return false;

	const int LocalId = GameClient()->m_aLocalIds[g_Config.m_ClDummy];
	if(LocalId < 0)
		return false;

	CCharacter *pPredChar = GameClient()->m_PredictedWorld.GetCharacterById(LocalId);
	if(!pPredChar)
		return false;

	vec2 GoalPos;
	if(!ResolveGoalPos(pPredChar->GetPos(), GoalPos))
		return false;

	const int Population = std::clamp(g_Config.m_PastaAvoidPilotPopulationSize, 32, 1024);
	const int Horizon = std::clamp(g_Config.m_PastaAvoidPilotExplorationDepth, 4, 50);
	const int Commit = std::clamp(g_Config.m_PastaAvoidPilotSequenceLength, 2, minimum(Horizon, 10));
	const int TopK = std::clamp(g_Config.m_PastaAvoidPilotTopKCandidates, 4, 128);
	const int TotalCandidates = minimum(Population * maximum(1, TopK / 8), 2048);

	float BestScore = -std::numeric_limits<float>::infinity();
	std::vector<SFrame> vBestFrames;

	for(int Variant = 0; Variant < TotalCandidates; ++Variant)
	{
		CGameWorld SimWorld;
		SimWorld.CopyWorldClean(&GameClient()->m_PredictedWorld);
		CCharacter *pSimChar = SimWorld.GetCharacterById(LocalId);
		if(!pSimChar)
			continue;

		const vec2 StartPos = pSimChar->GetPos();
		std::vector<SFrame> vFrames;
		vFrames.reserve(Commit);
		int HookTicks = 0;
		int IdleTicks = 0;
		int DirectionSwitches = 0;
		int LastDir = 0;
		bool FullSafe = true;
		int HookLatchTicks = 0;
		vec2 HookLatchAim(0.0f, 0.0f);
		const int HookStartTick = (Variant / 9) % maximum(1, minimum(8, Horizon));
		const int HookHoldTicks = 24 + ((Variant / 23) % 12);
		const bool PreferNoHook = (Variant % 6) == 0;

		for(int Tick = 0; Tick < Horizon; ++Tick)
		{
			const vec2 Pos = pSimChar->GetPos();
			const vec2 Vel = pSimChar->Core() != nullptr ? pSimChar->Core()->m_Vel : vec2(0.0f, 0.0f);
			const int Index = Collision()->GetPureMapIndex(Pos);

			int FlowX = 0;
			int FlowY = 0;
			if(Index >= 0 && Index < (int)m_vFieldDirX.size())
			{
				FlowX = m_vFieldDirX[Index];
				FlowY = m_vFieldDirY[Index];
			}
			if((FlowX == 0 && FlowY == 0) && Index >= 0)
				FindNearestFlowDir(Collision(), m_FieldWidth, m_FieldHeight, Index, m_vFieldDirX, m_vFieldDirY, 8, FlowX, FlowY);

			CNetObj_PlayerInput Input{};
			const vec2 ToGoal = GoalPos - Pos;
			const bool Grounded = pSimChar->IsGrounded();
			const float DirSign = ToGoal.x > 0.0f ? 1.0f : ToGoal.x < 0.0f ? -1.0f : 0.0f;
			const bool BlockedAhead = Collision()->TestBox(Pos + vec2(DirSign * 18.0f, 0.0f), vec2(28.0f, 28.0f));
			const bool NoSupportAhead = !HasSolidSupport(Collision(), Pos + vec2(DirSign * 24.0f, 0.0f));
			const bool NeedsUp = ToGoal.y < -16.0f || FlowY < 0;

			if(std::abs(ToGoal.x) > 6.0f)
				Input.m_Direction = ToGoal.x > 0.0f ? 1 : -1;
			else if(FlowX != 0)
				Input.m_Direction = FlowX > 0 ? 1 : -1;

			if(Tick == 0 && (Variant % 9) == 1)
				Input.m_Direction = -Input.m_Direction;
			if(Tick == 0 && (Variant % 11) == 3)
				Input.m_Direction = 0;
			if(BlockedAhead && !NeedsUp)
				Input.m_Direction = 0;

			const bool AllowJump = Grounded && (NeedsUp || BlockedAhead || (NoSupportAhead && (Variant % 3) == 0));
			if(AllowJump)
				Input.m_Jump = 1;

			bool TryHook = !PreferNoHook && (!Grounded || NeedsUp || (NoSupportAhead && std::abs(ToGoal.x) > 32.0f));
			if((Variant % 5) == 0)
				TryHook = true;
			if((Variant % 7) == 0 && Grounded)
				TryHook = false;
			if(Tick < HookStartTick)
				TryHook = false;

			vec2 Aim = length(ToGoal) > 0.001f ? ToGoal : vec2((float)(Input.m_Direction == 0 ? 1 : Input.m_Direction), 0.0f);
			if(HookLatchTicks > 0)
			{
				Input.m_Hook = 1;
				Aim = HookLatchAim;
				--HookLatchTicks;
			}
			else if(TryHook && Tick == HookStartTick)
			{
				SPilotHookAnchor aAnchors[6];
				const int AnchorCount = CollectHookTargets(Collision(), m_vHookTiles, Pos, Vel, GoalPos, aAnchors, std::size(aAnchors));
				if(AnchorCount > 0)
				{
					const int AnchorIndex = (Variant / 3 + Tick) % AnchorCount;
					Input.m_Hook = 1;
					Aim = aAnchors[AnchorIndex].m_Pos - Pos;
					HookLatchAim = Aim;
					HookLatchTicks = HookHoldTicks - 1;
				}
			}

			Input.m_TargetX = (int)Aim.x;
			Input.m_TargetY = (int)Aim.y;
			EnsureAimNonZero(Input);

			if(LastDir != 0 && Input.m_Direction != 0 && Input.m_Direction != LastDir)
				++DirectionSwitches;
			LastDir = Input.m_Direction;
			if(Input.m_Hook)
				++HookTicks;

			pSimChar->OnDirectInput(&Input);
			pSimChar->OnDirectInput(&Input);
			pSimChar->OnPredictedInput(&Input);
			SimWorld.m_GameTick++;
			SimWorld.Tick();
			pSimChar = SimWorld.GetCharacterById(LocalId);
			if(!pSimChar)
			{
				FullSafe = false;
				break;
			}

			const vec2 NewPos = pSimChar->GetPos();
			if(distance(Pos, NewPos) < 1.0f)
				++IdleTicks;
			if(BlockedAhead && Input.m_Direction != 0 && !Input.m_Jump && !Input.m_Hook)
			{
				FullSafe = false;
				break;
			}
			if(TouchesHazard(Collision(), NewPos))
			{
				FullSafe = false;
				break;
			}

			if((int)vFrames.size() < Commit)
				vFrames.push_back({Input, vec2((float)Input.m_TargetX, (float)Input.m_TargetY), NewPos});
		}

		if(vFrames.empty())
			continue;

		const vec2 EndPos = vFrames.back().m_Pos;
		const float EndSpeed = pSimChar != nullptr && pSimChar->Core() != nullptr ? length(pSimChar->Core()->m_Vel) : 0.0f;
		const float Score = EvaluateSafetyScore(StartPos, EndPos, GoalPos, EndSpeed, FullSafe, HookTicks, DirectionSwitches, IdleTicks);
		if((FullSafe || (int)vFrames.size() >= maximum(2, Commit / 2)) && Score > BestScore)
		{
			BestScore = Score;
			vBestFrames = vFrames;
		}
	}

	if(vBestFrames.empty())
		return false;

	m_vPlan = std::move(vBestFrames);
	m_ReplanCountdown = maximum(1, minimum(Commit, (int)m_vPlan.size()));
	return true;
}

bool CPastaPilot::GetRealtimeInput(CNetObj_PlayerInput &OutInput, vec2 &OutMouse) const
{
	if(!m_Running || !m_HasRealtimeInput)
		return false;
	OutInput = m_RealtimeInput;
	OutMouse = m_RealtimeMouse;
	return true;
}

void CPastaPilot::OnUpdate()
{
	const bool ShouldRun = Client()->State() == IClient::STATE_ONLINE && g_Config.m_PastaAvoidMode == 4 && g_Config.m_PastaAvoidfreeze != 0;
	if(ShouldRun && !m_Running)
		Start();
	else if(!ShouldRun && m_Running)
	{
		Stop();
		return;
	}

	if(!m_Running)
		return;

	const int LocalId = GameClient()->m_aLocalIds[g_Config.m_ClDummy];
	CCharacter *pPredChar = LocalId >= 0 ? GameClient()->m_PredictedWorld.GetCharacterById(LocalId) : nullptr;
	if(pPredChar)
	{
		const vec2 CurPos = pPredChar->GetPos();
		if(distance(CurPos, m_RuntimeLastPos) < 1.5f)
			++m_RuntimeStuckTicks;
		else
			m_RuntimeStuckTicks = 0;
		m_RuntimeLastPos = CurPos;
	}

	const int64_t Now = time_get();
	if(m_LastUpdateTime == 0)
		m_LastUpdateTime = Now;
	const double Delta = std::clamp((double)(Now - m_LastUpdateTime) / (double)time_freq(), 0.0, 0.1);
	m_LastUpdateTime = Now;
	m_Accumulator += Delta * 50.0;

	if((m_vPlan.empty() || m_ExecCursor >= (int)m_vPlan.size() || m_ReplanCountdown <= 0) && !BuildPlan())
	{
		m_HasRealtimeInput = false;
		return;
	}

	if(m_Accumulator < 1.0)
		return;

	m_Accumulator = minimum(m_Accumulator - 1.0, 1.0);
	if(m_vPlan.empty() || m_ExecCursor >= (int)m_vPlan.size())
	{
		m_HasRealtimeInput = false;
		return;
	}

	const SFrame &Frame = m_vPlan[m_ExecCursor++];
	m_RealtimeInput = Frame.m_Input;
	m_RealtimeMouse = Frame.m_Mouse;

	if(m_RuntimeHookHoldTicks > 0)
	{
		m_RealtimeInput.m_Hook = 1;
		m_RealtimeInput.m_TargetX = (int)m_RuntimeHookAim.x;
		m_RealtimeInput.m_TargetY = (int)m_RuntimeHookAim.y;
		m_RealtimeMouse = m_RuntimeHookAim;
		--m_RuntimeHookHoldTicks;
		if(m_RuntimeHookHoldTicks == 0)
			m_RuntimeHookCooldownTicks = 20;
	}
	else if(m_RuntimeHookCooldownTicks > 0)
	{
		m_RealtimeInput.m_Hook = 0;
		--m_RuntimeHookCooldownTicks;
	}
	else if(Frame.m_Input.m_Hook)
	{
		m_RuntimeHookAim = Frame.m_Mouse;
		m_RuntimeHookHoldTicks = 24;
		m_RealtimeInput.m_Hook = 1;
		m_RealtimeInput.m_TargetX = (int)m_RuntimeHookAim.x;
		m_RealtimeInput.m_TargetY = (int)m_RuntimeHookAim.y;
		m_RealtimeMouse = m_RuntimeHookAim;
	}

	if(pPredChar)
	{
		const bool BlockedAhead = m_RealtimeInput.m_Direction != 0 && Collision()->TestBox(pPredChar->GetPos() + vec2((float)m_RealtimeInput.m_Direction * 18.0f, 0.0f), vec2(28.0f, 28.0f));
		if(BlockedAhead && m_RuntimeStuckTicks >= 3)
		{
			if(pPredChar->IsGrounded())
				m_RealtimeInput.m_Jump = 1;
			else
				m_RealtimeInput.m_Direction = 0;
			if(m_RuntimeHookHoldTicks == 0)
				m_ReplanCountdown = 0;
		}
	}

	m_HasRealtimeInput = true;
	m_RuntimeLastDir = m_RealtimeInput.m_Direction;
	if(m_ReplanCountdown > 0)
		--m_ReplanCountdown;
}
