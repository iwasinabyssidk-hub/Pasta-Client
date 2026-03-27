#ifndef GAME_CLIENT_COMPONENTS_PASTA_AVOIDFREEZE_H
#define GAME_CLIENT_COMPONENTS_PASTA_AVOIDFREEZE_H

#include <base/vmath.h>
#include <engine/console.h>
#include <engine/shared/jobs.h>
#include <game/client/component.h>
#include <generated/protocol.h>
#include <game/client/prediction/gameworld.h>
#include <engine/client/enums.h>
#include <mutex>
#include <vector>

class CAvoidFreeze : public CComponent
{
public:
	virtual int Sizeof() const override { return sizeof(*this); }
	virtual void OnReset();

	int PastaHookAssistTicks() const;
	int PastaDirectionSensitivityStep() const;
	float PastaDirectionSensitivityFactor() const;
	int PastaMaxAvoidAttempts() const;
	int PastaMaxAvoidAttemptsPerDirection() const;
	bool PastaGetFreeze(vec2 Pos, int FreezeTime) const;
	bool PastaAutomationAllowed() const;
	void PastaAvoidFreeze();
	void PastaHookAssist();
	bool PastaIsAvoidCooldownElapsed(int64_t CurrentTime) const;
	bool IsFreezeTile(int Tile) const;
	bool HasFreezeTile(vec2 Pos) const;
	bool IsDangerousMapIndex(int MapIndex, bool DetectFreeze, bool DetectDeath, bool DetectTele);
	bool PastaTryMove(const CNetObj_PlayerInput &BaseInput, int Direction, int CheckTicks);
	bool PastaTryAvoidFreeze(int LocalPlayerId);
	int PastaAvoidPredictTicks() const;
	void PastaUpdateAvoidCooldown(int64_t CurrentTime);
	bool PastaPredictFreeze(const CNetObj_PlayerInput &Input, int Ticks, int *pDangerTick = nullptr, float *pDangerDistance = nullptr) const;
	bool PastaPredictFreezeMT(const std::vector<CNetObj_PlayerInput> &vInputs, int Ticks,
		int *pOutDangerTick = nullptr, float *pOutDangerDistance = nullptr, int *pOutInputIndex = nullptr);
	bool HasDangerousTile(vec2 Pos, bool DetectFreeze, bool DetectDeath, bool DetectTele);
	bool HasFreezeAbove(vec2 Pos, int Tiles);
	int DetectFreezeSide(vec2 Pos);
	bool ShouldAutoJumpSave(CCharacter *pCharacter);
	bool HasPlayerNearAim(const CCharacter *pLocalCharacter, vec2 AimDir, float MaxDistance, float MaxAngleRadians);
	bool HasImmediateSideFreeze(const CCharacter *pCharacter, int Direction);
	bool IsSolidTileAt(int TileX, int TileY);
	bool FindBlatantHookTarget(vec2 Pos, vec2 Vel, vec2 GoalPos, float HookLength, bool DangerAhead, vec2 &OutTarget);
	bool HasImpendingFreeze(const CCharacter *pCharacter, int Ticks);
	bool HasBlatantDangerAhead(const CCharacter *pCharacter, int Direction, int CheckTicks, bool DetectFreeze, bool DetectDeath, bool DetectTele, bool DetectUnfreeze);
	int GetBlatantDangerTick(const CCharacter *pCharacter, int Direction, int CheckTicks, bool DetectFreeze, bool DetectDeath, bool DetectTele, bool DetectUnfreeze);
	int SimulateBlatantFreezeTick(const CNetObj_PlayerInput &BaseInput, int Ticks, bool DetectFreeze, bool DetectDeath, bool DetectTele, bool DetectUnfreeze);
	int SimulateBlatantFreezeTickMT(const CNetObj_PlayerInput &BaseInput, int Ticks, bool DetectFreeze, bool DetectDeath, bool DetectTele, bool DetectUnfreeze, const std::vector<CNetObj_PlayerInput> &vInputs);
	bool IsBlatantFrozenAtPos(vec2 Pos);

	bool m_AvoidForcing[NUM_DUMMIES];
	bool m_BlatantTrackPointInit[NUM_DUMMIES];
	vec2 m_BlatantTrackPoint[NUM_DUMMIES];
	vec2 m_BlatantHookTarget[NUM_DUMMIES];
	int m_BlatantHookHoldTicks[NUM_DUMMIES];
	bool m_BlatantFreezeResetFrozenLast[NUM_DUMMIES];
	int64_t m_BlatantFreezeResetSince[NUM_DUMMIES];
	int m_AvoidForcedDir[NUM_DUMMIES];
	int64_t m_AvoidForceUntil[NUM_DUMMIES];
	bool m_AvoidWasInDanger[NUM_DUMMIES];
	mutable vec2 m_AvoidLastMousePos[NUM_DUMMIES];

private:
	// Job class for parallel freeze prediction
	class CPredictFreezeJob : public IJob
	{
		CAvoidFreeze *m_pAvoidFreeze;
		CGameClient *m_pGameClient;
		CNetObj_PlayerInput m_Input;
		int m_Ticks;
		int *m_pOutDangerTick;
		float *m_pOutDangerDistance;
		std::mutex *m_pMutex;
		volatile bool m_Finished;

	public:
		CPredictFreezeJob(CAvoidFreeze *pAvoidFreeze, CGameClient *pGameClient, const CNetObj_PlayerInput &Input, int Ticks,
			int *pOutDangerTick, float *pOutDangerDistance, std::mutex *pMutex);
		void Run() override;
		bool IsFinished() const { return m_Finished; }
	};

	// Job class for parallel blatant freeze simulation
	class CSimulateBlatantFreezeJob : public IJob
	{
		CAvoidFreeze *m_pAvoidFreeze;
		CGameClient *m_pGameClient;
		int m_LocalId;
		CNetObj_PlayerInput m_Input;
		int m_Ticks;
		bool m_DetectFreeze;
		bool m_DetectDeath;
		bool m_DetectTele;
		bool m_DetectUnfreeze;
		int m_Result;
		std::mutex *m_pMutex;
		volatile bool m_Finished;

	public:
		CSimulateBlatantFreezeJob(CAvoidFreeze *pAvoidFreeze, CGameClient *pGameClient, int LocalId, const CNetObj_PlayerInput &Input, int Ticks,
			bool DetectFreeze, bool DetectDeath, bool DetectTele, bool DetectUnfreeze, std::mutex *pMutex);
		void Run() override;
		int GetResult() const { return m_Result; }
		bool IsFinished() const { return m_Finished; }
	};

	std::mutex m_Mutex;
};

#endif
