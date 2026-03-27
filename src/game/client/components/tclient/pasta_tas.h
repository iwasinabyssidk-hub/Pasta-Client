#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_PASTA_TAS_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_PASTA_TAS_H

#include <generated/protocol.h>

#include <game/client/component.h>
#include <game/client/prediction/gameworld.h>

#include <base/vmath.h>

#include <vector>

class CPastaTas : public CComponent
{
public:
	struct SFrame
	{
		int m_Tick = 0;
		CNetObj_PlayerInput m_Input{};
		vec2 m_Mouse{};
		vec2 m_Pos{};
	};

	CPastaTas() = default;
	int Sizeof() const override { return sizeof(*this); }

	void OnReset() override;
	void OnStateChange(int NewState, int OldState) override;
	void OnUpdate() override;
	void OnRender() override;

	void ObserveInput(const CNetObj_PlayerInput &Input, vec2 Pos);
	bool GetPlaybackInput(CNetObj_PlayerInput &Out) const;
	bool HasRenderMouseOverride() const;
	vec2 GetRenderMousePos() const;
	bool IsWorldActive() const;
	bool IsWorldInitialized() const { return m_WorldInitialized; }
	vec2 GetInterpolatedPlayerPos() const;

	void SaveReplay();
	void LoadSelectedReplay();
	void ValidateReplay();
	void ReportReplayTime();
	void RemoveUseless();
	void SyncNow();

	bool IsRecording() const { return m_Recording; }
	bool IsPlaying() const { return m_Playing; }
	bool IsPaused() const { return m_Paused; }
	int NumFrames() const { return (int)m_vFrames.size(); }
	int PlaybackCursor() const { return m_PlayCursor; }
	float GetStepRate() const;
	void RenderWorldCharacters();

private:
	std::vector<SFrame> m_vFrames;
	std::vector<int> m_vRecordedWorldTicks;
	std::vector<CCharacterCore> m_vRecordedCores;
	std::vector<vec2> m_vRecordedPositions;
	std::vector<vec2> m_vRecordedPrevPositions;
	std::vector<int> m_vRecordedFreezeTimes;
	std::vector<int> m_vRecordedReloadTimers;
	CGameWorld *m_pWorld = nullptr;
	CGameWorld *m_pWorldPredicted = nullptr;
	int m_PlayCursor = 0;
	int m_LastRecordTasTick = -1;
	int64_t m_LastAutoSave = 0;
	bool m_Recording = false;
	bool m_Playing = false;
	bool m_Paused = false;
	bool m_WorldInitialized = false;
	bool m_HaveObservedInput = false;
	bool m_LatchedJump = false;
	bool m_WasFrozenInWorld = false;
	double m_Accumulator = 0.0;
	int64_t m_LastSeekTime = 0;
	float m_SmoothIntra = 0.0f;
	int m_PlaybackStepRateOverride = 0;
	CNetObj_PlayerInput m_ObservedInput{};
	vec2 m_ObservedMouse{};
	vec2 m_ObservedPos{};

	void ClearReplay(bool KillPlayer);
	void StartRecording();
	void StopRecording();
	bool StartPlayback();
	void StopPlayback();
	void StepPlayback(int Direction, int Amount);
	void AdvancePlaybackOneStep();
	void EnsureWorldInitialized();
	void ShutdownWorld();
	void ResetWorldFromPredicted();
	void ResimulatePlaybackWorld();
	void RewindRecording(int Amount);
	void StepForwardRecording();
	void AutoForwardRecording();
	bool IsLocalFrozenInWorld() const;
	void TickWorld(const CNetObj_PlayerInput &Input, vec2 Mouse);
	void UpdatePredictedWorld(const CNetObj_PlayerInput &Input, vec2 Mouse);
	bool SaveReplayToPath(const char *pPath) const;
	bool LoadReplayFromPath(const char *pPath);
	bool FindReplayPath(char *pBuf, int BufSize, bool ExactStemOnly) const;
	bool GetReplaySavePath(char *pBuf, int BufSize, bool AutoSave) const;
	void PushTasWarning(const char *pTitle, const char *pMessage) const;
	void RenderTasWorld();
	void RenderStartEndPos();
	void RenderReplayPath();
	void RenderPredictionPath();
	void RenderHud();
};

#endif
