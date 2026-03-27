#include "pasta_misc.h"

#include <base/math.h>
#include <base/str.h>

#include <engine/shared/config.h>

#include <generated/protocol.h>

#include <game/client/components/chat.h>
#include <game/client/components/emoticon.h>
#include <game/client/components/voting.h>
#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>
#include <game/teamscore.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
constexpr int64_t EMOTE_SPAM_INTERVAL_MS = 10;
constexpr int64_t MASS_MENTION_INTERVAL_MS = 5000;
constexpr int64_t AUTO_TEAM_INTERVAL_MS = 2000;
constexpr int64_t AUTO_VOTE_KICK_INTERVAL_MS = 9000;

int64_t MsToTimeFreq(int64_t Ms)
{
	return Ms * time_freq() / 1000;
}

bool IsUsablePlayer(const CGameClient *pGameClient, int ClientId)
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return false;
	if(ClientId == pGameClient->m_Snap.m_LocalClientId)
		return false;
	if(!pGameClient->m_Snap.m_aCharacters[ClientId].m_Active || !pGameClient->m_Snap.m_apPlayerInfos[ClientId])
		return false;
	return pGameClient->m_Snap.m_apPlayerInfos[ClientId]->m_Team != TEAM_SPECTATORS;
}

void ToLowerAscii(const char *pSrc, char *pDst, int DstSize)
{
	if(DstSize <= 0)
		return;

	int Cursor = 0;
	while(pSrc[Cursor] != '\0' && Cursor < DstSize - 1)
	{
		const char Chr = pSrc[Cursor];
		pDst[Cursor] = Chr >= 'A' && Chr <= 'Z' ? Chr - 'A' + 'a' : Chr;
		++Cursor;
	}
	pDst[Cursor] = '\0';
}

bool ContainsAnyKeyword(const char *pText, const std::array<const char *, 10> &aKeywords)
{
	char aLower[128];
	ToLowerAscii(pText, aLower, sizeof(aLower));
	for(const char *pKeyword : aKeywords)
	{
		if(str_find_nocase(aLower, pKeyword) != nullptr)
			return true;
	}
	return false;
}

bool IsFrozenLocalCharacter(CGameClient *pGameClient)
{
	const int LocalClientId = pGameClient->m_Snap.m_LocalClientId;
	if(LocalClientId < 0 || !pGameClient->m_Snap.m_aCharacters[LocalClientId].m_Active)
		return false;

	CCharacter *pLocalCharacter = pGameClient->m_PredictedWorld.GetCharacterById(LocalClientId);
	return pLocalCharacter != nullptr && (pLocalCharacter->m_FreezeTime > 0 || pLocalCharacter->Core()->m_DeepFrozen || pLocalCharacter->Core()->m_LiveFrozen);
}
}

void CPastaMisc::OnReset()
{
	const int64_t Now = time_get();
	m_NextAutoTeamTry = Now;
	m_LastEmoteSpam = Now;
	m_LastMassMentionSpam = Now;
	m_LastAutoVoteKick = Now;
	m_LastIdSteal = Now;
	m_LastChatRepeat = 0;
	m_WasFrozen = false;
	std::fill(std::begin(m_aModDetected), std::end(m_aModDetected), false);
	std::fill(std::begin(m_aWarnDetected), std::end(m_aWarnDetected), false);
}

void CPastaMisc::OnShutdown()
{
	if(m_BenchmarkThread.joinable())
		m_BenchmarkThread.request_stop();
}

void CPastaMisc::OnStateChange(int NewState, int OldState)
{
	if(g_Config.m_PastaRandomTimeoutSeed && NewState == IClient::STATE_CONNECTING && OldState < IClient::STATE_CONNECTING)
		Client()->GenerateTimeoutSeed();

	if(NewState != IClient::STATE_ONLINE)
	{
		if(m_BenchmarkThread.joinable())
			m_BenchmarkThread.request_stop();
		m_BenchmarkToggleLatch = false;
		g_Config.m_PastaPredictionBenchmark = 0;
		OnReset();
		return;
	}

	const int64_t Now = time_get();
	m_NextAutoTeamTry = Now;
	m_LastEmoteSpam = Now;
	m_LastMassMentionSpam = Now;
	m_LastAutoVoteKick = Now;
	m_LastIdSteal = Now;
}

void CPastaMisc::OnUpdate()
{
	if(Client()->State() != IClient::STATE_ONLINE)
		return;

	const int64_t Now = time_get();
	UpdateDummyFly();
	UpdateAutoTeam(Now);
	UpdateEmoteSpam(Now);
	UpdateAutoVote(Now);
	UpdateAutoVoteKick(Now);
	UpdateMassMentionSpam(Now);
	UpdateIdStealer(Now);
	UpdateKillOnFreeze();
	UpdateModDetector();
	UpdatePredictionBenchmark();
}

void CPastaMisc::UpdateAutoTeam(int64_t Now)
{
	if(!g_Config.m_PastaAutoTeam || GameClient()->m_Snap.m_LocalClientId < 0)
		return;
	if(GameClient()->m_Snap.m_apPlayerInfos[GameClient()->m_Snap.m_LocalClientId] &&
		GameClient()->m_Snap.m_apPlayerInfos[GameClient()->m_Snap.m_LocalClientId]->m_Team == TEAM_SPECTATORS)
		return;
	if(GameClient()->m_Teams.Team(GameClient()->m_Snap.m_LocalClientId) != TEAM_FLOCK)
		return;
	if(Now < m_NextAutoTeamTry)
		return;

	GameClient()->m_Chat.SendChat(0, "/team 999999");
	m_NextAutoTeamTry = Now + MsToTimeFreq(AUTO_TEAM_INTERVAL_MS);
}

void CPastaMisc::UpdateDummyFly()
{
	if(g_Config.m_PastaDummyFly)
		g_Config.m_ClDummyHammer = 1;
}

void CPastaMisc::UpdateEmoteSpam(int64_t Now)
{
	if(!g_Config.m_PastaEmoteSpam || Now < m_LastEmoteSpam + MsToTimeFreq(EMOTE_SPAM_INTERVAL_MS))
		return;

	GameClient()->m_Emoticon.Emote(rand() % NUM_EMOTICONS);
	m_LastEmoteSpam = Now;
}

void CPastaMisc::UpdateAutoVote(int64_t Now)
{
	(void)Now;
	if(!g_Config.m_PastaAutoVote || !GameClient()->m_Voting.IsVoting() || GameClient()->m_Voting.TakenChoice() != 0)
		return;

	GameClient()->m_Voting.Vote(g_Config.m_PastaAutoVoteF4 ? -1 : 1);
}

void CPastaMisc::UpdateAutoVoteKick(int64_t Now)
{
	if(!g_Config.m_PastaAutoVoteKick || GameClient()->m_Voting.IsVoting() || Now < m_LastAutoVoteKick + MsToTimeFreq(AUTO_VOTE_KICK_INTERVAL_MS))
		return;

	int TargetId = g_Config.m_PastaAutoVoteKickMode == 1 ? g_Config.m_PastaAutoVoteKickTarget : ChooseIdentityTarget(false);
	if(!IsUsablePlayer(GameClient(), TargetId))
		return;

	GameClient()->m_Voting.CallvoteKick(TargetId, g_Config.m_PastaAutoVoteKickReason, false);
	m_LastAutoVoteKick = Now;
}

void CPastaMisc::BuildMentionSpam(char *pBuf, int BufSize) const
{
	pBuf[0] = '\0';

	std::vector<int> vCandidates;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		if(IsUsablePlayer(GameClient(), ClientId) && GameClient()->m_aClients[ClientId].m_aName[0] != '\0')
			vCandidates.push_back(ClientId);
	}

	const int MentionCount = minimum<int>(3, vCandidates.size());
	for(int Index = 0; Index < MentionCount; ++Index)
	{
		const int PickedIndex = rand() % vCandidates.size();
		const int ClientId = vCandidates[PickedIndex];
		if(pBuf[0] != '\0')
			str_append(pBuf, " ", BufSize);
		str_append(pBuf, GameClient()->m_aClients[ClientId].m_aName, BufSize);
		vCandidates.erase(vCandidates.begin() + PickedIndex);
	}

	if(pBuf[0] != '\0')
		str_append(pBuf, " ", BufSize);
	str_append(pBuf, "Wanna become good? Try pastateam.com", BufSize);
}

void CPastaMisc::UpdateMassMentionSpam(int64_t Now)
{
	if(!g_Config.m_PastaMassMentionSpam || Now < m_LastMassMentionSpam + MsToTimeFreq(MASS_MENTION_INTERVAL_MS))
		return;

	char aBuf[256];
	BuildMentionSpam(aBuf, sizeof(aBuf));
	if(aBuf[0] == '\0')
		return;

	GameClient()->m_Chat.SendChat(0, aBuf);
	m_LastMassMentionSpam = Now;
}

int CPastaMisc::ChooseIdentityTarget(bool Closest) const
{
	std::vector<int> vCandidates;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		if(IsUsablePlayer(GameClient(), ClientId))
			vCandidates.push_back(ClientId);
	}

	if(vCandidates.empty())
		return -1;

	if(!Closest)
		return vCandidates[rand() % vCandidates.size()];

	const CCharacter *pLocalCharacter = GameClient()->m_PredictedWorld.GetCharacterById(GameClient()->m_Snap.m_LocalClientId);
	if(pLocalCharacter == nullptr)
		return vCandidates[rand() % vCandidates.size()];

	int ClosestClientId = -1;
	float ClosestDistance = 1e9f;
	for(const int ClientId : vCandidates)
	{
		const float Distance = distance(pLocalCharacter->GetPos(), GameClient()->m_aClients[ClientId].m_RenderPos);
		if(Distance < ClosestDistance)
		{
			ClosestDistance = Distance;
			ClosestClientId = ClientId;
		}
	}

	return ClosestClientId;
}

void CPastaMisc::CopyIdentityFrom(int ClientId)
{
	if(!IsUsablePlayer(GameClient(), ClientId))
		return;

	const auto &Target = GameClient()->m_aClients[ClientId];
	bool Dirty = false;

	if(g_Config.m_PastaIdStealerName)
	{
		str_copy(g_Config.m_PlayerName, Target.m_aName);
		Dirty = true;
	}
	if(g_Config.m_PastaIdStealerClan)
	{
		str_copy(g_Config.m_PlayerClan, Target.m_aClan);
		Dirty = true;
	}
	if(g_Config.m_PastaIdStealerSkin)
	{
		str_copy(g_Config.m_ClPlayerSkin, Target.m_aSkinName);
		g_Config.m_ClPlayerUseCustomColor = Target.m_UseCustomColor;
		g_Config.m_ClPlayerColorBody = Target.m_ColorBody;
		g_Config.m_ClPlayerColorFeet = Target.m_ColorFeet;
		Dirty = true;
	}
	if(g_Config.m_PastaIdStealerFlag)
	{
		g_Config.m_PlayerCountry = Target.m_Country;
		Dirty = true;
	}
	if(g_Config.m_PastaIdStealerEmote)
	{
		g_Config.m_ClPlayerDefaultEyes = std::clamp(Target.m_Emoticon, 0, NUM_EMOTES - 1);
		Dirty = true;
	}

	if(Dirty)
		GameClient()->SendInfo(false);
}

void CPastaMisc::UpdateIdStealer(int64_t Now)
{
	if(!g_Config.m_PastaIdStealer || Now < m_LastIdSteal + MsToTimeFreq((int64_t)g_Config.m_PastaIdStealerSpeed * 1000))
		return;

	const int TargetId = ChooseIdentityTarget(g_Config.m_PastaIdStealerClosest != 0);
	if(TargetId >= 0)
		CopyIdentityFrom(TargetId);
	m_LastIdSteal = Now;
}

void CPastaMisc::UpdateKillOnFreeze()
{
	const bool Frozen = IsFrozenLocalCharacter(GameClient());
	if(g_Config.m_PastaKillOnFreeze && Frozen && !m_WasFrozen)
		GameClient()->m_Chat.SendChat(0, "/kill");
	m_WasFrozen = Frozen;
}

bool CPastaMisc::DetectPlayerByName(int ClientId, bool WarnOnly) const
{
	if(!IsUsablePlayer(GameClient(), ClientId))
		return false;

	static const std::array<const char *, 10> s_aModKeywords = {
		"moderator",
		"[mod]",
		"[admin]",
		" admin",
		"helper",
		"staff",
		"guard",
		"police",
		"mod_",
		"mod "};
	static const std::array<const char *, 10> s_aWarnKeywords = {
		"report",
		"screenshare",
		"record",
		"rec ",
		"demo",
		"clip",
		"cheater",
		"ban",
		"proof",
		"ticket"};

	const auto &ClientData = GameClient()->m_aClients[ClientId];
	return WarnOnly ? (ContainsAnyKeyword(ClientData.m_aName, s_aWarnKeywords) || ContainsAnyKeyword(ClientData.m_aClan, s_aWarnKeywords)) :
			  (ContainsAnyKeyword(ClientData.m_aName, s_aModKeywords) || ContainsAnyKeyword(ClientData.m_aClan, s_aModKeywords));
}

void CPastaMisc::UpdateModDetector()
{
	if(!g_Config.m_PastaModDetector)
		return;

	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		if(!IsUsablePlayer(GameClient(), ClientId))
			continue;

		if(g_Config.m_PastaModDetectorNames && !m_aModDetected[ClientId] && DetectPlayerByName(ClientId, false))
		{
			m_aModDetected[ClientId] = true;

			char aBuf[256];
			str_format(aBuf, sizeof(aBuf), "%s joined or is present (id %d)", GameClient()->m_aClients[ClientId].m_aName, ClientId);
			Client()->AddWarning(SWarning("Moderator Detected", aBuf));
			if(g_Config.m_PastaModDetectorLeave)
			{
				Client()->Disconnect();
				return;
			}
		}

		if(g_Config.m_PastaModDetectorWarn && !m_aWarnDetected[ClientId] && DetectPlayerByName(ClientId, true))
		{
			m_aWarnDetected[ClientId] = true;

			char aBuf[256];
			str_format(aBuf, sizeof(aBuf), "%s looks suspicious (id %d)", GameClient()->m_aClients[ClientId].m_aName, ClientId);
			Client()->AddWarning(SWarning("Warn Detected", aBuf));
			if(g_Config.m_PastaModDetectorWarnLeave)
			{
				Client()->Disconnect();
				return;
			}
		}
	}
}

void CPastaMisc::QueueBenchmarkResult(const char *pTitle, const char *pMessage)
{
	std::scoped_lock Lock(m_BenchmarkResultMutex);
	str_copy(m_aBenchmarkTitle, pTitle, sizeof(m_aBenchmarkTitle));
	str_copy(m_aBenchmarkMessage, pMessage, sizeof(m_aBenchmarkMessage));
	m_BenchmarkNotificationReady = true;
}

void CPastaMisc::StartPredictionBenchmark()
{
	if(m_BenchmarkRunning.exchange(true))
		return;

	const int Runs = maximum(1, g_Config.m_PastaPredictionBenchmarkRuns);
	const int Threads = maximum(1, g_Config.m_PastaPredictionBenchmarkThreads);
	m_BenchmarkThread = std::jthread([this, Runs, Threads](std::stop_token StopToken) {
		using clock = std::chrono::steady_clock;
		struct SThreadResult
		{
			uint64_t m_Steps = 0;
			double m_Score = 0.0;
		};

		const auto Worker = [&StopToken, Runs, Threads](int ThreadIndex, SThreadResult &Result) {
			uint32_t Seed = (uint32_t)(0x9e3779b9u * (ThreadIndex + 1));
			for(int Run = ThreadIndex; Run < Runs && !StopToken.stop_requested(); Run += Threads)
			{
				vec2 Pos(Seed % 128, (Seed >> 8) % 128);
				vec2 Vel(0.0f, 0.0f);
				const int Iterations = 60000 + Run * 2000;
				for(int Step = 0; Step < Iterations && !StopToken.stop_requested(); ++Step)
				{
					Seed = Seed * 1664525u + 1013904223u;
					const float InputDir = (float)((int)((Seed >> 29) & 3) - 1);
					const bool Jump = ((Seed >> 7) & 31) == 0;
					Vel.x = std::clamp(Vel.x + InputDir * 0.36f, -12.0f, 12.0f);
					Vel.y = std::clamp(Vel.y + 0.55f - (Jump ? 9.2f : 0.0f), -18.0f, 22.0f);
					Pos += Vel * 0.25f;
					Result.m_Score += length(Pos) + absolute(Vel.x) * 0.2 + absolute(Vel.y) * 0.4;
					++Result.m_Steps;
				}
			}
		};

		const auto Start = clock::now();
		std::vector<SThreadResult> vResults(Threads);
		std::vector<std::thread> vWorkers;
		vWorkers.reserve(maximum(0, Threads - 1));
		for(int ThreadIndex = 1; ThreadIndex < Threads; ++ThreadIndex)
			vWorkers.emplace_back(Worker, ThreadIndex, std::ref(vResults[ThreadIndex]));
		Worker(0, vResults[0]);
		for(std::thread &WorkerThread : vWorkers)
			WorkerThread.join();
		const auto End = clock::now();

		if(StopToken.stop_requested())
		{
			m_BenchmarkRunning = false;
			return;
		}

		uint64_t TotalSteps = 0;
		double TotalScore = 0.0;
		for(const SThreadResult &Result : vResults)
		{
			TotalSteps += Result.m_Steps;
			TotalScore += Result.m_Score;
		}

		const double Ms = std::chrono::duration<double, std::milli>(End - Start).count();
		const double StepsPerSecond = Ms > 0.0 ? (TotalSteps * 1000.0 / Ms) : 0.0;

		char aTitle[128];
		char aMessage[256];
		str_copy(aTitle, "Prediction Benchmark", sizeof(aTitle));
		str_format(aMessage, sizeof(aMessage), "%d runs, %d threads, %.2f ms, %.2f Msteps/s", Runs, Threads, Ms, StepsPerSecond / 1000000.0);
		(void)TotalScore;
		QueueBenchmarkResult(aTitle, aMessage);
		m_BenchmarkRunning = false;
	});
}

void CPastaMisc::UpdatePredictionBenchmark()
{
	if(g_Config.m_PastaPredictionBenchmark && !m_BenchmarkToggleLatch && !m_BenchmarkRunning)
		StartPredictionBenchmark();
	if(!g_Config.m_PastaPredictionBenchmark && m_BenchmarkToggleLatch && m_BenchmarkThread.joinable())
		m_BenchmarkThread.request_stop();
	m_BenchmarkToggleLatch = g_Config.m_PastaPredictionBenchmark != 0;

	if(m_BenchmarkNotificationReady.exchange(false))
	{
		char aTitle[128];
		char aMessage[256];
		{
			std::scoped_lock Lock(m_BenchmarkResultMutex);
			str_copy(aTitle, m_aBenchmarkTitle, sizeof(aTitle));
			str_copy(aMessage, m_aBenchmarkMessage, sizeof(aMessage));
		}
		Client()->AddWarning(SWarning(aTitle, aMessage));
		g_Config.m_PastaPredictionBenchmark = 0;
		m_BenchmarkToggleLatch = false;
	}
}

void CPastaMisc::BuildAlternatingCase(char *pBuf, int BufSize, const char *pSrc) const
{
	pBuf[0] = '\0';
	bool Upper = true;
	for(const char *pChr = pSrc; *pChr != '\0'; ++pChr)
	{
		char aChar[2] = {*pChr, '\0'};
		if(*pChr >= 'a' && *pChr <= 'z')
			aChar[0] = Upper ? (*pChr - 'a' + 'A') : *pChr;
		else if(*pChr >= 'A' && *pChr <= 'Z')
			aChar[0] = Upper ? *pChr : (*pChr - 'A' + 'a');

		if((*pChr >= 'a' && *pChr <= 'z') || (*pChr >= 'A' && *pChr <= 'Z'))
			Upper = !Upper;

		str_append(pBuf, aChar, BufSize);
	}
}

void CPastaMisc::OnMessage(int MsgType, void *pRawMsg)
{
	if(Client()->State() != IClient::STATE_ONLINE || GameClient()->m_Snap.m_LocalClientId < 0)
		return;

	if(MsgType == NETMSGTYPE_SV_CHAT)
	{
		CNetMsg_Sv_Chat *pMsg = (CNetMsg_Sv_Chat *)pRawMsg;
		if(!g_Config.m_PastaChatRepeater || pMsg->m_ClientId < 0 || pMsg->m_ClientId == GameClient()->m_Snap.m_LocalClientId)
			return;
		if(str_length(pMsg->m_pMessage) < g_Config.m_PastaChatRepeaterLength)
			return;
		if(time_get() < m_LastChatRepeat + time_freq())
			return;

		char aBuf[256];
		BuildAlternatingCase(aBuf, sizeof(aBuf), pMsg->m_pMessage);
		GameClient()->m_Chat.SendChat(0, aBuf);
		m_LastChatRepeat = time_get();
	}
	else if(MsgType == NETMSGTYPE_SV_KILLMSG && g_Config.m_PastaKillsay)
	{
		CNetMsg_Sv_KillMsg *pMsg = (CNetMsg_Sv_KillMsg *)pRawMsg;
		static const std::array<const char *, 6> s_aDeathsay = {
			"lagged, totally",
			"nt, i guess",
			"that freeze looked personal",
			"reporting my keyboard",
			"unlucky spawn of destiny",
			"i meant to do that"};
		static const std::array<const char *, 6> s_aKillsay = {
			"pastad",
			"skill issue",
			"outplayed by pasta",
			"too slow",
			"frozen and folded",
			"gg go next"};

		if(pMsg->m_Victim == GameClient()->m_Snap.m_LocalClientId)
			GameClient()->m_Chat.SendChat(0, s_aDeathsay[rand() % s_aDeathsay.size()]);
		else if(pMsg->m_Killer == GameClient()->m_Snap.m_LocalClientId && pMsg->m_Victim != GameClient()->m_Snap.m_LocalClientId)
			GameClient()->m_Chat.SendChat(0, s_aKillsay[rand() % s_aKillsay.size()]);
	}
}
