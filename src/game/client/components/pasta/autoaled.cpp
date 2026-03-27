#include "autoaled.h"

#include <game/client/gameclient.h>

void CAutoAled::OnReset()
{
	std::fill(std::begin(m_LastAutoAled), std::end(m_LastAutoAled), 0);
	std::fill(std::begin(m_AutoAledLatched), std::end(m_AutoAledLatched), false);

	for (int i = 0; i < NUM_DUMMIES; i++)
	{
		m_LastAutoAled[i] = 0;
		m_AutoAledLatched[i] = false;
	}
}

bool CAutoAled::HasSingleFreezeGapBetween(vec2 From, vec2 To)
{
	const int Samples = 16;
	bool WasFreeze = false;
	bool SeenFreeze = false;
	int FreezeRuns = 0;

	for(int Index = 1; Index < Samples; ++Index)
	{
		const float T = Index / (float)Samples;
		const vec2 Pos = mix(From, To, T);
		const bool Freeze = GameClient()->m_AvoidFreeze.HasFreezeTile(Pos);
		if(Freeze && !WasFreeze)
		{
			++FreezeRuns;
			SeenFreeze = true;
		}
		WasFreeze = Freeze;
	}

	return SeenFreeze && FreezeRuns == 1;
}

bool CAutoAled::FindAutoAledTarget(CCharacter *pLocalCharacter, vec2 RealAimPos)
{
	if(pLocalCharacter == nullptr || pLocalCharacter->GetActiveWeapon() != WEAPON_HAMMER)
		return false;

	const vec2 LocalPos = pLocalCharacter->GetPos();
	const vec2 AimDir = normalize(GameClient()->m_Controls.EnsureValidAim(RealAimPos));
	const float Proximity = pLocalCharacter->GetProximityRadius();
	const vec2 ProjStartPos = LocalPos + AimDir * Proximity * 0.75f;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		if(ClientId == GameClient()->m_Snap.m_LocalClientId || !GameClient()->m_Snap.m_aCharacters[ClientId].m_Active)
			continue;

		CCharacter *pTarget = GameClient()->m_PredictedWorld.GetCharacterById(ClientId);
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
		const bool SingleFreezeGap = HasSingleFreezeGapBetween(StartCheck, EndCheck);
		if(SingleFreezeGap)
			return true;
	}

	return false;
}