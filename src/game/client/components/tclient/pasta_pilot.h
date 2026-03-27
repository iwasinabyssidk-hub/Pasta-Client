#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_PASTA_PILOT_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_PASTA_PILOT_H

#include <generated/protocol.h>

#include <game/client/component.h>

#include <base/vmath.h>

#include <vector>

class CPastaPilot : public CComponent
{
public:
	struct SFrame
	{
		CNetObj_PlayerInput m_Input{};
		vec2 m_Mouse{};
		vec2 m_Pos{};
	};

	int Sizeof() const override { return sizeof(*this); }
	void OnReset() override;
	void OnStateChange(int NewState, int OldState) override;
	void OnUpdate() override;

	bool GetRealtimeInput(CNetObj_PlayerInput &OutInput, vec2 &OutMouse) const;
	bool IsRunning() const { return m_Running; }

private:
	std::vector<vec2> m_vFinishTiles;
	std::vector<int> m_vFinishTileIndices;
	std::vector<vec2> m_vHookTiles;
	std::vector<int8_t> m_vFieldDirX;
	std::vector<int8_t> m_vFieldDirY;
	std::vector<SFrame> m_vPlan;
	bool m_Running = false;
	bool m_TilesCached = false;
	bool m_FieldReady = false;
	bool m_HasRealtimeInput = false;
	CNetObj_PlayerInput m_RealtimeInput{};
	vec2 m_RealtimeMouse = vec2(0.0f, 0.0f);
	double m_Accumulator = 0.0;
	int64_t m_LastUpdateTime = 0;
	int m_FieldWidth = 0;
	int m_FieldHeight = 0;
	int m_ExecCursor = 0;
	int m_ReplanCountdown = 0;
	int m_RuntimeHookHoldTicks = 0;
	int m_RuntimeHookCooldownTicks = 0;
	vec2 m_RuntimeHookAim = vec2(0.0f, 0.0f);
	int m_RuntimeStuckTicks = 0;
	vec2 m_RuntimeLastPos = vec2(0.0f, 0.0f);
	int m_RuntimeLastDir = 0;

	void Start();
	void Stop();
	void CacheTiles();
	void BuildFlowField();
	bool BuildPlan();
	bool ResolveGoalPos(vec2 LocalPos, vec2 &OutGoalPos) const;
};

#endif
