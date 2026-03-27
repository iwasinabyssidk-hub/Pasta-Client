#ifndef GAME_CLIENT_COMPONENTS_PASTA_AUTOALED_H
#define GAME_CLIENT_COMPONENTS_PASTA_AUTOALED_H

#include <base/vmath.h>
#include <engine/console.h>
#include <generated/protocol.h>
#include <game/client/component.h>
#include <engine/client/enums.h>
#include <game/client/prediction/entities/character.h>

class CAutoAled : public CComponent
{
public:
	virtual int Sizeof() const override { return sizeof(*this); }
	virtual void OnReset();

	bool HasSingleFreezeGapBetween(vec2 From, vec2 To);
	bool FindAutoAledTarget(CCharacter *pLocalCharacter, vec2 RealAimPos);

	int64_t m_LastAutoAled[NUM_DUMMIES];
	bool m_AutoAledLatched[NUM_DUMMIES];
};

#endif