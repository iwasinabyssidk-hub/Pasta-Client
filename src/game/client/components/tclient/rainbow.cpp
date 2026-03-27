#include "rainbow.h"

#include <engine/graphics.h>
#include <engine/shared/config.h>

#include <game/client/animstate.h>
#include <game/client/gameclient.h>
#include <game/client/render.h>

template<typename T>
static T color_lerp(T a, T b, float c)
{
	T Result;
	for(size_t i = 0; i < 4; ++i)
		Result[i] = a[i] + c * (b[i] - a[i]);
	return Result;
}

void CRainbow::OnRender()
{
	const bool PastaRainbowTee = g_Config.m_PastaCustomColorTee != 0;
	const bool PastaRainbowHook = g_Config.m_PastaCustomColorHook != 0;
	const bool PastaRainbowWeapon = g_Config.m_PastaCustomColorWeapon != 0;
	if(!g_Config.m_TcRainbowTees && !g_Config.m_TcRainbowWeapon && !g_Config.m_TcRainbowHook && !PastaRainbowTee && !PastaRainbowHook && !PastaRainbowWeapon)
		return;

	const int RainbowMode = g_Config.m_TcRainbowMode > 0 ? g_Config.m_TcRainbowMode : COLORMODE_RAINBOW;
	if(RainbowMode == 0)
		return;

	const int RainbowSpeed = (g_Config.m_TcRainbowTees || g_Config.m_TcRainbowWeapon || g_Config.m_TcRainbowHook) ? g_Config.m_TcRainbowSpeed : g_Config.m_PastaCustomColorRainbowSpeed;
	m_Time += Client()->RenderFrameTime() * ((float)RainbowSpeed / 100.0f);
	float DefTick = std::fmod(m_Time, 1.0f);
	ColorRGBA Col;

	switch(RainbowMode)
	{
	case COLORMODE_RAINBOW:
		Col = color_cast<ColorRGBA>(ColorHSLA(DefTick, 1.0f, 0.5f));
		break;
	case COLORMODE_PULSE:
		Col = color_cast<ColorRGBA>(ColorHSLA(std::fmod(std::floor(m_Time) * 0.1f, 1.0f), 1.0f, 0.5f + std::fabs(DefTick - 0.5f)));
		break;
	case COLORMODE_DARKNESS:
		Col = ColorRGBA(0.0f, 0.0f, 0.0f);
		break;
	case COLORMODE_RANDOM:
		static ColorHSLA s_Col1 = ColorHSLA(0.0f, 0.0f, 0.0f, 0.0f), s_Col2 = ColorHSLA(0.0f, 0.0f, 0.0f, 0.0f);
		if(s_Col2.a == 0.0f) // Create first target
			s_Col2 = ColorHSLA((float)rand() / (float)RAND_MAX, 1.0f, (float)rand() / (float)RAND_MAX, 1.0f);
		static float s_LastSwap = -INFINITY;
		if(m_Time - s_LastSwap > 1.0f) // Shift target to source, create new target
		{
			s_LastSwap = m_Time;
			s_Col1 = s_Col2;
			s_Col2 = ColorHSLA((float)rand() / (float)RAND_MAX, 1.0f, (float)rand() / (float)RAND_MAX, 1.0f);
		}
		Col = color_cast<ColorRGBA>(color_lerp(s_Col1, s_Col2, DefTick));
		break;
	}

	m_RainbowColor = Col;

	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(!GameClient()->m_Snap.m_aCharacters[i].m_Active)
			continue;
		// check if local player
		bool Local = GameClient()->m_Snap.m_LocalClientId == i;
		CTeeRenderInfo *RenderInfo = &GameClient()->m_aClients[i].m_RenderInfo;

		// check if rainbow is enabled
		if(Local ? (g_Config.m_TcRainbowTees || PastaRainbowTee) : (g_Config.m_TcRainbowTees && g_Config.m_TcRainbowOthers))
		{
			RenderInfo->m_BloodColor = Col;
			RenderInfo->m_ColorBody = Col;
			RenderInfo->m_ColorFeet = Col;
			RenderInfo->m_CustomColoredSkin = true;
		}
	}
}
