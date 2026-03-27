#ifndef GAME_CLIENT_COMPONENTS_TCLIENT_PASTA_VISUALS_H
#define GAME_CLIENT_COMPONENTS_TCLIENT_PASTA_VISUALS_H

#include <engine/graphics.h>
#include <engine/warning.h>

#include <game/client/component.h>

#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class CPastaVisuals : public CComponent
{
public:
	int Sizeof() const override { return sizeof(*this); }
	void OnRender() override;

	void PushNotification(const SWarning &Warning);

private:
	struct SNotification
	{
		char m_aTitle[128];
		char m_aMessage[256];
		double m_StartTime;
		double m_Duration;
	};

	std::deque<SNotification> m_vNotifications;
	bool m_PastaThemeActive = false;
	unsigned m_StoredUiColor = 0;
	bool m_NowPlayingAvailable = false;
	char m_aNowPlayingText[256] = "";
	char m_aNowPlayingTrackKey[256] = "";
	float m_NowPlayingPeakLevel = 0.0f;
	IGraphics::CTextureHandle m_NowPlayingCoverTexture;
	std::jthread m_NowPlayingThread;
	std::mutex m_NowPlayingMutex;
	bool m_NowPlayingWorkerStarted = false;
	bool m_PendingNowPlayingAvailable = false;
	float m_PendingNowPlayingPeakLevel = 0.0f;
	uint64_t m_NowPlayingPendingVersion = 0;
	uint64_t m_NowPlayingAppliedVersion = 0;
	std::string m_PendingNowPlayingText;
	std::string m_PendingNowPlayingTrackKey;
	std::vector<uint8_t> m_vPendingNowPlayingCover;

	void UpdateMenuTheme();
	void EnsureNowPlayingWorker();
	void UpdateNowPlaying();
	void RenderWorldVisuals();
	void RenderAimbotOverlay();
	void RenderAimLines();
	void RenderTrajectory();
	void RenderAutoEdge();
	void RenderSelfHitEsp();
	void RenderFeaturesHud(float Width, float Height);
	void RenderWatermark(float Width, float Height);
	void RenderNowPlayingHud(float Width, float Height);
	void RenderNotifications(float Width, float Height);
};

#endif
