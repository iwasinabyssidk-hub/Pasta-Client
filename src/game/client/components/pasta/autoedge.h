#ifndef GAME_CLIENT_COMPONENTS_PASTA_AUTOEDGE_H
#define GAME_CLIENT_COMPONENTS_PASTA_AUTOEDGE_H

#include <base/vmath.h>

#include <engine/console.h>
#include <generated/protocol.h>
#include <game/client/component.h>
#include <game/client/prediction/entities/character.h>
#include <game/client/prediction/gameworld.h>
#include <engine/client/enums.h>

class CAutoEdge : public CComponent
{
public:
	virtual int Sizeof() const override { return sizeof(*this); }
	virtual void OnReset();

	bool HasSupportTile(int TileX, float FootY);
	bool HasHazardOrVoidBeyondPlatform(int TileX, int Direction, float FootY);
	void UpdateAutoEdgeState(int Dummy, CCharacter *pLocalCharacter);

	int m_FoundCount[NUM_DUMMIES];
	bool m_Locked[NUM_DUMMIES];
	vec2 m_LockedPos[NUM_DUMMIES];
	vec2 m_FoundPos[NUM_DUMMIES][16];
};

#endif