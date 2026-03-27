#include "laserunfreeze.h"

#include <algorithm>
#include <base/system.h>
#include <base/vmath.h>
#include <game/client/gameclient.h>
#include <vector>

void CLaserUnfreeze::OnReset()
{
	std::fill(std::begin(m_LastSelfHitFire), std::end(m_LastSelfHitFire), 0);
	std::fill(std::begin(m_PendingSelfHitFireRelease), std::end(m_PendingSelfHitFireRelease), false);
}

float CLaserUnfreeze::DistancePointToSegment(vec2 Point, vec2 From, vec2 To)
{
	const vec2 Delta = To - From;
	const float DeltaLenSq = length_squared(Delta);
	if(DeltaLenSq <= 0.0001f)
		return distance(Point, From);
	const float T = std::clamp(dot(Point - From, Delta) / DeltaLenSq, 0.0f, 1.0f);
	return distance(Point, From + Delta * T);
}

bool CLaserUnfreeze::EvaluateSelfHitLine(vec2 StartPos, vec2 Direction, float Reach, const CTuningParams *pTuning, vec2 TargetPos, int DesiredBounces, bool PreferMostBounces, float VelocityPreference, float *pOutScore, int *pOutBounces, int *pOutHitSegment)
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
		const int Res = GameClient()->Collision()->IntersectLineTeleWeapon(Pos, To, &Coltile, &BeforeCollision);
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
			OriginalTile = GameClient()->Collision()->GetTile(round_to_int(Coltile.x), round_to_int(Coltile.y));
			GameClient()->Collision()->SetCollisionAt(round_to_int(Coltile.x), round_to_int(Coltile.y), TILE_SOLID);
		}
		GameClient()->Collision()->MovePoint(&TempPos, &TempDir, 1.0f, nullptr);
		if(Res == -1)
			GameClient()->Collision()->SetCollisionAt(round_to_int(Coltile.x), round_to_int(Coltile.y), OriginalTile);

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
		const vec2 VelocityPrefDir = GameClient()->m_Controls.EnsureValidAim(TargetPos - StartPos);
		const float VelocityAlignment = dot(normalize(VelocityPrefDir), -LastSegmentDir);
		if(VelocityPreference > 0.0f && VelocityAlignment < 0.15f)
			return false;
		*pOutScore = BestDistance + BouncePenalty + BounceBias - absolute(VelocityPreference) * VelocityAlignment * 18.0f;
	}
	return true;
}

vec2 CLaserUnfreeze::BuildFutureSelfHitTarget(const CCharacter *pLocalCharacter, int FutureTick, bool LiftForUnfreeze)
{
	const vec2 StartPos = pLocalCharacter->GetPos();
	const vec2 Vel = pLocalCharacter->Core()->m_Vel;
	vec2 TargetPos = StartPos + Vel * (float)FutureTick;
	if(LiftForUnfreeze)
		TargetPos.y -= CCharacterCore::PhysicalSizeVec2().y * 0.35f;
	return TargetPos;
}

bool CLaserUnfreeze::FindBestSelfHitAim(CCharacter *pLocalCharacter, vec2 RealAimPos, int Weapon, int ExtraLookTicks, vec2 *pOutAimPos, float *pOutTimingError = nullptr)
{
	if(pLocalCharacter == nullptr || pOutAimPos == nullptr)
		return false;
	if(Weapon != WEAPON_LASER && Weapon != WEAPON_SHOTGUN)
		return false;
	if(!pLocalCharacter->GetWeaponGot(Weapon))
		return false;
	if((Weapon == WEAPON_LASER && pLocalCharacter->LaserHitDisabled()) || (Weapon == WEAPON_SHOTGUN && pLocalCharacter->ShotgunHitDisabled()))
		return false;

	const CTuningParams *pTuning = GameClient()->GetTuning(pLocalCharacter->GetOverriddenTuneZone());
	if(pTuning == nullptr)
		return false;

	const vec2 RealAim = normalize(GameClient()->m_Controls.EnsureValidAim(RealAimPos));
	const vec2 StartPos = pLocalCharacter->GetPos();
	const int LookTicks = maximum(2, ExtraLookTicks);
	const float Fov = (Weapon == WEAPON_LASER ? g_Config.m_PastaUnfreezeBotFov : g_Config.m_PastaAutoShotgunFov) * pi / 180.0f;
	const int Samples = maximum(10, Weapon == WEAPON_LASER ? g_Config.m_PastaUnfreezeBotPoints : g_Config.m_PastaAutoShotgunPoints);
	const bool PreferMostBounces = Weapon == WEAPON_LASER ? g_Config.m_PastaUnfreezeBotMostBounces != 0 : g_Config.m_PastaAutoShotgunMostBounces != 0;
	const float VelocityPreference = Weapon == WEAPON_SHOTGUN ? (g_Config.m_PastaAutoShotgunHighestVel ? 1.0f : -1.0f) : 0.0f;
	const float BounceDelayTicks = pTuning->m_LaserBounceDelay * GameClient()->Client()->GameTickSpeed() / 1000.0f;
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
			if(!EvaluateSelfHitLine(StartPos, Dir, pTuning->m_LaserReach, pTuning, TargetPos, DesiredBounces, PreferMostBounces, VelocityPreference, &Score, &Bounces, &HitSegment))
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

bool CLaserUnfreeze::IsWeaponReadyForSelfHit(CCharacter *pLocalCharacter, int Weapon)
{
	if(GameClient() == nullptr || pLocalCharacter == nullptr)
		return false;

	const CTuningParams *pTuning = GameClient()->GetTuning(pLocalCharacter->GetOverriddenTuneZone());
	if(pTuning == nullptr)
		return false;

	const int FireDelayTicks = maximum(1, round_to_int(pTuning->GetWeaponFireDelay(Weapon) * GameClient()->Client()->GameTickSpeed()));
	return GameClient()->Client()->PredGameTick(g_Config.m_ClDummy) - pLocalCharacter->GetAttackTick() >= FireDelayTicks;
}