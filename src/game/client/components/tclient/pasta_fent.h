#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_PASTA_FENT_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_PASTA_FENT_H

#include <generated/protocol.h>

#include <game/client/component.h>
#include <game/client/prediction/gameworld.h>

#include <base/vmath.h>

#include <vector>

class CPastaFent : public CComponent
{
public:
	struct SFrame
	{
		int m_Tick = 0;
		CNetObj_PlayerInput m_Input{};
		vec2 m_Mouse{};
		vec2 m_Pos{};
	};

	int Sizeof() const override { return sizeof(*this); }
	void OnReset() override;
	void OnStateChange(int NewState, int OldState) override;
	void OnUpdate() override;
	void OnRender() override;

	bool IsRunning() const { return m_Running; }
	bool HasFinished() const { return m_Finished; }
	bool SaveCurrentReplay();
	vec2 GetInterpolatedRenderPos() const;
	bool GetRealtimeInput(CNetObj_PlayerInput &OutInput, vec2 &OutMouse) const;

private:
	CGameWorld *m_pWorld = nullptr;
	CGameWorld *m_pWorldPredicted = nullptr;
	std::vector<SFrame> m_vFrames;
	std::vector<vec2> m_vTrailPositions;
	std::vector<SFrame> m_vPlannedFrames;
	std::vector<vec2> m_vBestPreviewPath;
	std::vector<vec2> m_vFinishTiles;
	std::vector<int> m_vFinishTileIndices;
	std::vector<vec2> m_vHookTiles;
	std::vector<int8_t> m_vFieldDirX;
	std::vector<int8_t> m_vFieldDirY;
	int m_FieldWidth = 0;
	int m_FieldHeight = 0;
	int m_LastPlannedVariant = 0;
	int m_PlanningSeed = 0;
	bool m_Running = false;
	bool m_Finished = false;
	bool m_Saved = false;
	bool m_WorldInitialized = false;
	bool m_TilesCached = false;
	bool m_FieldReady = false;
	bool m_Planning = false;
	bool m_HaveSafePlan = false;
	double m_Accumulator = 0.0;
	int64_t m_LastUpdateTime = 0;
	int m_StartDelayFrames = 0;
	int m_LastJumpTick = -1000;
	int m_StuckTicks = 0;
	int m_NoSafePlanStreak = 0;
	int m_ExecCursor = 0;
	int m_ReplanCountdown = 0;
	int m_PlanningTotalCandidates = 0;
	int m_PlanningDoneCandidates = 0;
	bool m_SpectatePauseSent = false;
	float m_BestCandidateScore = -1e9f;
	float m_LastAcceptedScore = -1e9f;
	int m_LastChosenVariant = -1;
	int m_RepeatedChoiceCount = 0;
	int m_ForcedMoveTicks = 0;
	int m_ForcedMoveDir = 0;
	int m_RecentVariants[6] = {-1, -1, -1, -1, -1, -1};
	vec2 m_RecentVariantPositions[6] = {vec2(0, 0), vec2(0, 0), vec2(0, 0), vec2(0, 0), vec2(0, 0), vec2(0, 0)};
	int m_BannedVariantsAtLoop[3] = {-1, -1, -1};
	vec2 m_BannedLoopPos = vec2(0.0f, 0.0f);
	vec2 m_LastChoiceStartPos = vec2(0.0f, 0.0f);
	int m_BannedVariant = -1;
	vec2 m_BannedVariantPos = vec2(0.0f, 0.0f);
	bool m_HasBannedOpeningInput = false;
	CNetObj_PlayerInput m_BannedOpeningInput{};
	vec2 m_BannedOpeningPos = vec2(0.0f, 0.0f);
	bool m_HasBannedTransition = false;
	vec2 m_BannedTransitionStartPos = vec2(0.0f, 0.0f);
	vec2 m_BannedTransitionEndPos = vec2(0.0f, 0.0f);
	vec2 m_LastRenderPos = vec2(0.0f, 0.0f);
	vec2 m_CurRenderPos = vec2(0.0f, 0.0f);
	bool m_HasRealtimeInput = false;
	CNetObj_PlayerInput m_RealtimeInput{};
	vec2 m_RealtimeMouse = vec2(0.0f, 0.0f);

	void StartRun();
	void StopRun(bool KeepResults);
	void EnsureWorldInitialized();
	void ShutdownWorld();
	void ResetWorldFromPredicted();
	void ResetWorldFromSnapshot();
	void CacheTiles();
	void BuildFlowField();
	void BeginPlanning();
	void RunPlanningSlice();
	void AdvanceOneStep();
	bool IsFinishReached() const;
	void TickWorld(const CNetObj_PlayerInput &Input, vec2 Mouse);
	bool RewindExecutedFrames(int Steps);
	bool SaveReplay() const;
	void RenderPath();
	void RenderMarker();
	void RenderFinishTiles();
	void RenderPathfinding();
};

#endif
