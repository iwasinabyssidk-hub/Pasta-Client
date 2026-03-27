#include "aimbot.h"

#include <base/vmath.h>
#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>

void CAimbot::OnReset()
{
	std::fill(std::begin(m_WeaponTargetId), std::end(m_WeaponTargetId), -1);
	std::fill(std::begin(m_HookTargetId), std::end(m_HookTargetId), -1);
	std::fill(std::begin(m_WeaponAimPos), std::end(m_WeaponAimPos), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_HookAimPos), std::end(m_HookAimPos), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_WeaponWorldPos), std::end(m_WeaponWorldPos), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_HookWorldPos), std::end(m_HookWorldPos), vec2(0.0f, 0.0f));
	std::fill(std::begin(m_NextAutoShoot), std::end(m_NextAutoShoot), 0);
	std::fill(std::begin(m_ScheduledTargetId), std::end(m_ScheduledTargetId), -1);
}

void CAimbot::PulseFire(CNetObj_PlayerInput &Input)
{
	Input.m_Fire = (Input.m_Fire + 1) & INPUT_STATE_MASK;
}

int CAimbot::FindClosestTargetClientId(const CCharacter *pLocalCharacter)
{
	if(pLocalCharacter == nullptr)
		return -1;

	int ClosestClientId = -1;
	float ClosestDistance = 1e9f;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		if(ClientId == GameClient()->m_Snap.m_LocalClientId || !GameClient()->m_Snap.m_aCharacters[ClientId].m_Active)
			continue;

		const CCharacter *pTarget = GameClient()->m_PredictedWorld.GetCharacterById(ClientId);
		const vec2 TargetPos = pTarget != nullptr ? pTarget->GetPos() : GameClient()->m_aClients[ClientId].m_RenderPos;
		const float Distance = distance(pLocalCharacter->GetPos(), TargetPos);
		if(Distance < ClosestDistance)
		{
			ClosestDistance = Distance;
			ClosestClientId = ClientId;
		}
	}

	return ClosestClientId;
}

vec2 CAimbot::GetAimToClient(const CCharacter *pLocalCharacter, int ClientId, vec2 Fallback)
{
	if(pLocalCharacter == nullptr || ClientId < 0 || ClientId >= MAX_CLIENTS || !GameClient()->m_Snap.m_aCharacters[ClientId].m_Active)
		return GameClient()->m_Controls.EnsureValidAim(Fallback);

	const CCharacter *pTarget = GameClient()->m_PredictedWorld.GetCharacterById(ClientId);
	const vec2 TargetPos = pTarget != nullptr ? pTarget->GetPos() : GameClient()->m_aClients[ClientId].m_RenderPos;
	return GameClient()->m_Controls.EnsureValidAim(TargetPos - pLocalCharacter->GetPos());
}

vec2 CAimbot::BuildFakeAimPos(int Dummy, vec2 RealPos, CCharacter *pLocalCharacter, int64_t Now)
{
	const float Radius = maximum(96.0f, length(RealPos));
	const float AngleSpeed = 0.9f + g_Config.m_PastaFakeAimSpeed * 0.22f;

	switch(g_Config.m_PastaFakeAimMode)
	{
	case 0:
		return GameClient()->m_Controls.EnsureValidAim(RealPos);
	case 1:
		return GameClient()->m_Controls.EnsureValidAim(RealPos);
	case 2:
	{
		const float Angle = GameClient()->Client()->LocalTime() * AngleSpeed * 2.0f * pi;
		return vec2(std::cos(Angle), std::sin(Angle)) * Radius;
	}
	case 3:
	{
		const int64_t Interval = maximum<int64_t>(1, time_freq() / maximum(2, g_Config.m_PastaFakeAimSpeed * 5));
		if(Now > GameClient()->m_Controls.m_aPastaFakeAimLastRandomUpdate[Dummy] + Interval)
		{
			GameClient()->m_Controls.m_aPastaFakeAimLastRandomUpdate[Dummy] = Now;
			GameClient()->m_Controls.m_aPastaFakeAimAngle[Dummy] = (rand() / (float)RAND_MAX) * 2.0f * pi;
		}
		const float Angle = GameClient()->m_Controls.m_aPastaFakeAimAngle[Dummy];
		return vec2(std::cos(Angle), std::sin(Angle)) * Radius;
	}
	case 4:
		return GameClient()->m_Controls.EnsureValidAim(-RealPos);
	case 5:
		return GetAimToClient(pLocalCharacter, FindClosestTargetClientId(pLocalCharacter), -RealPos);
	default:
		return GameClient()->m_Controls.EnsureValidAim(RealPos);
	}
}

float CAimbot::GetFov(int SlotId)
{
	CAimbot::AimbotSlot Slot = static_cast<CAimbot::AimbotSlot>(SlotId);
	switch(Slot)
	{
	case CAimbot::AimbotSlot::HOOK: return (float)g_Config.m_PastaAimbotHookFov;
	case CAimbot::AimbotSlot::HAMMER: return (float)g_Config.m_PastaAimbotHammerFov;
	case CAimbot::AimbotSlot::GUN: return (float)g_Config.m_PastaAimbotPistolFov;
	case CAimbot::AimbotSlot::SHOTGUN: return (float)g_Config.m_PastaAimbotShotgunFov;
	case CAimbot::AimbotSlot::GRENADE: return (float)g_Config.m_PastaAimbotGrenadeFov;
	case CAimbot::AimbotSlot::LASER: return (float)g_Config.m_PastaAimbotLaserFov;
	default: return 90.0f;
	}
}

int CAimbot::GetPriority(int SlotId)
{
	CAimbot::AimbotSlot Slot = static_cast<CAimbot::AimbotSlot>(SlotId);
	switch(Slot)
	{
	case CAimbot::AimbotSlot::HOOK: return g_Config.m_PastaAimbotHookTargetPriority;
	case CAimbot::AimbotSlot::HAMMER: return g_Config.m_PastaAimbotHammerTargetPriority;
	case CAimbot::AimbotSlot::GUN: return g_Config.m_PastaAimbotPistolTargetPriority;
	case CAimbot::AimbotSlot::SHOTGUN: return g_Config.m_PastaAimbotShotgunTargetPriority;
	case CAimbot::AimbotSlot::GRENADE: return g_Config.m_PastaAimbotGrenadeTargetPriority;
	case CAimbot::AimbotSlot::LASER: return g_Config.m_PastaAimbotLaserTargetPriority;
	default: return g_Config.m_PastaAimbotTargetPriority;
	}
}

bool CAimbot::GetSilent(int SlotId)
{
	CAimbot::AimbotSlot Slot = static_cast<CAimbot::AimbotSlot>(SlotId);
	switch(Slot)
	{
	case CAimbot::AimbotSlot::HOOK: return g_Config.m_PastaAimbotHookSilent != 0;
	case CAimbot::AimbotSlot::HAMMER: return g_Config.m_PastaAimbotHammerSilent != 0;
	case CAimbot::AimbotSlot::GUN: return g_Config.m_PastaAimbotPistolSilent != 0;
	case CAimbot::AimbotSlot::SHOTGUN: return g_Config.m_PastaAimbotShotgunSilent != 0;
	case CAimbot::AimbotSlot::GRENADE: return g_Config.m_PastaAimbotGrenadeSilent != 0;
	case CAimbot::AimbotSlot::LASER: return g_Config.m_PastaAimbotLaserSilent != 0;
	default: return false;
	}
}

bool CAimbot::GetIgnoreFriends(int SlotId)
{
	CAimbot::AimbotSlot Slot = static_cast<CAimbot::AimbotSlot>(SlotId);
	switch(Slot)
	{
	case CAimbot::AimbotSlot::HOOK: return g_Config.m_PastaAimbotHookIgnoreFriends != 0;
	case CAimbot::AimbotSlot::HAMMER: return g_Config.m_PastaAimbotHammerIgnoreFriends != 0;
	case CAimbot::AimbotSlot::GUN: return g_Config.m_PastaAimbotPistolIgnoreFriends != 0;
	case CAimbot::AimbotSlot::SHOTGUN: return g_Config.m_PastaAimbotShotgunIgnoreFriends != 0;
	case CAimbot::AimbotSlot::GRENADE: return g_Config.m_PastaAimbotGrenadeIgnoreFriends != 0;
	case CAimbot::AimbotSlot::LASER: return g_Config.m_PastaAimbotLaserIgnoreFriends != 0;
	default: return false;
	}
}

bool CAimbot::GetEnabled(int SlotId)
{
	CAimbot::AimbotSlot Slot = static_cast<CAimbot::AimbotSlot>(SlotId);
	switch(Slot)
	{
	case CAimbot::AimbotSlot::HOOK: return g_Config.m_PastaAimbotHook != 0;
	case CAimbot::AimbotSlot::HAMMER: return g_Config.m_PastaAimbotHammer != 0;
	case CAimbot::AimbotSlot::GUN: return g_Config.m_PastaAimbotPistol != 0;
	case CAimbot::AimbotSlot::SHOTGUN: return g_Config.m_PastaAimbotShotgun != 0;
	case CAimbot::AimbotSlot::GRENADE: return g_Config.m_PastaAimbotGrenade != 0;
	case CAimbot::AimbotSlot::LASER: return g_Config.m_PastaAimbotLaser != 0;
	default: return false;
	}
}

vec2 CAimbot::NormalizePastaAim(vec2 Pos)
{
	constexpr float CameraMaxDistance = 200.0f;
	const float FollowFactor = (g_Config.m_ClDyncam ? g_Config.m_ClDyncamFollowFactor : g_Config.m_ClMouseFollowfactor) / 100.0f;
	const float DeadZone = g_Config.m_ClDyncam ? g_Config.m_ClDyncamDeadzone : g_Config.m_ClMouseDeadzone;
	const float MaxDistance = g_Config.m_ClMouseMaxDistance;
	const float MouseMax = minimum((FollowFactor != 0.0f ? CameraMaxDistance / FollowFactor + DeadZone : MaxDistance), MaxDistance);
	const float AimDistance = length(Pos);
	return normalize_pre_length(GameClient()->m_Controls.EnsureValidAim(Pos), AimDistance) * MouseMax;
}

float CAimbot::PastaAngleDiff(vec2 From, vec2 To)
{
	return absolute(atan2(std::sin(angle(GameClient()->m_Controls.EnsureValidAim(From)) - angle(GameClient()->m_Controls.EnsureValidAim(To))),
		       std::cos(angle(GameClient()->m_Controls.EnsureValidAim(From)) - angle(GameClient()->m_Controls.EnsureValidAim(To))))) *
	       180.0f / pi;
}

bool CAimbot::IsPastaTargetAllowed(const CCharacter *pLocalCharacter, int ClientId, int Slot)
{
	if(pLocalCharacter == nullptr || ClientId < 0 || ClientId >= MAX_CLIENTS || ClientId == GameClient()->m_Snap.m_LocalClientId)
		return false;
	if(!GameClient()->m_Snap.m_aCharacters[ClientId].m_Active)
		return false;
	if(GetIgnoreFriends(Slot))
	{
		const bool FriendByFlag = GameClient()->m_aClients[ClientId].m_Friend;
		const bool FriendByNameClan = GameClient()->Friends()->IsFriend(GameClient()->m_aClients[ClientId].m_aName, GameClient()->m_aClients[ClientId].m_aClan, true);
		const bool FriendByName = GameClient()->Friends()->IsFriend(GameClient()->m_aClients[ClientId].m_aName, "", false);
		if(FriendByFlag || FriendByNameClan || FriendByName)
			return false;
	}
	return true;
}

bool CAimbot::HasPastaDirectLineOfSight(vec2 From, vec2 To)
{
	vec2 Col;
	vec2 NewPos;
	return !GameClient()->Collision()->IntersectLine(From, To, &Col, &NewPos);
}

vec2 CAimbot::PredictPastaWeaponTarget(const CCharacter *pLocalCharacter, vec2 TargetPos, vec2 TargetVel, int SlotId)
{
	if(pLocalCharacter == nullptr)
		return TargetPos;

	const vec2 StartPos = pLocalCharacter->GetPos();
	const float Distance = distance(StartPos, TargetPos);
	CAimbot::AimbotSlot Slot = static_cast<CAimbot::AimbotSlot>(SlotId);
	switch(Slot)
	{
	case CAimbot::AimbotSlot::GUN:
		TargetPos += TargetVel * (Distance / maximum(1.0f, (float)GameClient()->m_aTuning[g_Config.m_ClDummy].m_GunSpeed));
		break;
	case CAimbot::AimbotSlot::SHOTGUN:
		TargetPos += TargetVel * (Distance / maximum(1.0f, (float)GameClient()->m_aTuning[g_Config.m_ClDummy].m_ShotgunSpeed));
		break;
	case CAimbot::AimbotSlot::GRENADE:
		TargetPos += TargetVel * (Distance / maximum(1.0f, (float)GameClient()->m_aTuning[g_Config.m_ClDummy].m_GrenadeSpeed));
		break;
	case CAimbot::AimbotSlot::LASER:
		TargetPos += TargetVel * 0.18f;
		break;
	default:
		break;
	}
	return TargetPos;
}

bool CAimbot::PastaPredictHook(vec2 MyPos, vec2 MyVel, vec2 &TargetPos, vec2 TargetVel)
{
	const vec2 Delta = TargetPos - MyPos;
	const vec2 DeltaVel = TargetVel - MyVel;
	const float HookSpeed = length(TargetVel) + GameClient()->m_aTuning[g_Config.m_ClDummy].m_HookFireSpeed;
	const float A = dot(DeltaVel, DeltaVel) - HookSpeed * HookSpeed;
	const float B = 2.0f * dot(DeltaVel, Delta);
	const float C = dot(Delta, Delta);
	const float Solution = B * B - 4.0f * A * C;
	if(Solution <= 0.0f)
		return false;

	const float Time = absolute(2.0f * C / (std::sqrt(Solution) - B)) + (float)GameClient()->Client()->GetPredictionTime() / 100.0f;
	TargetPos += TargetVel * Time;
	return true;
}

bool CAimbot::PastaIntersectCharacter(vec2 HookPos, vec2 TargetPos, vec2 &NewPos)
{
	vec2 ClosestPoint;
	if(closest_point_on_line(HookPos, NewPos, TargetPos, ClosestPoint) && distance(TargetPos, ClosestPoint) < CCharacterCore::PhysicalSize() + 2.0f)
	{
		NewPos = ClosestPoint;
		return true;
	}
	return false;
}

bool CAimbot::PastaHookLineHits(vec2 InitPos, vec2 TargetPos, vec2 ScanDir)
{
	vec2 ExDirection = normalize(ScanDir);
	vec2 FinishPos = InitPos + ExDirection * ((float)GameClient()->m_aTuning[g_Config.m_ClDummy].m_HookLength - CCharacterCore::PhysicalSize() * 1.5f);
	vec2 OldPos = InitPos + ExDirection * CCharacterCore::PhysicalSize() * 1.5f;
	vec2 NewPos = OldPos;
	bool DoBreak = false;

	do
	{
		OldPos = NewPos;
		NewPos = OldPos + ExDirection * GameClient()->m_aTuning[g_Config.m_ClDummy].m_HookFireSpeed;
		if(distance(InitPos, NewPos) > GameClient()->m_aTuning[g_Config.m_ClDummy].m_HookLength)
		{
			NewPos = InitPos + normalize(NewPos - InitPos) * GameClient()->m_aTuning[g_Config.m_ClDummy].m_HookLength;
			DoBreak = true;
		}

		int TeleNr = 0;
		const int Hit = GameClient()->Collision()->IntersectLineTeleHook(OldPos, NewPos, &FinishPos, nullptr, &TeleNr);
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

vec2 CAimbot::PastaHookEdgeScan(const CCharacter *pLocalCharacter, vec2 TargetPos, vec2 TargetVel)
{
	if(pLocalCharacter == nullptr)
		return vec2(0.0f, 0.0f);

	vec2 MyPos = pLocalCharacter->GetPos();
	vec2 MyVel = pLocalCharacter->Core()->m_Vel;
	if(!PastaPredictHook(MyPos, MyVel, TargetPos, TargetVel))
		return vec2(0.0f, 0.0f);
	if(PastaHookLineHits(MyPos, TargetPos, TargetPos - MyPos))
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
		if(PastaHookLineHits(MyPos, TargetPos, Dir))
			aHitPoints[HitPointsCount++] = Dir;
	}

	if(HitPointsCount <= 0)
		return vec2(0.0f, 0.0f);
	return aHitPoints[(HitPointsCount - 1) / 2];
}

bool CAimbot::FindTarget(const CCharacter *pLocalCharacter, int SlotId, vec2 RealAimPos, SPastaAimbotTarget *pOutTarget)
{
	CAimbot::AimbotSlot Slot = static_cast<CAimbot::AimbotSlot>(SlotId);
	if(GameClient() == nullptr || pLocalCharacter == nullptr || pOutTarget == nullptr || !GetEnabled(SlotId))
		return false;

	const vec2 LocalPos = pLocalCharacter->GetPos();
	const float Fov = GetFov(SlotId); // TODO: make it accept CAimbot::AimbotSlot
	const int Priority = GetPriority(SlotId);
	SPastaAimbotTarget BestTarget;

	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		if(!IsPastaTargetAllowed(pLocalCharacter, ClientId, SlotId))
			continue;

		CCharacter *pTarget = GameClient()->m_PredictedWorld.GetCharacterById(ClientId);
		if(pTarget == nullptr)
			continue;

		vec2 WorldPos = pTarget->GetPos();
		vec2 AimPos = WorldPos - LocalPos;
		if(Slot == CAimbot::AimbotSlot::HOOK)
			AimPos = PastaHookEdgeScan(pLocalCharacter, WorldPos, pTarget->Core()->m_Vel);
		else if(Slot == CAimbot::AimbotSlot::HAMMER)
		{
			if(distance(LocalPos, WorldPos) > pLocalCharacter->GetProximityRadius())
				continue;
			if(!HasPastaDirectLineOfSight(LocalPos, WorldPos))
				continue;
		}
		else
		{
			WorldPos = PredictPastaWeaponTarget(pLocalCharacter, WorldPos, pTarget->Core()->m_Vel, SlotId);
			if(!HasPastaDirectLineOfSight(LocalPos, WorldPos))
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