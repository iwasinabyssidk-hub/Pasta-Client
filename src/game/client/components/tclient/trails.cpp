#include "trails.h"

#include <engine/graphics.h>
#include <engine/shared/config.h>

#include <generated/client_data.h>

#include <game/client/animstate.h>
#include <game/client/gameclient.h>
#include <game/client/render.h>

namespace
{
template<typename T>
T ClampValue(T Value, T Min, T Max)
{
	return minimum(Max, maximum(Min, Value));
}

struct STrailPixelGrid
{
	float m_ScreenX0;
	float m_ScreenY0;
	float m_PixelSizeX;
	float m_PixelSizeY;
};

STrailPixelGrid GetTrailPixelGrid(const IGraphics *pGraphics)
{
	float ScreenX0, ScreenY0, ScreenX1, ScreenY1;
	pGraphics->GetScreen(&ScreenX0, &ScreenY0, &ScreenX1, &ScreenY1);
	return {ScreenX0, ScreenY0,
		(ScreenX1 - ScreenX0) / maximum(1, pGraphics->ScreenWidth()),
		(ScreenY1 - ScreenY0) / maximum(1, pGraphics->ScreenHeight())};
}

vec2 SnapTrailPoint(const STrailPixelGrid &Grid, vec2 Pos)
{
	return vec2(
		Grid.m_ScreenX0 + roundf((Pos.x - Grid.m_ScreenX0) / Grid.m_PixelSizeX) * Grid.m_PixelSizeX,
		Grid.m_ScreenY0 + roundf((Pos.y - Grid.m_ScreenY0) / Grid.m_PixelSizeY) * Grid.m_PixelSizeY);
}
}

bool CTrails::ShouldPredictPlayer(int ClientId)
{
	if(!GameClient()->Predict())
		return false;
	CCharacter *pChar = GameClient()->m_PredictedWorld.GetCharacterById(ClientId);
	if(GameClient()->Predict() && (ClientId == GameClient()->m_Snap.m_LocalClientId || (GameClient()->AntiPingPlayers() && !GameClient()->IsOtherTeam(ClientId))) && pChar)
		return true;
	return false;
}

void CTrails::ClearAllHistory()
{
	for(int i = 0; i < MAX_CLIENTS; ++i)
		ClearHistory(i);
}
void CTrails::ClearHistory(int ClientId)
{
	for(int i = 0; i < 200; ++i)
		m_History[ClientId][i] = {{}, -1};
	m_HistoryValid[ClientId] = false;
}
void CTrails::OnReset()
{
	ClearAllHistory();
}

void CTrails::OnRender()
{
	const bool UseTcTrail = g_Config.m_TcTeeTrail != 0;
	const bool UsePastaTrail = g_Config.m_PastaTrail != 0;
	if(!UseTcTrail && !UsePastaTrail)
		return;

	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;

	if(!GameClient()->m_Snap.m_pGameInfoObj)
		return;

	Graphics()->TextureClear();

	for(int ClientId = 0; ClientId < MAX_CLIENTS; ClientId++)
	{
		const bool Local = GameClient()->m_Snap.m_LocalClientId == ClientId;

		const bool ZoomAllowed = GameClient()->m_Camera.ZoomAllowed();
		if((UseTcTrail && !g_Config.m_TcTeeTrailOthers && !Local) || (UsePastaTrail && !Local))
			continue;

		if(!Local && !ZoomAllowed)
			continue;

		if(!GameClient()->m_Snap.m_aCharacters[ClientId].m_Active)
		{
			if(m_HistoryValid[ClientId])
				ClearHistory(ClientId);
			continue;
		}
		else
			m_HistoryValid[ClientId] = true;

		CTeeRenderInfo TeeInfo = GameClient()->m_aClients[ClientId].m_RenderInfo;

		const bool PredictPlayer = ShouldPredictPlayer(ClientId) && !(UsePastaTrail && Local);
		int StartTick;
		const int GameTick = Client()->GameTick(g_Config.m_ClDummy);
		const int PredTick = Client()->PredGameTick(g_Config.m_ClDummy);
		float IntraTick;
		if(PredictPlayer)
		{
			StartTick = PredTick;
			IntraTick = Client()->PredIntraGameTick(g_Config.m_ClDummy);
			if(g_Config.m_TcRemoveAnti)
			{
				StartTick = GameClient()->m_SmoothTick;
				IntraTick = GameClient()->m_SmoothIntraTick;
			}
			if(g_Config.m_TcUnpredOthersInFreeze && !Local && Client()->m_IsLocalFrozen)
			{
				StartTick = GameTick;
			}
		}
		else
		{
			StartTick = GameTick;
			IntraTick = Client()->IntraGameTick(g_Config.m_ClDummy);
		}

		const vec2 CurServerPos = vec2(GameClient()->m_Snap.m_aCharacters[ClientId].m_Cur.m_X, GameClient()->m_Snap.m_aCharacters[ClientId].m_Cur.m_Y);
		const vec2 PrevServerPos = vec2(GameClient()->m_Snap.m_aCharacters[ClientId].m_Prev.m_X, GameClient()->m_Snap.m_aCharacters[ClientId].m_Prev.m_Y);
		const vec2 HistoryRenderPos = Local ? GameClient()->m_aClients[ClientId].m_RenderPos : mix(PrevServerPos, CurServerPos, IntraTick);
		m_History[ClientId][GameTick % 200] = {
			HistoryRenderPos,
			GameTick,
		};

		// // NOTE: this is kind of a hack to fix 25tps. This fixes flickering when using the speed mode
		// m_History[ClientId][(GameTick + 1) % 200] = m_History[ClientId][GameTick % 200];
		// m_History[ClientId][(GameTick + 2) % 200] = m_History[ClientId][GameTick % 200];

		IGraphics::CLineItem LineItem;
		const int TrailWidthConfig = UseTcTrail ? g_Config.m_TcTeeTrailWidth : g_Config.m_PastaTrailWidth;
		const int PastaTrailType = UsePastaTrail ? g_Config.m_PastaTrailType : 4;
		const bool SmokeTrail = UsePastaTrail && PastaTrailType == 0;
		const bool BulletTrail = UsePastaTrail && PastaTrailType == 1;
		const bool PowerupTrail = UsePastaTrail && PastaTrailType == 2;
		const bool SparkleTrail = UsePastaTrail && PastaTrailType == 3;
		const bool TaterTrail = !UsePastaTrail || PastaTrailType == 4;
		const bool SkipBaseTrail = BulletTrail;
		bool LineMode = (TrailWidthConfig == 0 || SparkleTrail) && !SkipBaseTrail;

		float Alpha = (UseTcTrail ? g_Config.m_TcTeeTrailAlpha : g_Config.m_PastaTrailAlpha) / 100.0f;
		// Taken from players.cpp
		if(ClientId == -2)
			Alpha *= g_Config.m_ClRaceGhostAlpha / 100.0f;
		else if(ClientId < 0 || GameClient()->IsOtherTeam(ClientId))
			Alpha *= g_Config.m_ClShowOthersAlpha / 100.0f;

		int TrailLength = UseTcTrail ? g_Config.m_TcTeeTrailLength : g_Config.m_PastaTrailLength;
		float Width = TrailWidthConfig;
		if(SmokeTrail)
		{
			Width *= 1.4f;
			Alpha *= 0.75f;
		}
		else if(BulletTrail)
		{
			Width = maximum(1.0f, Width * 0.35f);
			Alpha = ClampValue(Alpha * 1.15f, 0.0f, 1.0f);
		}
		else if(PowerupTrail)
		{
			Width *= 1.15f;
			Alpha = ClampValue(Alpha * 1.1f, 0.0f, 1.0f);
		}
		else if(SparkleTrail)
		{
			Width = maximum(1.0f, Width * 0.5f);
			Alpha = ClampValue(Alpha * 1.05f, 0.0f, 1.0f);
		}
		else if(TaterTrail)
		{
			Width *= 1.0f;
		}

		static std::vector<CTrailPart> s_Trail;
		s_Trail.clear();

		// TODO: figure out why this is required
		if(!PredictPlayer)
			TrailLength += 2;
		bool TrailFull = false;
		// Fill trail list with initial positions
		for(int i = 0; i < TrailLength; i++)
		{
			CTrailPart Part;
			int PosTick = StartTick - i;
			if(PredictPlayer)
			{
				if(GameClient()->m_aClients[ClientId].m_aPredTick[PosTick % 200] != PosTick)
					continue;
				Part.m_Pos = GameClient()->m_aClients[ClientId].m_aPredPos[PosTick % 200];
				if(i == TrailLength - 1)
					TrailFull = true;
			}
			else
			{
				if(m_History[ClientId][PosTick % 200].m_Tick != PosTick)
					continue;
				Part.m_Pos = m_History[ClientId][PosTick % 200].m_Pos;
				if(i == TrailLength - 2 || i == TrailLength - 3)
					TrailFull = true;
			}
			Part.m_UnmovedPos = Part.m_Pos;
			Part.m_Tick = PosTick;
			s_Trail.push_back(Part);
		}

		// Trim the ends if intratick is too big
		// this was not trivial to figure out
		int TrimTicks = (int)IntraTick;
		for(int i = 0; i < TrimTicks; i++)
			if((int)s_Trail.size() > 0)
				s_Trail.pop_back();

		// Stuff breaks if we have less than 3 points because we cannot calculate an angle between segments to preserve constant width
		// TODO: Pad the list with generated entries in the same direction as before
		if((int)s_Trail.size() < 3)
			continue;

		if(PredictPlayer)
			s_Trail.at(0).m_Pos = GameClient()->m_aClients[ClientId].m_RenderPos;
		else
			s_Trail.at(0).m_Pos = Local ? GameClient()->m_aClients[ClientId].m_RenderPos : mix(PrevServerPos, CurServerPos, IntraTick);

		if(TrailFull)
			s_Trail.at(s_Trail.size() - 1).m_Pos = mix(s_Trail.at(s_Trail.size() - 1).m_Pos, s_Trail.at(s_Trail.size() - 2).m_Pos, std::fmod(IntraTick, 1.0f));

		// Set progress
		for(int i = 0; i < (int)s_Trail.size(); i++)
		{
			float Size = float(s_Trail.size() - 1 + TrimTicks);
			CTrailPart &Part = s_Trail.at(i);
			if(i == 0)
				Part.m_Progress = 0.0f;
			else if(i == (int)s_Trail.size() - 1)
				Part.m_Progress = 1.0f;
			else
				Part.m_Progress = ((float)i + IntraTick - 1.0f) / (Size - 1.0f);

			const int TrailColorMode = UseTcTrail ? g_Config.m_TcTeeTrailColorMode : (g_Config.m_PastaTrailColorRainbow ? COLORMODE_RAINBOW : COLORMODE_SOLID);
			switch(TrailColorMode)
			{
			case COLORMODE_SOLID:
				Part.m_Col = color_cast<ColorRGBA>(ColorHSLA(UseTcTrail ? g_Config.m_TcTeeTrailColor : g_Config.m_PastaTrailColor));
				break;
			case COLORMODE_TEE:
				if(TeeInfo.m_CustomColoredSkin)
					Part.m_Col = TeeInfo.m_ColorBody;
				else
					Part.m_Col = TeeInfo.m_BloodColor;
				break;
			case COLORMODE_RAINBOW:
			{
				float Cycle = (1.0f / TrailLength) * 0.5f;
				float Hue = std::fmod(((Part.m_Tick + 6361 * ClientId) % 1000000) * Cycle, 1.0f);
				Part.m_Col = color_cast<ColorRGBA>(ColorHSLA(Hue, 1.0f, 0.5f));
				break;
			}
			case COLORMODE_SPEED:
			{
				float Speed = 0.0f;
				if(s_Trail.size() > 3)
				{
					if(i < 2)
						Speed = distance(s_Trail.at(i + 2).m_UnmovedPos, Part.m_UnmovedPos) / std::abs(s_Trail.at(i + 2).m_Tick - Part.m_Tick);
					else
						Speed = distance(Part.m_UnmovedPos, s_Trail.at(i - 2).m_UnmovedPos) / std::abs(Part.m_Tick - s_Trail.at(i - 2).m_Tick);
				}
				Part.m_Col = color_cast<ColorRGBA>(ColorHSLA(65280 * ((int)(Speed * Speed / 12.5f) + 1)).UnclampLighting(ColorHSLA::DARKEST_LGT));
				break;
			}
			default:
				dbg_assert(false, "Invalid value for g_Config.m_TcTeeTrailColorMode");
				dbg_break();
			}

			Part.m_Col.a = Alpha;
			if((UseTcTrail && g_Config.m_TcTeeTrailFade) || (UsePastaTrail && (g_Config.m_PastaTrailFade || SmokeTrail || SparkleTrail)))
				Part.m_Col.a *= 1.0 - Part.m_Progress;

			Part.m_Width = Width;
			if((UseTcTrail && g_Config.m_TcTeeTrailTaper) || (UsePastaTrail && (g_Config.m_PastaTrailLowTaper || SmokeTrail)))
				Part.m_Width = Width * (1.0 - Part.m_Progress);
			else if(PowerupTrail)
				Part.m_Width = Width * (1.15f - Part.m_Progress * 0.35f);
			else if(BulletTrail)
				Part.m_Width = maximum(1.0f, Width * (1.0f - Part.m_Progress * 0.55f));
		}

		// Remove duplicate elements (those with same Pos)
		auto NewEnd = std::unique(s_Trail.begin(), s_Trail.end());
		s_Trail.erase(NewEnd, s_Trail.end());

		if((int)s_Trail.size() < 3)
			continue;

		const STrailPixelGrid PixelGrid = GetTrailPixelGrid(Graphics());
		for(auto &Part : s_Trail)
		{
			Part.m_Pos = SnapTrailPoint(PixelGrid, Part.m_Pos);
			Part.m_Top = SnapTrailPoint(PixelGrid, Part.m_Top);
			Part.m_Bot = SnapTrailPoint(PixelGrid, Part.m_Bot);
		}

		// Calculate the widths
		for(int i = 0; i < (int)s_Trail.size(); i++)
		{
			CTrailPart &Part = s_Trail.at(i);
			vec2 PrevPos;
			vec2 Pos = s_Trail.at(i).m_Pos;
			vec2 NextPos;

			if(i == 0)
			{
				vec2 Direction = normalize(s_Trail.at(i + 1).m_Pos - Pos);
				PrevPos = Pos - Direction;
			}
			else
				PrevPos = s_Trail.at(i - 1).m_Pos;

			if(i == (int)s_Trail.size() - 1)
			{
				vec2 Direction = normalize(Pos - s_Trail.at(i - 1).m_Pos);
				NextPos = Pos + Direction;
			}
			else
				NextPos = s_Trail.at(i + 1).m_Pos;

			vec2 NextDirection = normalize(NextPos - Pos);
			vec2 PrevDirection = normalize(Pos - PrevPos);

			vec2 Normal = vec2(-PrevDirection.y, PrevDirection.x);
			Part.m_Normal = Normal;
			vec2 Tangent = normalize(NextDirection + PrevDirection);
			if(Tangent == vec2(0.0f, 0.0f))
				Tangent = Normal;

			vec2 PerpVec = vec2(-Tangent.y, Tangent.x);
			Width = Part.m_Width;
			float ScaledWidth = Width / dot(Normal, PerpVec);
			float TopScaled = ScaledWidth;
			float BotScaled = ScaledWidth;
			if(dot(PrevDirection, Tangent) > 0.0f)
				TopScaled = std::min(Width * 3.0f, TopScaled);
			else
				BotScaled = std::min(Width * 3.0f, BotScaled);

			vec2 Top = Pos + PerpVec * TopScaled;
			vec2 Bot = Pos - PerpVec * BotScaled;
			Part.m_Top = Top;
			Part.m_Bot = Bot;

			// Bevel Cap
			if(dot(PrevDirection, NextDirection) < -0.25f)
			{
				Top = Pos + Tangent * Width;
				Bot = Pos - Tangent * Width;

				float Det = PrevDirection.x * NextDirection.y - PrevDirection.y * NextDirection.x;
				if(Det >= 0.0f)
				{
					Part.m_Top = Top;
					Part.m_Bot = Bot;
					if(i > 0)
						s_Trail.at(i).m_Flip = true;
				}
				else // <-Left Direction
				{
					Part.m_Top = Bot;
					Part.m_Bot = Top;
					if(i > 0)
						s_Trail.at(i).m_Flip = true;
				}
			}
		}

		if(!SkipBaseTrail)
		{
			if(LineMode)
				Graphics()->LinesBegin();
			else
				Graphics()->QuadsBegin();

			// Draw the trail
			for(int i = 0; i < (int)s_Trail.size() - 1; i++)
			{
				const CTrailPart &Part = s_Trail.at(i);
				const CTrailPart &NextPart = s_Trail.at(i + 1);
				if(SparkleTrail && i % 2 != 0)
					continue;

				const float Dist = distance(Part.m_UnmovedPos, NextPart.m_UnmovedPos);

				const float MaxDiff = 120.0f;
				if(i > 0)
				{
					const CTrailPart &PrevPart = s_Trail.at(i - 1);
					float PrevDist = distance(PrevPart.m_UnmovedPos, Part.m_UnmovedPos);
					if(std::abs(Dist - PrevDist) > MaxDiff)
						continue;
				}
				if(i < (int)s_Trail.size() - 2)
				{
					const CTrailPart &NextNextPart = s_Trail.at(i + 2);
					float NextDist = distance(NextPart.m_UnmovedPos, NextNextPart.m_UnmovedPos);
					if(std::abs(Dist - NextDist) > MaxDiff)
						continue;
				}

				if(LineMode)
				{
					Graphics()->SetColor(Part.m_Col);
					LineItem = IGraphics::CLineItem(Part.m_Pos.x, Part.m_Pos.y, NextPart.m_Pos.x, NextPart.m_Pos.y);
					Graphics()->LinesDraw(&LineItem, 1);
				}
				else
				{
					vec2 Top, Bot;
					if(Part.m_Flip)
					{
						Top = Part.m_Bot;
						Bot = Part.m_Top;
					}
					else
					{
						Top = Part.m_Top;
						Bot = Part.m_Bot;
					}

					Graphics()->SetColor4(NextPart.m_Col, NextPart.m_Col, Part.m_Col, Part.m_Col);
					IGraphics::CFreeformItem FreeformItem(NextPart.m_Top, NextPart.m_Bot, Top, Bot);
					Graphics()->QuadsDrawFreeform(&FreeformItem, 1);
				}
			}
			if(LineMode)
				Graphics()->LinesEnd();
			else
				Graphics()->QuadsEnd();
		}

		if(UsePastaTrail && (SmokeTrail || BulletTrail || PowerupTrail || SparkleTrail))
		{
			IGraphics::CTextureHandle Texture;
			if(SmokeTrail)
				Texture = GameClient()->m_ParticlesSkin.m_aSpriteParticles[SPRITE_PART_SMOKE - SPRITE_PART_SLICE];
			else if(BulletTrail)
				Texture = GameClient()->m_GameSkin.m_aSpriteWeaponProjectiles[WEAPON_GUN];
			else if(PowerupTrail)
				Texture = GameClient()->m_ParticlesSkin.m_aSpriteParticles[SPRITE_PART_BALL - SPRITE_PART_SLICE];
			else
				Texture = GameClient()->m_ExtrasSkin.m_aSpriteParticles[SPRITE_PART_SPARKLE - SPRITE_PART_SNOWFLAKE];

			Graphics()->TextureSet(Texture);
			Graphics()->QuadsBegin();
			for(int i = 0; i < (int)s_Trail.size(); ++i)
			{
				const CTrailPart &Part = s_Trail.at(i);
				if((SmokeTrail && i % 2 != 0) || (PowerupTrail && i % 3 != 0) || (SparkleTrail && i % 2 != 0))
					continue;

				ColorRGBA StampColor = Part.m_Col;
				if(PowerupTrail)
				{
					StampColor.r = minimum(1.0f, StampColor.r * 1.1f + 0.2f);
					StampColor.g = minimum(1.0f, StampColor.g * 1.1f + 0.2f);
					StampColor.b = minimum(1.0f, StampColor.b * 1.1f + 0.2f);
					StampColor.a *= 0.9f;
				}
				else if(SparkleTrail)
				{
					StampColor.a *= 0.8f;
				}

				Graphics()->SetColor(StampColor);
				float Size = maximum(4.0f, Part.m_Width * (SmokeTrail ? 1.35f : (PowerupTrail ? 1.2f : (BulletTrail ? 1.6f : 0.9f))));
				if(BulletTrail)
				{
					vec2 Dir = i + 1 < (int)s_Trail.size() ? normalize(Part.m_Pos - s_Trail.at(i + 1).m_Pos) : vec2(1.0f, 0.0f);
					if(length(Dir) < 0.001f)
						Dir = vec2(1.0f, 0.0f);
					Graphics()->QuadsSetRotation(angle(Dir));
					Size = maximum(6.0f, Part.m_Width * 2.25f);
				}
				IGraphics::CQuadItem QuadItem(Part.m_Pos.x, Part.m_Pos.y, Size, Size);
				Graphics()->QuadsDraw(&QuadItem, 1);
			}
			Graphics()->QuadsSetRotation(0.0f);
			Graphics()->QuadsEnd();
		}
	}
}
