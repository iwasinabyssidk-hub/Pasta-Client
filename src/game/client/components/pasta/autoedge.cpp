#include "autoedge.h"

#include <game/client/gameclient.h>

void CAutoEdge::OnReset()
{
	std::fill(std::begin(m_FoundCount), std::end(m_FoundCount), 0);
	std::fill(std::begin(m_Locked), std::end(m_Locked), false);
	std::fill(std::begin(m_LockedPos), std::end(m_LockedPos), vec2(0.0f, 0.0f));

	for(auto &aFoundPos : m_FoundPos)
		std::fill(std::begin(aFoundPos), std::end(aFoundPos), vec2(0.0f, 0.0f));
}

bool CAutoEdge::HasSupportTile(int TileX, float FootY)
{
	if(TileX < 0 || TileX >= GameClient()->Collision()->GetWidth())
		return false;
	return GameClient()->Collision()->CheckPoint((TileX + 0.5f) * 32, FootY);
}

bool CAutoEdge::HasHazardOrVoidBeyondPlatform(int TileX, int Direction, float FootY)
{	
	const int BeyondTileX = TileX + Direction;
	if(BeyondTileX < 0 || BeyondTileX >= GameClient()->Collision()->GetWidth())
		return true;
	if(HasSupportTile(BeyondTileX, FootY))
		return false;

	const vec2 CheckPos((BeyondTileX + 0.5f) * 32.0f, FootY + 6.0f);
	return GameClient()->m_AvoidFreeze.HasFreezeTile(CheckPos) || !GameClient()->Collision()->CheckPoint(CheckPos.x, CheckPos.y);
}

void CAutoEdge::UpdateAutoEdgeState(int Dummy, CCharacter *pLocalCharacter)
{
	m_FoundCount[Dummy] = 0;
	m_Locked[Dummy] = false;
	m_LockedPos[Dummy] = vec2(0.0f, 0.0f);

	if(!g_Config.m_PastaAutoEdge || pLocalCharacter == nullptr)
		return;

	const vec2 Pos = pLocalCharacter->GetPos();
	const float FootY = Pos.y + CCharacterCore::PhysicalSizeVec2().y * 0.5f + 4.0f;
	const float MarkerY = FootY - 8.0f;
	const int BaseTileX = (int)std::floor(Pos.x / 32.0f);
	constexpr int SearchOffsets[] = {0, -1, 1};
	const int MaxPlatformScan = maximum(1, GameClient()->Collision()->GetWidth());
	for(const int Offset : SearchOffsets)
	{
		const int TileX = BaseTileX + Offset;
		if(!HasSupportTile(TileX, FootY))
			continue;

		int LeftMost = TileX;
		int RightMost = TileX;
		int Safety = 0;
		while(Safety++ < MaxPlatformScan && HasSupportTile(LeftMost - 1, FootY))
			--LeftMost;
		Safety = 0;
		while(Safety++ < MaxPlatformScan && HasSupportTile(RightMost + 1, FootY))
			++RightMost;

		int FoundCount = 0;
		if(HasHazardOrVoidBeyondPlatform(LeftMost, -1, FootY))
			m_FoundPos[Dummy][FoundCount++] = vec2(LeftMost * 32.0f + 1.0f, MarkerY);
		if(HasHazardOrVoidBeyondPlatform(RightMost, 1, FootY))
			m_FoundPos[Dummy][FoundCount++] = vec2((RightMost + 1) * 32.0f - 1.0f, MarkerY);

		if(FoundCount <= 0)
			return;

		m_FoundCount[Dummy] = FoundCount;
		const float DistLeft = absolute(Pos.x - m_FoundPos[Dummy][0].x);
		if(FoundCount == 1)
			m_LockedPos[Dummy] = m_FoundPos[Dummy][0];
		else
		{
			const float DistRight = absolute(Pos.x - m_FoundPos[Dummy][1].x);
			m_LockedPos[Dummy] = DistLeft < DistRight ? m_FoundPos[Dummy][0] : m_FoundPos[Dummy][1];
		}
		m_Locked[Dummy] = true;
		return;
	}
}