#include "pasta_tas.h"

#include <base/color.h>
#include <base/math.h>
#include <base/str.h>
#include <base/system.h>

#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/shared/protocol.h>

#include <generated/client_data.h>
#include <game/gamecore.h>
#include <game/client/animstate.h>
#include <game/client/components/chat.h>
#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>
#include <game/client/render.h>
#include <game/client/ui.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
constexpr const char *gs_pTasMagic = "PSTATAS2";

struct STasScreenPixelGrid
{
	float m_ScreenX0;
	float m_ScreenY0;
	float m_PixelSizeX;
	float m_PixelSizeY;
};

int GetTasStepAmount()
{
	return g_Config.m_PastaTickControlStep ? 1 : maximum(1, g_Config.m_PastaTasTickControlTicks);
}

STasScreenPixelGrid GetTasScreenPixelGrid(const IGraphics *pGraphics)
{
	float ScreenX0, ScreenY0, ScreenX1, ScreenY1;
	pGraphics->GetScreen(&ScreenX0, &ScreenY0, &ScreenX1, &ScreenY1);

	return {
		ScreenX0,
		ScreenY0,
		(ScreenX1 - ScreenX0) / maximum(1, pGraphics->ScreenWidth()),
		(ScreenY1 - ScreenY0) / maximum(1, pGraphics->ScreenHeight())};
}

vec2 SnapTasPoint(const STasScreenPixelGrid &Grid, vec2 Pos)
{
	return vec2(
		Grid.m_ScreenX0 + roundf((Pos.x - Grid.m_ScreenX0) / Grid.m_PixelSizeX) * Grid.m_PixelSizeX,
		Grid.m_ScreenY0 + roundf((Pos.y - Grid.m_ScreenY0) / Grid.m_PixelSizeY) * Grid.m_PixelSizeY);
}

float SnapTasSpan(float Span, float PixelSize)
{
	return maximum(PixelSize, roundf(Span / PixelSize) * PixelSize);
}

void RenderTasPoints(IGraphics *pGraphics, IGraphics::CTextureHandle Texture, const std::vector<vec2> &vPoints, ColorRGBA Color, float Size)
{
	if(vPoints.empty() || Color.a <= 0.0f)
		return;

	const STasScreenPixelGrid Grid = GetTasScreenPixelGrid(pGraphics);
	const float SizeX = SnapTasSpan(Size, Grid.m_PixelSizeX);
	const float SizeY = SnapTasSpan(Size, Grid.m_PixelSizeY);

	if(Texture.IsValid())
		pGraphics->TextureSet(Texture);
	else
		pGraphics->TextureClear();

	pGraphics->QuadsBegin();
	pGraphics->SetColor(Color);
	for(const vec2 &Point : vPoints)
	{
		const vec2 TopLeft = SnapTasPoint(Grid, Point - vec2(SizeX * 0.5f, SizeY * 0.5f));
		const IGraphics::CQuadItem Quad(TopLeft.x, TopLeft.y, SizeX, SizeY);
		pGraphics->QuadsDrawTL(&Quad, 1);
	}
	pGraphics->QuadsEnd();
}

void RenderTasLines(IGraphics *pGraphics, const std::vector<IGraphics::CLineItem> &vSegments, ColorRGBA Color, float Thickness, float Zoom)
{
	if(vSegments.empty() || Color.a <= 0.0f)
		return;

	const STasScreenPixelGrid Grid = GetTasScreenPixelGrid(pGraphics);
	const float PixelSize = maximum(Grid.m_PixelSizeX, Grid.m_PixelSizeY);
	const float LineWidth = SnapTasSpan(maximum(PixelSize, (0.8f + (Thickness - 1.0f) * 0.5f) * Zoom), PixelSize);

	pGraphics->TextureClear();
	std::vector<IGraphics::CFreeformItem> vQuads;
	vQuads.reserve(vSegments.size());
	for(const auto &Segment : vSegments)
	{
		const vec2 Start = SnapTasPoint(Grid, vec2(Segment.m_X0, Segment.m_Y0));
		const vec2 End = SnapTasPoint(Grid, vec2(Segment.m_X1, Segment.m_Y1));
		const vec2 Diff = End - Start;
		if(length(Diff) < 0.001f)
			continue;
		const vec2 Perp = normalize(vec2(Diff.y, -Diff.x)) * (LineWidth * 0.5f);
		vQuads.emplace_back(
			End.x - Perp.x, End.y - Perp.y,
			End.x + Perp.x, End.y + Perp.y,
			Start.x - Perp.x, Start.y - Perp.y,
			Start.x + Perp.x, Start.y + Perp.y);
	}
	if(vQuads.empty())
		return;
	pGraphics->QuadsBegin();
	pGraphics->SetColor(Color);
	pGraphics->QuadsDrawFreeform(vQuads.data(), vQuads.size());
	pGraphics->QuadsEnd();
}
}

void CPastaTas::OnReset()
{
	m_vFrames.clear();
	m_vRecordedWorldTicks.clear();
	m_vRecordedCores.clear();
	m_vRecordedPositions.clear();
	m_vRecordedPrevPositions.clear();
	m_vRecordedFreezeTimes.clear();
	m_vRecordedReloadTimers.clear();
	m_PlayCursor = 0;
	m_LastRecordTasTick = -1;
	m_LastAutoSave = time_get();
	m_Recording = false;
	m_Playing = false;
	m_Paused = false;
	m_HaveObservedInput = false;
	m_LatchedJump = false;
	m_WasFrozenInWorld = false;
	m_Accumulator = 0.0;
	m_LastSeekTime = 0;
	m_SmoothIntra = 0.0f;
	m_PlaybackStepRateOverride = 0;
	mem_zero(&m_ObservedInput, sizeof(m_ObservedInput));
	m_ObservedMouse = vec2(0.0f, 0.0f);
	m_ObservedPos = vec2(0.0f, 0.0f);
	g_Config.m_PastaTasInit = 0;
	ShutdownWorld();
}

float CPastaTas::GetStepRate() const
{
	if(m_Playing && m_PlaybackStepRateOverride > 0)
		return (float)m_PlaybackStepRateOverride;
	return (float)std::clamp(g_Config.m_PastaTasTps, 1, 50);
}

void CPastaTas::OnStateChange(int NewState, int OldState)
{
	(void)OldState;
	if(NewState != IClient::STATE_ONLINE)
	{
		g_Config.m_PastaRecordReplay = 0;
		g_Config.m_PastaLoadReplay = 0;
		g_Config.m_PastaTasPause = 0;
		g_Config.m_PastaTasRespawn = 0;
		g_Config.m_PastaTasInit = 0;
		OnReset();
	}
}

void CPastaTas::EnsureWorldInitialized()
{
	if(m_WorldInitialized)
		return;

	if(Client()->State() != IClient::STATE_ONLINE)
		return;

	m_pWorld = new CGameWorld();
	m_pWorld->Init(Collision(), GameClient()->m_GameWorld.TuningList(), nullptr);
	m_pWorld->CopyWorld(&GameClient()->m_PredictedWorld);
	m_pWorld->m_GameTick = Client()->PredGameTick(g_Config.m_ClDummy);

	m_pWorldPredicted = new CGameWorld();
	m_pWorldPredicted->Init(Collision(), GameClient()->m_GameWorld.TuningList(), nullptr);
	m_pWorldPredicted->CopyWorld(m_pWorld);
	m_pWorldPredicted->m_GameTick = m_pWorld->m_GameTick + 1;

	m_WorldInitialized = true;
}

void CPastaTas::ShutdownWorld()
{
	if(m_pWorld != nullptr)
	{
		delete m_pWorld;
		m_pWorld = nullptr;
	}
	if(m_pWorldPredicted != nullptr)
	{
		delete m_pWorldPredicted;
		m_pWorldPredicted = nullptr;
	}
	m_WorldInitialized = false;
}

void CPastaTas::ResetWorldFromPredicted()
{
	ShutdownWorld();
	EnsureWorldInitialized();
}

void CPastaTas::StartRecording()
{
	g_Config.m_PastaTasInit = 0;
	ShutdownWorld();
	m_vFrames.clear();
	m_vRecordedWorldTicks.clear();
	m_vRecordedCores.clear();
	m_vRecordedPositions.clear();
	m_vRecordedPrevPositions.clear();
	m_vRecordedFreezeTimes.clear();
	m_vRecordedReloadTimers.clear();
	m_PlayCursor = 0;
	m_LastRecordTasTick = -1;
	m_LastAutoSave = time_get();
	m_Accumulator = 0.0;
	m_LastSeekTime = 0;
	m_LatchedJump = false;
	m_WasFrozenInWorld = false;
	m_Recording = true;
	m_Playing = false;
	g_Config.m_PastaTasInit = 1;
	g_Config.m_PastaLoadReplay = 0;
	ResetWorldFromPredicted();
}

void CPastaTas::StopRecording()
{
	m_Recording = false;
	if(!m_Playing)
		ShutdownWorld();
}

bool CPastaTas::FindReplayPath(char *pBuf, int BufSize, bool ExactStemOnly) const
{
	pBuf[0] = '\0';
	const char *pAppData = std::getenv("APPDATA");
	if(pAppData == nullptr || pAppData[0] == '\0')
		return false;

	namespace fs = std::filesystem;
	const fs::path Root = fs::path(pAppData) / "DDNet" / "pastateam.com";
	if(!fs::exists(Root))
		return false;

	std::error_code ErrorCode;
	for(const auto &Entry : fs::recursive_directory_iterator(Root, ErrorCode))
	{
		if(ErrorCode)
			break;
		if(!Entry.is_regular_file() || Entry.path().extension() != ".tas")
			continue;

		const std::string Stem = Entry.path().stem().string();
		const std::string Relative = fs::relative(Entry.path(), Root, ErrorCode).generic_string();
		if((ExactStemOnly && Stem == g_Config.m_PastaTasReplayName) ||
			(!ExactStemOnly && (Stem == g_Config.m_PastaTasReplayName || Relative == g_Config.m_PastaTasReplayName)))
		{
			str_copy(pBuf, Entry.path().string().c_str(), BufSize);
			return true;
		}
	}

	return false;
}

bool CPastaTas::GetReplaySavePath(char *pBuf, int BufSize, bool AutoSave) const
{
	pBuf[0] = '\0';
	const char *pAppData = std::getenv("APPDATA");
	if(pAppData == nullptr || pAppData[0] == '\0')
		return false;

	namespace fs = std::filesystem;
	fs::path ReplayDir = fs::path(pAppData) / "DDNet" / "pastateam.com" / "tas";
	if(AutoSave)
		ReplayDir /= "auto";
	std::error_code ErrorCode;
	fs::create_directories(ReplayDir, ErrorCode);

	char aName[128];
	if(AutoSave)
	{
		char aTimestamp[64];
		str_timestamp_format(aTimestamp, sizeof(aTimestamp), "%Y%m%d_%H%M%S");
		str_format(aName, sizeof(aName), "%s_%s.tas", g_Config.m_PastaTasReplayName[0] != '\0' ? g_Config.m_PastaTasReplayName : "my_tas", aTimestamp);
	}
	else
		str_format(aName, sizeof(aName), "%s.tas", g_Config.m_PastaTasReplayName[0] != '\0' ? g_Config.m_PastaTasReplayName : "my_tas");

	str_copy(pBuf, (ReplayDir / aName).string().c_str(), BufSize);
	return true;
}

bool CPastaTas::SaveReplayToPath(const char *pPath) const
{
	std::ofstream File(pPath, std::ios::out | std::ios::trunc);
	if(!File.is_open())
		return false;

	File << gs_pTasMagic << "\n";
	File << "frames " << m_vFrames.size() << "\n";
	for(const SFrame &Frame : m_vFrames)
	{
		File
			<< Frame.m_Tick << ' '
			<< Frame.m_Input.m_Direction << ' '
			<< Frame.m_Input.m_TargetX << ' '
			<< Frame.m_Input.m_TargetY << ' '
			<< Frame.m_Input.m_Jump << ' '
			<< Frame.m_Input.m_Fire << ' '
			<< Frame.m_Input.m_Hook << ' '
			<< Frame.m_Input.m_PlayerFlags << ' '
			<< Frame.m_Input.m_WantedWeapon << ' '
			<< Frame.m_Input.m_NextWeapon << ' '
			<< Frame.m_Input.m_PrevWeapon << ' '
			<< Frame.m_Mouse.x << ' '
			<< Frame.m_Mouse.y << ' '
			<< Frame.m_Pos.x << ' '
			<< Frame.m_Pos.y << "\n";
	}
	return true;
}

bool CPastaTas::LoadReplayFromPath(const char *pPath)
{
	std::ifstream File(pPath);
	if(!File.is_open())
		return false;

	std::string Line;
	if(!std::getline(File, Line) || Line != gs_pTasMagic)
		return false;
	if(!std::getline(File, Line))
		return false;

	std::vector<SFrame> vFrames;
	while(std::getline(File, Line))
	{
		if(Line.empty())
			continue;
		SFrame Frame;
		std::istringstream Stream(Line);
		Stream
			>> Frame.m_Tick
			>> Frame.m_Input.m_Direction
			>> Frame.m_Input.m_TargetX
			>> Frame.m_Input.m_TargetY
			>> Frame.m_Input.m_Jump
			>> Frame.m_Input.m_Fire
			>> Frame.m_Input.m_Hook
			>> Frame.m_Input.m_PlayerFlags
			>> Frame.m_Input.m_WantedWeapon
			>> Frame.m_Input.m_NextWeapon
			>> Frame.m_Input.m_PrevWeapon
			>> Frame.m_Mouse.x
			>> Frame.m_Mouse.y
			>> Frame.m_Pos.x
			>> Frame.m_Pos.y;
		if(Stream.fail())
			return false;
		vFrames.push_back(Frame);
	}

	m_vFrames = std::move(vFrames);
	m_PlayCursor = 0;
	return !m_vFrames.empty();
}

bool CPastaTas::StartPlayback()
{
	if(m_vFrames.empty())
	{
		char aPath[IO_MAX_PATH_LENGTH];
		if(!FindReplayPath(aPath, sizeof(aPath), false) || !LoadReplayFromPath(aPath))
		{
			PushTasWarning("TAS", "Replay not found or failed to load.");
			g_Config.m_PastaLoadReplay = 0;
			return false;
		}
	}

	const vec2 StartPos = m_vFrames.front().m_Pos;
	vec2 CurrentPos = GameClient()->m_LocalCharacterPos;
	const int LocalId = GameClient()->m_aLocalIds[g_Config.m_ClDummy];
	if(LocalId >= 0)
	{
		if(CCharacter *pLocalChar = GameClient()->m_PredictedWorld.GetCharacterById(LocalId))
			CurrentPos = pLocalChar->GetPos();
	}
	if(distance(CurrentPos, StartPos) > 28.0f)
	{
		char aBuf[128];
		str_format(aBuf, sizeof(aBuf), "You are not standing on the start point. Stand at %.0f %.0f.", StartPos.x, StartPos.y);
		PushTasWarning("TAS", aBuf);
		g_Config.m_PastaLoadReplay = 0;
		return false;
	}

	m_PlayCursor = std::clamp(m_PlayCursor, 0, maximum(0, (int)m_vFrames.size() - 1));
	m_Accumulator = 0.0;
	m_LastSeekTime = 0;
	g_Config.m_PastaTasInit = 1;
	m_Playing = true;
	m_Recording = false;
	g_Config.m_PastaTasPause = 0;
	m_Paused = false;
	m_PlaybackStepRateOverride = 50;
	g_Config.m_PastaRecordReplay = 0;
	ResetWorldFromPredicted();
	if(!m_vFrames.empty())
	{
		TickWorld(m_vFrames[0].m_Input, m_vFrames[0].m_Mouse);
		m_PlayCursor = m_vFrames.size() > 1 ? 1 : 0;
	}
	return true;
}

void CPastaTas::StopPlayback()
{
	m_Playing = false;
	m_PlaybackStepRateOverride = 0;
	if(!m_Recording)
		ShutdownWorld();
}

void CPastaTas::ClearReplay(bool KillPlayer)
{
	m_vFrames.clear();
	m_vRecordedWorldTicks.clear();
	m_vRecordedCores.clear();
	m_vRecordedPositions.clear();
	m_vRecordedPrevPositions.clear();
	m_vRecordedFreezeTimes.clear();
	m_vRecordedReloadTimers.clear();
	m_PlayCursor = 0;
	m_LastRecordTasTick = -1;
	m_Recording = false;
	m_Playing = false;
	m_Paused = false;
	g_Config.m_PastaRecordReplay = 0;
	g_Config.m_PastaLoadReplay = 0;
	g_Config.m_PastaTasPause = 0;
	g_Config.m_PastaTasInit = 0;
	ShutdownWorld();
	if(KillPlayer && Client()->State() == IClient::STATE_ONLINE)
		GameClient()->m_Chat.SendChat(0, "/kill");
}

void CPastaTas::StepPlayback(int Direction, int Amount)
{
	if(m_vFrames.empty())
		return;
	m_PlayCursor = std::clamp(m_PlayCursor + Direction * maximum(1, Amount), 0, (int)m_vFrames.size() - 1);
	ResimulatePlaybackWorld();
}

void CPastaTas::AdvancePlaybackOneStep()
{
	if(m_vFrames.empty())
		return;
	if(m_PlayCursor + 1 < (int)m_vFrames.size())
	{
		++m_PlayCursor;
	}
	else if(g_Config.m_PastaTasAutoReplay)
	{
		m_PlayCursor = 0;
		ResetWorldFromPredicted();
	}
	else
	{
		m_Playing = false;
		g_Config.m_PastaLoadReplay = 0;
	}
}

void CPastaTas::TickWorld(const CNetObj_PlayerInput &Input, vec2 Mouse)
{
	if(!m_WorldInitialized || m_pWorld == nullptr)
		return;

	for(int Dummy = 0; Dummy < NUM_DUMMIES; ++Dummy)
	{
		const int LocalId = GameClient()->m_aLocalIds[Dummy];
		if(LocalId < 0)
			continue;

		CCharacter *pChar = m_pWorld->GetCharacterById(LocalId);
		if(pChar == nullptr)
			continue;

		CNetObj_PlayerInput AppliedInput = Dummy == g_Config.m_ClDummy ? Input : GameClient()->m_Controls.m_aInputData[Dummy];
		if(Dummy == g_Config.m_ClDummy)
		{
			AppliedInput.m_TargetX = (int)Mouse.x;
			AppliedInput.m_TargetY = (int)Mouse.y;
			if(!AppliedInput.m_TargetX && !AppliedInput.m_TargetY)
				AppliedInput.m_TargetX = 1;
		}

		pChar->OnDirectInput(&AppliedInput);
		pChar->OnDirectInput(&AppliedInput);
		pChar->OnPredictedInput(&AppliedInput);
	}

	m_pWorld->Tick();
	m_pWorld->m_GameTick++;
	UpdatePredictedWorld(Input, Mouse);
}

void CPastaTas::UpdatePredictedWorld(const CNetObj_PlayerInput &Input, vec2 Mouse)
{
	if(!m_WorldInitialized || m_pWorld == nullptr || m_pWorldPredicted == nullptr)
		return;

	m_pWorldPredicted->CopyWorld(m_pWorld);
	m_pWorldPredicted->m_GameTick = m_pWorld->m_GameTick + 1;

	const int LocalId = GameClient()->m_aLocalIds[g_Config.m_ClDummy];
	if(LocalId >= 0)
	{
		if(CCharacter *pChar = m_pWorldPredicted->GetCharacterById(LocalId))
		{
			CNetObj_PlayerInput PredInput = Input;
			PredInput.m_TargetX = (int)Mouse.x;
			PredInput.m_TargetY = (int)Mouse.y;
			if(!PredInput.m_TargetX && !PredInput.m_TargetY)
				PredInput.m_TargetX = 1;
			pChar->OnDirectInput(&PredInput);
			pChar->OnDirectInput(&PredInput);
			pChar->OnPredictedInput(&PredInput);
		}
	}

	m_pWorldPredicted->Tick();
}

void CPastaTas::ResimulatePlaybackWorld()
{
	if(!m_Playing || m_vFrames.empty())
		return;

	ResetWorldFromPredicted();
	if(!m_WorldInitialized)
		return;

	const int LastIndex = std::clamp(m_PlayCursor, 0, (int)m_vFrames.size() - 1);
	for(int i = 0; i < LastIndex; ++i)
		TickWorld(m_vFrames[i].m_Input, m_vFrames[i].m_Mouse);
}

bool CPastaTas::IsLocalFrozenInWorld() const
{
	if(!m_WorldInitialized || m_pWorld == nullptr)
		return false;

	const int LocalId = GameClient()->m_aLocalIds[g_Config.m_ClDummy];
	if(LocalId < 0)
		return false;

	const CCharacter *pChar = m_pWorld->GetCharacterById(LocalId);
	if(pChar == nullptr)
		return false;

	return pChar->m_FreezeTime > 0 || pChar->Core()->m_DeepFrozen || pChar->Core()->m_LiveFrozen;
}

void CPastaTas::RewindRecording(int Amount)
{
	if(!m_Recording || m_vFrames.empty())
		return;

	for(int Step = 0; Step < maximum(1, Amount) && !m_vFrames.empty(); ++Step)
	{
		m_vFrames.pop_back();
		if(!m_vRecordedWorldTicks.empty())
			m_vRecordedWorldTicks.pop_back();
		if(!m_vRecordedCores.empty())
			m_vRecordedCores.pop_back();
		if(!m_vRecordedPositions.empty())
			m_vRecordedPositions.pop_back();
		if(!m_vRecordedPrevPositions.empty())
			m_vRecordedPrevPositions.pop_back();
		if(!m_vRecordedFreezeTimes.empty())
			m_vRecordedFreezeTimes.pop_back();
		if(!m_vRecordedReloadTimers.empty())
			m_vRecordedReloadTimers.pop_back();
	}

	if(m_vFrames.empty() || m_vRecordedWorldTicks.empty() || m_vRecordedCores.empty())
	{
		m_PlayCursor = 0;
		m_LastRecordTasTick = -1;
		ResetWorldFromPredicted();
		return;
	}

	m_PlayCursor = (int)m_vFrames.size() - 1;
	m_LastRecordTasTick = m_vFrames.back().m_Tick;
	m_ObservedInput = m_vFrames.back().m_Input;
	m_ObservedMouse = m_vFrames.back().m_Mouse;
	m_ObservedPos = m_vFrames.back().m_Pos;
	m_HaveObservedInput = true;
	m_LatchedJump = m_ObservedInput.m_Jump != 0;

	const int LocalId = GameClient()->m_aLocalIds[g_Config.m_ClDummy];
	if(LocalId < 0 || m_pWorld == nullptr)
		return;

	CCharacter *pChar = m_pWorld->GetCharacterById(LocalId);
	if(pChar == nullptr)
		return;

	pChar->SetCore(m_vRecordedCores.back());
	pChar->m_Pos = m_vRecordedPositions.back();
	pChar->m_PrevPos = m_vRecordedPrevPositions.back();
	pChar->m_FreezeTime = m_vRecordedFreezeTimes.back();
	pChar->SetReloadTimer(m_vRecordedReloadTimers.back());
	m_pWorld->m_GameTick = m_vRecordedWorldTicks.back();
	UpdatePredictedWorld(m_ObservedInput, m_ObservedMouse);
}

void CPastaTas::StepForwardRecording()
{
	if(!m_Recording || !m_HaveObservedInput)
		return;

	CNetObj_PlayerInput RecordedInput = m_ObservedInput;
	if(m_LatchedJump)
		RecordedInput.m_Jump = 1;
	TickWorld(RecordedInput, m_ObservedMouse);

	const int TasTick = m_pWorld != nullptr ? m_pWorld->m_GameTick : (m_LastRecordTasTick + 1);
	if(TasTick == m_LastRecordTasTick)
		return;

	vec2 RecordedPos = m_ObservedPos;
	const int LocalId = GameClient()->m_aLocalIds[g_Config.m_ClDummy];
	if(m_pWorld != nullptr && LocalId >= 0)
	{
		if(CCharacter *pRecordedChar = m_pWorld->GetCharacterById(LocalId))
		{
			RecordedPos = pRecordedChar->GetPos();
			m_vRecordedWorldTicks.push_back(m_pWorld->m_GameTick);
			m_vRecordedCores.push_back(pRecordedChar->GetCore());
			m_vRecordedPositions.push_back(pRecordedChar->GetPos());
			m_vRecordedPrevPositions.push_back(pRecordedChar->m_PrevPos);
			m_vRecordedFreezeTimes.push_back(pRecordedChar->m_FreezeTime);
			m_vRecordedReloadTimers.push_back(pRecordedChar->GetReloadTimer());
		}
	}

	m_vFrames.push_back({TasTick, RecordedInput, m_ObservedMouse, RecordedPos});
	m_LastRecordTasTick = TasTick;
	m_PlayCursor = (int)m_vFrames.size() - 1;
	m_LatchedJump = false;
}

void CPastaTas::AutoForwardRecording()
{
	if(!m_Recording || !m_HaveObservedInput || !IsLocalFrozenInWorld())
		return;

	const int MaxTicks = maximum(1, g_Config.m_PastaTasTickControlTicks) * 16;
	for(int i = 0; i < MaxTicks && IsLocalFrozenInWorld(); ++i)
	{
		CNetObj_PlayerInput RecordedInput = m_ObservedInput;
		if(m_LatchedJump)
			RecordedInput.m_Jump = 1;
		TickWorld(RecordedInput, m_ObservedMouse);

		const int TasTick = m_pWorld != nullptr ? m_pWorld->m_GameTick : (m_LastRecordTasTick + 1);
		vec2 RecordedPos = m_ObservedPos;
		const int LocalId = GameClient()->m_aLocalIds[g_Config.m_ClDummy];
		if(m_pWorld != nullptr && LocalId >= 0)
		{
			if(CCharacter *pRecordedChar = m_pWorld->GetCharacterById(LocalId))
			{
				RecordedPos = pRecordedChar->GetPos();
				m_vRecordedWorldTicks.push_back(m_pWorld->m_GameTick);
				m_vRecordedCores.push_back(pRecordedChar->GetCore());
				m_vRecordedPositions.push_back(pRecordedChar->GetPos());
				m_vRecordedPrevPositions.push_back(pRecordedChar->m_PrevPos);
				m_vRecordedFreezeTimes.push_back(pRecordedChar->m_FreezeTime);
				m_vRecordedReloadTimers.push_back(pRecordedChar->GetReloadTimer());
			}
		}

		m_vFrames.push_back({TasTick, RecordedInput, m_ObservedMouse, RecordedPos});
		m_LastRecordTasTick = TasTick;
		m_PlayCursor = (int)m_vFrames.size() - 1;
		m_LatchedJump = false;
	}
}

void CPastaTas::OnUpdate()
{
	if(Client()->State() != IClient::STATE_ONLINE)
		return;

	if(g_Config.m_PastaTasFakeAim)
	{
		if(!m_vFrames.empty())
		{
			for(size_t i = 0; i < m_vFrames.size(); ++i)
			{
				SFrame &Frame = m_vFrames[i];
				const bool EssentialAim = Frame.m_Input.m_Fire != 0 || Frame.m_Input.m_Hook != 0;
				if(EssentialAim)
					continue;

				switch(g_Config.m_PastaTasFakeAimMode)
				{
				case 0: // robot aim
					if(i > 0)
						Frame.m_Mouse = m_vFrames[i - 1].m_Mouse;
					break;
				case 1: // spinbot
				{
					const float Angle = (float)i * 0.28f;
					Frame.m_Mouse = vec2(cosf(Angle), sinf(Angle)) * 380.0f;
					break;
				}
				case 2: // random
				{
					const float Angle = random_float() * pi * 2.0f;
					Frame.m_Mouse = vec2(cosf(Angle), sinf(Angle)) * (220.0f + random_float() * 220.0f);
					break;
				}
				case 3: // smooth
					if(i > 0)
						Frame.m_Mouse = mix(m_vFrames[i - 1].m_Mouse, Frame.m_Mouse, 0.2f);
					break;
				default:
					break;
				}
				Frame.m_Input.m_TargetX = (int)Frame.m_Mouse.x;
				Frame.m_Input.m_TargetY = (int)Frame.m_Mouse.y;
				if(!Frame.m_Input.m_TargetX && !Frame.m_Input.m_TargetY)
					Frame.m_Input.m_TargetX = 1;
			}
		}
		g_Config.m_PastaTasFakeAim = 0;
	}

	if(g_Config.m_PastaTasRespawn)
	{
		ClearReplay(true);
		g_Config.m_PastaTasRespawn = 0;
	}

	if(g_Config.m_PastaTasInit && !m_WorldInitialized && Client()->State() == IClient::STATE_ONLINE)
		ResetWorldFromPredicted();
	else if(!g_Config.m_PastaTasInit && m_WorldInitialized && !m_Recording && !m_Playing)
		ShutdownWorld();

	if(g_Config.m_PastaRecordReplay && !m_Recording)
		StartRecording();
	else if(!g_Config.m_PastaRecordReplay && m_Recording)
		StopRecording();

	if(g_Config.m_PastaLoadReplay && !m_Playing)
		StartPlayback();
	else if(!g_Config.m_PastaLoadReplay && m_Playing)
		StopPlayback();

	m_Paused = g_Config.m_PastaTasPause != 0;
}

void CPastaTas::ObserveInput(const CNetObj_PlayerInput &Input, vec2 Pos)
{
	m_HaveObservedInput = true;
	m_ObservedInput = Input;
	if(Input.m_Jump != 0)
		m_LatchedJump = true;
	m_ObservedMouse = vec2((float)Input.m_TargetX, (float)Input.m_TargetY);
	if(length(m_ObservedMouse) < 0.001f)
		m_ObservedMouse = vec2(1.0f, 0.0f);
	m_ObservedPos = Pos;
}

bool CPastaTas::GetPlaybackInput(CNetObj_PlayerInput &Out) const
{
	if(!m_Playing || m_vFrames.empty())
		return false;
	Out = m_vFrames[std::clamp(m_PlayCursor, 0, (int)m_vFrames.size() - 1)].m_Input;
	return true;
}

bool CPastaTas::HasRenderMouseOverride() const
{
	return m_WorldInitialized && (m_Recording || (m_Playing && g_Config.m_PastaTasShowAim));
}

vec2 CPastaTas::GetRenderMousePos() const
{
	if(!HasRenderMouseOverride())
		return vec2(0.0f, 0.0f);
	if(m_Recording)
	{
		const vec2 Mouse = GameClient()->m_Controls.m_aMousePos[g_Config.m_ClDummy];
		return length(Mouse) < 0.001f ? vec2(1.0f, 0.0f) : Mouse;
	}
	if(m_Playing && !m_vFrames.empty())
		return m_vFrames[std::clamp(m_PlayCursor, 0, (int)m_vFrames.size() - 1)].m_Mouse;
	return m_ObservedMouse;
}

bool CPastaTas::IsWorldActive() const
{
	return (m_Recording || m_Playing) && m_WorldInitialized;
}

vec2 CPastaTas::GetInterpolatedPlayerPos() const
{
	if(!IsWorldActive())
		return GameClient()->m_LocalCharacterPos;

	const int LocalId = GameClient()->m_aLocalIds[g_Config.m_ClDummy];
	if(LocalId < 0 || m_pWorld == nullptr)
		return GameClient()->m_LocalCharacterPos;

	CCharacter *pCommitted = m_pWorld->GetCharacterById(LocalId);
	if(pCommitted == nullptr)
		return GameClient()->m_LocalCharacterPos;

	CCharacter *pPredicted = m_pWorldPredicted != nullptr ? m_pWorldPredicted->GetCharacterById(LocalId) : nullptr;
	const vec2 PrevPos = pCommitted->GetPos();
	const vec2 CurPos = pPredicted != nullptr ? pPredicted->GetPos() : PrevPos;
	return mix(PrevPos, CurPos, std::clamp(m_SmoothIntra, 0.0f, 1.0f));
}

void CPastaTas::PushTasWarning(const char *pTitle, const char *pMessage) const
{
	Client()->AddWarning(SWarning(pTitle, pMessage));
}

void CPastaTas::SaveReplay()
{
	if(m_vFrames.empty())
	{
		PushTasWarning("TAS", "Replay buffer is empty.");
		return;
	}

	char aPath[IO_MAX_PATH_LENGTH];
	if(!GetReplaySavePath(aPath, sizeof(aPath), false) || !SaveReplayToPath(aPath))
	{
		PushTasWarning("TAS", "Failed to save replay.");
		return;
	}

	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "Saved replay to %s", aPath);
	PushTasWarning("TAS", aBuf);
}

void CPastaTas::LoadSelectedReplay()
{
	char aPath[IO_MAX_PATH_LENGTH];
	if(!FindReplayPath(aPath, sizeof(aPath), false))
	{
		PushTasWarning("TAS", "No replay selected.");
		return;
	}

	if(m_Playing)
	{
		m_Playing = false;
		g_Config.m_PastaLoadReplay = 0;
	}
	m_Recording = false;
	g_Config.m_PastaRecordReplay = 0;
	g_Config.m_PastaTasPause = 0;
	m_Paused = false;

	if(!LoadReplayFromPath(aPath))
	{
		PushTasWarning("TAS", "Failed to load replay.");
		return;
	}

	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "Loaded replay %s", aPath);
	PushTasWarning("TAS", aBuf);
}

void CPastaTas::ValidateReplay()
{
	if(!m_vFrames.empty())
	{
		PushTasWarning("TAS", "Replay buffer is valid.");
		return;
	}

	char aPath[IO_MAX_PATH_LENGTH];
	if(!FindReplayPath(aPath, sizeof(aPath), false))
	{
		PushTasWarning("TAS", "No replay selected.");
		return;
	}

	if(LoadReplayFromPath(aPath))
		PushTasWarning("TAS", "Replay file looks valid.");
	else
		PushTasWarning("TAS", "Replay file failed to validate.");
}

void CPastaTas::ReportReplayTime()
{
	const int Frames = (int)m_vFrames.size();
	if(Frames <= 0)
	{
		PushTasWarning("TAS", "Replay buffer is empty.");
		return;
	}

	const float Seconds = Frames / GetStepRate();
	char aBuf[128];
	str_format(aBuf, sizeof(aBuf), "%d frames, %.2fs", Frames, Seconds);
	PushTasWarning("TAS", aBuf);
}

void CPastaTas::RemoveUseless()
{
	if(m_vFrames.size() < 2)
	{
		PushTasWarning("TAS", "Nothing to optimize.");
		return;
	}

	std::vector<SFrame> vOptimized;
	vOptimized.reserve(m_vFrames.size());
	vOptimized.push_back(m_vFrames.front());
	for(size_t i = 1; i < m_vFrames.size(); ++i)
	{
		const bool SameInput = mem_comp(&m_vFrames[i - 1].m_Input, &m_vFrames[i].m_Input, sizeof(CNetObj_PlayerInput)) == 0;
		const bool SameMouse = distance(m_vFrames[i - 1].m_Mouse, m_vFrames[i].m_Mouse) < 0.01f;
		if(!SameInput || !SameMouse)
			vOptimized.push_back(m_vFrames[i]);
	}

	const int Removed = (int)m_vFrames.size() - (int)vOptimized.size();
	m_vFrames = std::move(vOptimized);
	m_PlayCursor = std::clamp(m_PlayCursor, 0, maximum(0, (int)m_vFrames.size() - 1));

	char aBuf[128];
	str_format(aBuf, sizeof(aBuf), "Removed %d repeated frames.", maximum(0, Removed));
	PushTasWarning("TAS", aBuf);
}

void CPastaTas::SyncNow()
{
	PushTasWarning("TAS", "Replay Vault sync is not wired yet.");
}

void CPastaTas::RenderStartEndPos()
{
	if(!g_Config.m_PastaTasDrawStartEndPos || m_vFrames.empty())
		return;

	const int LocalId = GameClient()->m_aLocalIds[g_Config.m_ClDummy];
	if(LocalId < 0)
		return;

	CTeeRenderInfo TeeInfo = GameClient()->m_aClients[LocalId].m_RenderInfo;
	TeeInfo.m_Size = 64.0f;
	const CAnimState *pIdleState = CAnimState::GetIdle();

	CTeeRenderInfo StartInfo = TeeInfo;
	StartInfo.m_CustomColoredSkin = true;
	StartInfo.m_ColorBody = ColorRGBA(0.15f, 1.0f, 0.35f, 1.0f);
	StartInfo.m_ColorFeet = ColorRGBA(0.15f, 1.0f, 0.35f, 1.0f);
	GameClient()->RenderTools()->RenderTee(pIdleState, &StartInfo, EMOTE_NORMAL, vec2(1.0f, 0.0f), m_vFrames.front().m_Pos);

	if(!m_Recording && m_vFrames.size() > 1)
	{
		CTeeRenderInfo EndInfo = TeeInfo;
		EndInfo.m_CustomColoredSkin = true;
		EndInfo.m_ColorBody = ColorRGBA(1.0f, 0.2f, 0.95f, 1.0f);
		EndInfo.m_ColorFeet = ColorRGBA(1.0f, 0.2f, 0.95f, 1.0f);
		GameClient()->RenderTools()->RenderTee(pIdleState, &EndInfo, EMOTE_NORMAL, vec2(1.0f, 0.0f), m_vFrames.back().m_Pos);
	}
}

void CPastaTas::RenderReplayPath()
{
	if(!g_Config.m_PastaTasDrawPath || m_vFrames.size() < 2 || !m_Playing || m_Recording)
		return;

	const ColorRGBA PathColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_PastaTasDrawPathColor));
	int StartIndex = 0;
	int EndIndex = (int)m_vFrames.size() - 1;
	if(g_Config.m_PastaTasDrawPathSegmented)
	{
		const int Pivot = std::clamp(m_PlayCursor, 0, EndIndex);
		StartIndex = Pivot;
		EndIndex = minimum((int)m_vFrames.size() - 1, Pivot + 180);
	}

	Graphics()->TextureClear();
	if(g_Config.m_PastaTasDrawPathMode == 0)
	{
		std::vector<vec2> vPoints;
		vPoints.reserve(EndIndex - StartIndex + 1);
		for(int i = StartIndex; i <= EndIndex; ++i)
			if(i % 2 == 0)
				vPoints.push_back(m_vFrames[i].m_Pos);
		const auto DotTexture = GameClient()->m_ParticlesSkin.m_aSpriteParticles[SPRITE_PART_BALL - SPRITE_PART_SLICE];
		RenderTasPoints(Graphics(), DotTexture, vPoints, PathColor, 4.0f);
	}
	else
	{
		std::vector<IGraphics::CLineItem> vSegments;
		vSegments.reserve(EndIndex - StartIndex);
		for(int i = StartIndex; i < EndIndex; ++i)
		{
			const vec2 Pos1 = m_vFrames[i].m_Pos;
			const vec2 Pos2 = m_vFrames[i + 1].m_Pos;
			vSegments.emplace_back(Pos1.x, Pos1.y, Pos2.x, Pos2.y);
		}
		RenderTasLines(Graphics(), vSegments, PathColor, 1.35f, GameClient()->m_Camera.m_Zoom);
	}
}

void CPastaTas::RenderPredictionPath()
{
	if(!g_Config.m_PastaTasDrawPredictionPath || !m_WorldInitialized || m_pWorld == nullptr || m_Playing)
		return;

	const int LocalId = GameClient()->m_aLocalIds[g_Config.m_ClDummy];
	if(LocalId < 0)
		return;
	CCharacter *pChar = m_pWorld->GetCharacterById(LocalId);
	if(pChar == nullptr)
		return;

	CGameWorld TempWorld;
	TempWorld.Init(Collision(), GameClient()->m_GameWorld.TuningList(), nullptr);
	TempWorld.CopyWorld(m_pWorld);
	CCharacter *pTempChar = TempWorld.GetCharacterById(LocalId);
	if(pTempChar == nullptr)
		return;

	const CNetObj_PlayerInput PredictInput = m_Playing && !m_vFrames.empty() ? m_vFrames[std::clamp(m_PlayCursor, 0, (int)m_vFrames.size() - 1)].m_Input : m_ObservedInput;
	std::vector<vec2> vPath;
	const int Ticks = 30;
	vPath.reserve(Ticks + 1);
	for(int t = 0; t < Ticks; ++t)
	{
		vPath.push_back(pTempChar->GetPos());
		pTempChar->OnDirectInput(&PredictInput);
		pTempChar->OnDirectInput(&PredictInput);
		pTempChar->OnPredictedInput(&PredictInput);
		TempWorld.Tick();
		TempWorld.m_GameTick++;
	}
	vPath.push_back(pTempChar->GetPos());

	const ColorRGBA LocalColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_PastaTasDrawPredictionPathColor));
	const ColorRGBA FrozenColor = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_PastaTasDrawPredictionPathColorFrozen));
	Graphics()->TextureClear();
	if(g_Config.m_PastaTasDrawPredictionPathMode == 0)
	{
		std::vector<vec2> vLocalDots;
		std::vector<vec2> vFrozenDots;
		for(const vec2 &Pos : vPath)
		{
			bool Frozen = false;
			if(Collision() != nullptr)
			{
				const int Index = Collision()->GetPureMapIndex(Pos);
				const int Tile = Collision()->GetTileIndex(Index);
				const int FrontTile = Collision()->GetFrontTileIndex(Index);
				Frozen = Tile == TILE_FREEZE || FrontTile == TILE_FREEZE || Tile == TILE_DFREEZE || FrontTile == TILE_DFREEZE || Tile == TILE_LFREEZE || FrontTile == TILE_LFREEZE;
			}
			(Frozen ? vFrozenDots : vLocalDots).push_back(Pos);
		}
		const auto DotTexture = GameClient()->m_ParticlesSkin.m_aSpriteParticles[SPRITE_PART_BALL - SPRITE_PART_SLICE];
		RenderTasPoints(Graphics(), DotTexture, vLocalDots, LocalColor, 3.5f);
		RenderTasPoints(Graphics(), DotTexture, vFrozenDots, FrozenColor, 3.5f);
	}
	else
	{
		std::vector<IGraphics::CLineItem> vLocalSegments;
		std::vector<IGraphics::CLineItem> vFrozenSegments;
		for(size_t i = 1; i < vPath.size(); ++i)
		{
			bool Frozen = false;
			if(Collision() != nullptr)
			{
				const int Index = Collision()->GetPureMapIndex(vPath[i - 1]);
				const int Tile = Collision()->GetTileIndex(Index);
				const int FrontTile = Collision()->GetFrontTileIndex(Index);
				Frozen = Tile == TILE_FREEZE || FrontTile == TILE_FREEZE || Tile == TILE_DFREEZE || FrontTile == TILE_DFREEZE || Tile == TILE_LFREEZE || FrontTile == TILE_LFREEZE;
			}
			(Frozen ? vFrozenSegments : vLocalSegments).emplace_back(vPath[i - 1].x, vPath[i - 1].y, vPath[i].x, vPath[i].y);
		}
		RenderTasLines(Graphics(), vLocalSegments, LocalColor, 1.2f, GameClient()->m_Camera.m_Zoom);
		RenderTasLines(Graphics(), vFrozenSegments, FrozenColor, 1.2f, GameClient()->m_Camera.m_Zoom);
	}
}

void CPastaTas::RenderTasWorld()
{
	if(!m_WorldInitialized || m_pWorld == nullptr || !m_Recording)
		return;

	const float Intra = std::clamp(m_SmoothIntra, 0.0f, 1.0f);
	const int TasClientId = -3;
	for(int DummyIdx = 0; DummyIdx < NUM_DUMMIES; ++DummyIdx)
	{
		const int LocalId = GameClient()->m_aLocalIds[DummyIdx];
		if(LocalId < 0)
			continue;

		CCharacter *pCommitted = m_pWorld->GetCharacterById(LocalId);
		CCharacter *pPredicted = m_pWorldPredicted != nullptr ? m_pWorldPredicted->GetCharacterById(LocalId) : nullptr;
		if(pCommitted == nullptr)
			continue;

		const CCharacterCore &PrevCore = *pCommitted->Core();
		const CCharacterCore &CurCore = pPredicted != nullptr ? *pPredicted->Core() : PrevCore;
		const vec2 AimDir = normalize(length(GetRenderMousePos()) > 0.001f ? GetRenderMousePos() : vec2(1.0f, 0.0f));

		CNetObj_Character PrevObj{};
		CNetObj_Character CurObj{};
		PrevObj.m_X = (int)pCommitted->GetPos().x;
		PrevObj.m_Y = (int)pCommitted->GetPos().y;
		PrevObj.m_VelX = (int)(PrevCore.m_Vel.x * 256.0f);
		PrevObj.m_VelY = (int)(PrevCore.m_Vel.y * 256.0f);
		PrevObj.m_Angle = (int)(angle(AimDir) * 256.0f);
		PrevObj.m_Direction = PrevCore.m_Direction;
		PrevObj.m_Jumped = PrevCore.m_Jumped;
		PrevObj.m_HookState = PrevCore.m_HookState;
		PrevObj.m_HookX = (int)PrevCore.m_HookPos.x;
		PrevObj.m_HookY = (int)PrevCore.m_HookPos.y;
		PrevObj.m_HookedPlayer = PrevCore.HookedPlayer();
		PrevObj.m_AttackTick = 0;
		PrevObj.m_Weapon = PrevCore.m_ActiveWeapon;

		CurObj = PrevObj;
		if(pPredicted != nullptr)
		{
			CurObj.m_X = (int)pPredicted->GetPos().x;
			CurObj.m_Y = (int)pPredicted->GetPos().y;
			CurObj.m_VelX = (int)(CurCore.m_Vel.x * 256.0f);
			CurObj.m_VelY = (int)(CurCore.m_Vel.y * 256.0f);
			CurObj.m_Direction = CurCore.m_Direction;
			CurObj.m_Jumped = CurCore.m_Jumped;
			CurObj.m_HookState = CurCore.m_HookState;
			CurObj.m_HookX = (int)CurCore.m_HookPos.x;
			CurObj.m_HookY = (int)CurCore.m_HookPos.y;
			CurObj.m_HookedPlayer = CurCore.HookedPlayer();
			CurObj.m_Weapon = CurCore.m_ActiveWeapon;
		}

		CTeeRenderInfo TeeInfo = GameClient()->m_aClients[LocalId].m_RenderInfo;
		TeeInfo.m_Size = 64.0f;
		TeeInfo.m_TeeRenderFlags = 0;
		if(CurCore.m_FreezeEnd != 0 || CurCore.m_DeepFrozen)
			TeeInfo.m_TeeRenderFlags |= TEE_EFFECT_FROZEN | TEE_NO_WEAPON;
		else if(CurCore.m_LiveFrozen)
			TeeInfo.m_TeeRenderFlags |= TEE_EFFECT_FROZEN;
		TeeInfo.m_ColorBody.a = 0.9f;
		TeeInfo.m_ColorFeet.a = 0.9f;

		GameClient()->m_Players.RenderExternalHook(&PrevObj, &CurObj, &TeeInfo, TasClientId, Intra);
		GameClient()->m_Players.RenderExternalPlayer(&PrevObj, &CurObj, &TeeInfo, TasClientId, Intra);
	}
}

void CPastaTas::RenderWorldCharacters()
{
	RenderTasWorld();
}

void CPastaTas::RenderHud()
{
	if(!g_Config.m_PastaTasHud || GameClient()->m_Menus.IsActive())
		return;
	if(!m_Recording && !m_Playing)
		return;

	const float Width = 300.0f * Graphics()->ScreenAspect();
	const float Height = 300.0f;
	Graphics()->MapScreen(0, 0, Width, Height);

	const float X = Width - 95.0f;
	const float Y = 94.0f;
	const float BoxW = 88.0f;
	const float BoxH = 64.0f;
	CUIRect HudRect = {X, Y, BoxW, BoxH};
	HudRect.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.34f), IGraphics::CORNER_ALL, 3.0f);

	const int TickValue = m_Playing ? maximum(0, m_PlayCursor) : (int)m_vFrames.size();
	const float Seconds = m_vFrames.empty() ? 0.0f : TickValue / maximum(1.0f, GetStepRate());
	char aLine1[64];
	str_format(aLine1, sizeof(aLine1), "%s  %.2fs", m_Recording ? "REC" : "PLAY", Seconds);

	const CNetObj_PlayerInput *pHudInput = nullptr;
	CNetObj_PlayerInput PlaybackInput;
	if(m_Playing && GetPlaybackInput(PlaybackInput))
		pHudInput = &PlaybackInput;
	else if(m_Recording && m_HaveObservedInput)
		pHudInput = &m_ObservedInput;

	const int LocalId = GameClient()->m_aLocalIds[g_Config.m_ClDummy];
	CCharacter *pTasChar = (m_WorldInitialized && m_pWorld != nullptr && LocalId >= 0) ? m_pWorld->GetCharacterById(LocalId) : nullptr;
	int Cooldown = 0;
	const char *pWeaponName = "HAM";
	if(pTasChar != nullptr)
	{
		static const char *s_apWeaponNames[] = {"HAM", "GUN", "SG", "GR", "LAS", "NIN"};
		const int Weapon = std::clamp(pTasChar->GetActiveWeapon(), 0, NUM_WEAPONS - 1);
		pWeaponName = s_apWeaponNames[Weapon];
		const int Zone = pTasChar->GetOverriddenTuneZone();
		const CTuningParams *pTuning = &GameClient()->m_GameWorld.TuningList()[std::clamp(Zone, 0, TuneZone::NUM - 1)];
		if(pTuning != nullptr)
		{
			const int FireDelayTicks = maximum(0, round_to_int(pTuning->GetWeaponFireDelay(Weapon) * Client()->GameTickSpeed()));
			Cooldown = maximum(0, pTasChar->GetAttackTick() + FireDelayTicks - (m_pWorld != nullptr ? m_pWorld->m_GameTick : Client()->GameTick(g_Config.m_ClDummy)));
		}
	}

	char aLine2[64];
	str_format(aLine2, sizeof(aLine2), "%s CD:%d", pWeaponName, Cooldown);

	char aLine3[96];
	str_format(
		aLine3,
		sizeof(aLine3),
		"D:%d J:%d H:%d F:%d",
		pHudInput != nullptr ? pHudInput->m_Direction : 0,
		pHudInput != nullptr && pHudInput->m_Jump ? 1 : 0,
		pHudInput != nullptr && pHudInput->m_Hook ? 1 : 0,
		pHudInput != nullptr && pHudInput->m_Fire ? 1 : 0);

	TextRender()->TextColor(m_Recording ? ColorRGBA(1.0f, 0.35f, 0.35f, 1.0f) : ColorRGBA(0.65f, 0.9f, 1.0f, 1.0f));
	TextRender()->Text(X + 4.0f, Y + 4.0f, 5.0f, aLine1, -1.0f);
	TextRender()->TextColor(1.0f, 1.0f, 1.0f, 0.95f);
	TextRender()->Text(X + 4.0f, Y + 16.0f, 4.8f, aLine2, -1.0f);
	TextRender()->Text(X + 4.0f, Y + 27.0f, 4.6f, aLine3, -1.0f);
	if(pTasChar != nullptr)
	{
		char aLine4[96];
		str_format(aLine4, sizeof(aLine4), "G:%d FR:%d", pTasChar->IsGrounded() ? 1 : 0, pTasChar->m_FreezeTime);
		TextRender()->Text(X + 4.0f, Y + 38.0f, 4.6f, aLine4, -1.0f);
	}
	if(m_Paused)
		TextRender()->Text(X + 4.0f, Y + 50.0f, 4.6f, "Paused", -1.0f);
	TextRender()->TextColor(TextRender()->DefaultTextColor());
}

void CPastaTas::OnRender()
{
	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;

	if((m_Recording || m_Playing) && !m_WorldInitialized)
		EnsureWorldInitialized();

	if((m_Recording || m_Playing) && m_WorldInitialized)
	{
		const float Delta = std::clamp(Client()->RenderFrameTime(), 0.0f, 0.1f);
		const bool Rewinding = g_Config.m_PastaTasRewind != 0;
		const bool Forwarding = g_Config.m_PastaTasForward != 0;
		bool DidSeek = false;

		if(Rewinding)
		{
			const float SeekRate = maximum(1.0f, m_Playing ? GetStepRate() : (float)std::clamp(g_Config.m_PastaTasTps, 1, 50));
			m_Accumulator -= Delta * SeekRate;
			while(m_Accumulator <= 0.0)
			{
				m_Accumulator += 1.0;
				if(m_Playing)
					StepPlayback(-1, 1);
				else
					RewindRecording(1);
			}
			DidSeek = true;
			if(g_Config.m_PastaTickControlPause)
				g_Config.m_PastaTasPause = 1;
		}
		else if(Forwarding)
		{
			const float SeekRate = 50.0f;
			m_Accumulator += Delta * SeekRate;
			while(m_Accumulator >= 1.0)
			{
				m_Accumulator -= 1.0;
				if(m_Playing)
					StepPlayback(1, 1);
				else
					StepForwardRecording();
			}
			DidSeek = true;
			if(g_Config.m_PastaTickControlPause)
				g_Config.m_PastaTasPause = 1;
		}

		if(!DidSeek)
			m_Accumulator += Delta * GetStepRate();

		while(!DidSeek && !m_Paused && m_Accumulator >= 1.0)
		{
			m_Accumulator -= 1.0;
			if(m_Recording && m_HaveObservedInput)
			{
				CNetObj_PlayerInput RecordedInput = m_ObservedInput;
				if(m_LatchedJump)
					RecordedInput.m_Jump = 1;
				TickWorld(RecordedInput, m_ObservedMouse);
				const int TasTick = m_pWorld != nullptr ? m_pWorld->m_GameTick : (m_LastRecordTasTick + 1);
				if(TasTick != m_LastRecordTasTick)
				{
					vec2 RecordedPos = m_ObservedPos;
					const int LocalId = GameClient()->m_aLocalIds[g_Config.m_ClDummy];
					if(m_pWorld != nullptr && LocalId >= 0)
					{
						if(CCharacter *pRecordedChar = m_pWorld->GetCharacterById(LocalId))
						{
							RecordedPos = pRecordedChar->GetPos();
							m_vRecordedWorldTicks.push_back(m_pWorld->m_GameTick);
							m_vRecordedCores.push_back(pRecordedChar->GetCore());
							m_vRecordedPositions.push_back(pRecordedChar->GetPos());
							m_vRecordedPrevPositions.push_back(pRecordedChar->m_PrevPos);
							m_vRecordedFreezeTimes.push_back(pRecordedChar->m_FreezeTime);
							m_vRecordedReloadTimers.push_back(pRecordedChar->GetReloadTimer());
						}
					}
					m_vFrames.push_back({TasTick, RecordedInput, m_ObservedMouse, RecordedPos});
					m_LastRecordTasTick = TasTick;
					m_LatchedJump = false;
					const int64_t AutoSaveInterval = (int64_t)maximum(1, g_Config.m_PastaAutoSaveReplayInterval) * time_freq();
					if(g_Config.m_PastaAutoSaveReplay && time_get() > m_LastAutoSave + AutoSaveInterval)
					{
						char aPath[IO_MAX_PATH_LENGTH];
						if(GetReplaySavePath(aPath, sizeof(aPath), true))
							SaveReplayToPath(aPath);
						m_LastAutoSave = time_get();
					}
				}
			}
			else if(m_Playing && !m_vFrames.empty())
			{
				const SFrame &Frame = m_vFrames[std::clamp(m_PlayCursor, 0, (int)m_vFrames.size() - 1)];
				TickWorld(Frame.m_Input, Frame.m_Mouse);
				AdvancePlaybackOneStep();
				if(!m_Playing)
					break;
			}
		}
		m_SmoothIntra = std::clamp((float)m_Accumulator, 0.0f, 1.0f);
		if(m_Recording)
		{
			const bool FrozenNow = IsLocalFrozenInWorld();
			if(FrozenNow && !m_WasFrozenInWorld)
			{
				if(g_Config.m_PastaTasAutoRewind)
				{
					RewindRecording(GetTasStepAmount());
					if(g_Config.m_PastaTickControlPause)
						g_Config.m_PastaTasPause = 1;
				}
				else if(g_Config.m_PastaTasAutoForward)
				{
					AutoForwardRecording();
					if(g_Config.m_PastaTickControlPause)
						g_Config.m_PastaTasPause = 1;
				}
			}
			m_WasFrozenInWorld = IsLocalFrozenInWorld();
		}
		else
		{
			m_WasFrozenInWorld = false;
		}

		if(m_Recording && m_HaveObservedInput)
		{
			CNetObj_PlayerInput PredictedObservedInput = m_ObservedInput;
			if(m_LatchedJump)
				PredictedObservedInput.m_Jump = 1;
			UpdatePredictedWorld(PredictedObservedInput, m_ObservedMouse);
		}
		else if(m_Playing && !m_vFrames.empty())
		{
			const SFrame &Frame = m_vFrames[std::clamp(m_PlayCursor, 0, (int)m_vFrames.size() - 1)];
			UpdatePredictedWorld(Frame.m_Input, Frame.m_Mouse);
		}
	}

	Graphics()->MapScreenToInterface(GameClient()->m_Camera.m_Center.x, GameClient()->m_Camera.m_Center.y, GameClient()->m_Camera.m_Zoom);
	RenderStartEndPos();
	RenderReplayPath();
	RenderPredictionPath();
	RenderHud();
}
