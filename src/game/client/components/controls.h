/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef GAME_CLIENT_COMPONENTS_CONTROLS_H
#define GAME_CLIENT_COMPONENTS_CONTROLS_H

#include <base/vmath.h>

#include <engine/client.h>
#include <engine/console.h>

#include <generated/protocol.h>

#include <game/client/component.h>

class CControls : public CComponent
{
public:
	float GetMinMouseDistance() const;
	float GetMaxMouseDistance() const;

	enum class EMouseInputType
	{
		ABSOLUTE,
		RELATIVE,
		AUTOMATED,
	};

	vec2 m_aMousePos[NUM_DUMMIES];
	vec2 m_aMousePosOnAction[NUM_DUMMIES];
	vec2 m_aTargetPos[NUM_DUMMIES];

	EMouseInputType m_aMouseInputType[NUM_DUMMIES];

	int m_aAmmoCount[NUM_WEAPONS];

	int64_t m_LastSendTime;
	CNetObj_PlayerInput m_aInputData[NUM_DUMMIES];
	CNetObj_PlayerInput m_aLastData[NUM_DUMMIES];
	int m_aInputDirectionLeft[NUM_DUMMIES];
	int m_aInputDirectionRight[NUM_DUMMIES];
	int m_aShowHookColl[NUM_DUMMIES];
	int m_aRawFireState[NUM_DUMMIES];
	int m_aRawJumpState[NUM_DUMMIES];
	int m_aRawHookState[NUM_DUMMIES];

	// TClient
	CNetObj_PlayerInput m_aFastInput[NUM_DUMMIES];
	bool m_FastInputHookAction = false;
	bool m_FastInputFireAction = false;

	// Pasta misc runtime state
	int m_aPastaLastHookState[NUM_DUMMIES];
	int m_aPastaMoonwalkFlip[NUM_DUMMIES];
	int64_t m_aPastaInputActivity[NUM_DUMMIES];
	int64_t m_aPastaAntiAfkPulseEnd[NUM_DUMMIES];
	int64_t m_aPastaLastAntiAfkPulse[NUM_DUMMIES];
	int64_t m_aPastaLastRehook[NUM_DUMMIES];
	bool m_aPastaPendingRehookRelease[NUM_DUMMIES];
	bool m_aPastaAssistAimActive[NUM_DUMMIES];
	int m_aPastaAssistAimWeapon[NUM_DUMMIES];
	vec2 m_aPastaAssistAimPos[NUM_DUMMIES];
	bool m_aPastaGhostMoveWarned[NUM_DUMMIES];
	bool m_aPastaRenderMouseOverride[NUM_DUMMIES];
	int m_aPastaRenderMouseTargetId[NUM_DUMMIES];
	vec2 m_aPastaVisibleMousePos[NUM_DUMMIES];
	vec2 m_aPastaSentMousePos[NUM_DUMMIES];
	float m_aPastaFakeAimAngle[NUM_DUMMIES];
	int64_t m_aPastaFakeAimLastRandomUpdate[NUM_DUMMIES];

	CControls();
	int Sizeof() const override { return sizeof(*this); }

	void OnReset() override;
	void OnRender() override;
	void OnMessage(int MsgType, void *pRawMsg) override;
	bool OnCursorMove(float x, float y, IInput::ECursorType CursorType) override;
	void OnConsoleInit() override;
	virtual void OnPlayerDeath();

	int SnapInput(int *pData);
	void ClampMousePos();
	void ResetInput(int Dummy);
	bool CheckNewInput();
	vec2 GetRenderMousePos(int Dummy);

	vec2 EnsureValidAim(vec2 Pos);
	void TriggerFireTap(int Dummy);
	bool IsMouseMoved(int LocalPlayerId) const;
	bool IsPlayerActive(int LocalPlayerId) const;

private:
	static void ConKeyInputState(IConsole::IResult *pResult, void *pUserData);
	static void ConKeyInputCounter(IConsole::IResult *pResult, void *pUserData);
	static void ConKeyFireInputCounter(IConsole::IResult *pResult, void *pUserData);
	static void ConKeyInputSet(IConsole::IResult *pResult, void *pUserData);
	static void ConKeyInputNextPrevWeapon(IConsole::IResult *pResult, void *pUserData);
};
#endif
