#ifndef GAME_CLIENT_COMPONENTS_PASTA_LASERUNFREEZE_H
#define GAME_CLIENT_COMPONENTS_PASTA_LASERUNFREEZE_H

#include <base/vmath.h>

#include <engine/client/enums.h>
#include <engine/console.h>
#include <engine/shared/jobs.h>

#include <generated/protocol.h>

#include <game/client/component.h>
#include <game/client/prediction/entities/character.h>
#include <game/client/prediction/gameworld.h>

class CLaserUnfreeze : public CComponent
{
public:
	virtual int Sizeof() const override { return sizeof(*this); }
	virtual void OnReset();

	float DistancePointToSegment(vec2 Point, vec2 From, vec2 To);
	bool EvaluateSelfHitLine(vec2 StartPos, vec2 Direction, float Reach, const CTuningParams *pTuning, vec2 TargetPos, int DesiredBounces, bool PreferMostBounces, float VelocityPreference, float *pOutScore, int *pOutBounces, int *pOutHitSegment);
	vec2 BuildFutureSelfHitTarget(const CCharacter *pLocalCharacter, int FutureTick, bool LiftForUnfreeze);
	bool FindBestSelfHitAim(CCharacter *pLocalCharacter, vec2 RealAimPos, int Weapon, int ExtraLookTicks, vec2 *pOutAimPos, float *pOutTimingError);
	bool IsWeaponReadyForSelfHit(CCharacter *pLocalCharacter, int Weapon);

	int64_t m_LastSelfHitFire[NUM_DUMMIES];
	bool m_PendingSelfHitFireRelease[NUM_DUMMIES];
};

#endif