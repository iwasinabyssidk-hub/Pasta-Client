#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_PASTA_MISC_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_PASTA_MISC_H

#include <engine/shared/protocol.h>

#include <game/client/component.h>

#include <atomic>
#include <mutex>
#include <thread>

class CPastaMisc : public CComponent
{
	int64_t m_NextAutoTeamTry = 0;
	int64_t m_LastEmoteSpam = 0;
	int64_t m_LastMassMentionSpam = 0;
	int64_t m_LastAutoVoteKick = 0;
	int64_t m_LastIdSteal = 0;
	int64_t m_LastChatRepeat = 0;
	bool m_aModDetected[MAX_CLIENTS] = {};
	bool m_aWarnDetected[MAX_CLIENTS] = {};
	bool m_WasFrozen = false;
	bool m_BenchmarkToggleLatch = false;
	std::atomic_bool m_BenchmarkRunning = false;
	std::atomic_bool m_BenchmarkNotificationReady = false;
	std::jthread m_BenchmarkThread;
	std::mutex m_BenchmarkResultMutex;
	char m_aBenchmarkTitle[128] = "";
	char m_aBenchmarkMessage[256] = "";

public:
	CPastaMisc() = default;
	int Sizeof() const override { return sizeof(*this); }

	void OnReset() override;
	void OnShutdown() override;
	void OnStateChange(int NewState, int OldState) override;
	void OnUpdate() override;
	void OnMessage(int MsgType, void *pRawMsg) override;

private:
	void UpdateAutoTeam(int64_t Now);
	void UpdateDummyFly();
	void UpdateEmoteSpam(int64_t Now);
	void UpdateAutoVote(int64_t Now);
	void UpdateAutoVoteKick(int64_t Now);
	void UpdateMassMentionSpam(int64_t Now);
	void UpdateIdStealer(int64_t Now);
	void UpdateKillOnFreeze();
	void UpdateModDetector();
	void UpdatePredictionBenchmark();
	void StartPredictionBenchmark();
	void QueueBenchmarkResult(const char *pTitle, const char *pMessage);

	int ChooseIdentityTarget(bool Closest) const;
	void CopyIdentityFrom(int ClientId);
	void BuildMentionSpam(char *pBuf, int BufSize) const;
	void BuildAlternatingCase(char *pBuf, int BufSize, const char *pSrc) const;
	bool DetectPlayerByName(int ClientId, bool WarnOnly) const;
};

#endif
