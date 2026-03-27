#ifndef GAME_CLIENT_COMPONENTS_PASTA_AIMBOT_H
#define GAME_CLIENT_COMPONENTS_PASTA_AIMBOT_H

#include <base/vmath.h>
#include <engine/console.h>
#include <generated/protocol.h>
#include <game/client/component.h>
#include <engine/client/enums.h>
#include <game/client/prediction/entities/character.h>

class CAimbot : public CComponent
{
public:
	struct SPastaAimbotTarget
	{
		int m_ClientId = -1;
		vec2 m_AimPos = vec2(0.0f, 0.0f);
		vec2 m_WorldPos = vec2(0.0f, 0.0f);
		float m_Score = 1e9f;
		bool m_Visible = false;
	};

	enum class AimbotSlot
	{
		HOOK = -1,
		HAMMER = WEAPON_HAMMER,
		GUN = WEAPON_GUN,
		SHOTGUN = WEAPON_SHOTGUN,
		GRENADE = WEAPON_GRENADE,
		LASER = WEAPON_LASER,
	};

	virtual int Sizeof() const override { return sizeof(*this); }
	virtual void OnReset();

	void PulseFire(CNetObj_PlayerInput &Input);
	int FindClosestTargetClientId(const CCharacter *pLocalCharacter);
	vec2 GetAimToClient(const CCharacter *pLocalCharacter, int ClientId, vec2 Fallback);
	vec2 BuildFakeAimPos(int Dummy, vec2 RealPos, CCharacter *pLocalCharacter, int64_t Now);
	float GetFov(int Slot);
	int GetPriority(int Slot);
	bool GetSilent(int Slot);
	bool GetIgnoreFriends(int Slot);
	bool GetEnabled(int Slot);
	vec2 NormalizePastaAim(vec2 Pos);
	float PastaAngleDiff(vec2 From, vec2 To);
	bool IsPastaTargetAllowed(const CCharacter *pLocalCharacter, int ClientId, int Slot);
	bool HasPastaDirectLineOfSight(vec2 From, vec2 To);
	vec2 PredictPastaWeaponTarget(const CCharacter *pLocalCharacter, vec2 TargetPos, vec2 TargetVel, int Slot);
	bool PastaPredictHook(vec2 MyPos, vec2 MyVel, vec2 &TargetPos, vec2 TargetVel);
	bool PastaIntersectCharacter(vec2 HookPos, vec2 TargetPos, vec2 &NewPos);
	bool PastaHookLineHits(vec2 InitPos, vec2 TargetPos, vec2 ScanDir);
	vec2 PastaHookEdgeScan(const CCharacter *pLocalCharacter, vec2 TargetPos, vec2 TargetVel);
	bool FindTarget(const CCharacter *pLocalCharacter, int Slot, vec2 RealAimPos, SPastaAimbotTarget *pOutTarget);

	int m_WeaponTargetId[NUM_DUMMIES];
	int m_HookTargetId[NUM_DUMMIES];
	vec2 m_WeaponAimPos[NUM_DUMMIES];
	vec2 m_HookAimPos[NUM_DUMMIES];
	vec2 m_WeaponWorldPos[NUM_DUMMIES];
	vec2 m_HookWorldPos[NUM_DUMMIES];
	int64_t m_NextAutoShoot[NUM_DUMMIES];
	int m_ScheduledTargetId[NUM_DUMMIES];
};

#endif