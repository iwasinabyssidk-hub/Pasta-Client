#include "pasta_visuals.h"

#include <base/color.h>
#include <base/math.h>
#include <base/str.h>
#include <base/system.h>
#include <base/windows.h>

#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/textrender.h>

#include <generated/client_data.h>
#include <generated/protocol.h>

#include <game/client/gameclient.h>
#include <game/client/prediction/entities/character.h>
#include <game/client/prediction/gameworld.h>
#include <game/client/ui_rect.h>
#include <game/collision.h>
#include <game/gamecore.h>
#include <game/localization.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(CONF_FAMILY_WINDOWS)
#define IStorage IWindowsStorageCompat
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#include <wincodec.h>
#undef IStorage
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "runtimeobject.lib")
#endif

namespace
{
ColorRGBA EvaluatePastaTextColor(unsigned StartColor, unsigned EndColor, bool Rainbow, float Speed, float Position)
{
	if(Rainbow)
	{
		const float Hue = std::fmod((float)time_get() / (float)time_freq() * Speed * 0.1f + Position, 1.0f);
		return color_cast<ColorRGBA>(ColorHSLA(Hue, 1.0f, 0.5f));
	}

	const ColorRGBA Start = color_cast<ColorRGBA>(ColorHSLA(StartColor));
	const ColorRGBA End = color_cast<ColorRGBA>(ColorHSLA(EndColor));
	return ColorRGBA(
		mix(Start.r, End.r, Position),
		mix(Start.g, End.g, Position),
		mix(Start.b, End.b, Position),
		mix(Start.a, End.a, Position));
}

void RenderPastaColoredText(ITextRender *pTextRender, float X, float Y, float FontSize, const char *pText, unsigned StartColor, unsigned EndColor, bool Rainbow, bool Segmented, float Speed)
{
	if(!Segmented)
	{
		pTextRender->TextColor(EvaluatePastaTextColor(StartColor, EndColor, Rainbow, Speed, 0.0f));
		pTextRender->Text(X, Y, FontSize, pText);
		pTextRender->TextColor(pTextRender->DefaultTextColor());
		return;
	}

	float CursorX = X;
	int Cursor = 0;
	int GlyphCount = 0;
	while(pText[Cursor] != '\0')
	{
		const int NextCursor = str_utf8_forward(pText, Cursor);
		if(NextCursor <= Cursor)
			break;
		++GlyphCount;
		Cursor = NextCursor;
	}

	char aGlyph[16] = {'\0'};
	Cursor = 0;
	int GlyphIndex = 0;
	while(pText[Cursor] != '\0')
	{
		const int NextCursor = str_utf8_forward(pText, Cursor);
		if(NextCursor <= Cursor)
			break;

		const int GlyphBytes = minimum<int>(NextCursor - Cursor, sizeof(aGlyph) - 1);
		mem_copy(aGlyph, pText + Cursor, GlyphBytes);
		aGlyph[GlyphBytes] = '\0';

		const float Position = GlyphCount > 1 ? GlyphIndex / (float)(GlyphCount - 1) : 0.0f;
		pTextRender->TextColor(EvaluatePastaTextColor(StartColor, EndColor, Rainbow, Speed, Position));
		pTextRender->Text(CursorX, Y, FontSize, aGlyph);
		CursorX += pTextRender->TextWidth(FontSize, aGlyph);

		Cursor = NextCursor;
		++GlyphIndex;
	}
	pTextRender->TextColor(pTextRender->DefaultTextColor());
}

ColorRGBA PackedToColor(unsigned Packed)
{
	return color_cast<ColorRGBA>(ColorHSLA(Packed));
}

ColorRGBA MixColor(const ColorRGBA &From, const ColorRGBA &To, float Amount, float Alpha)
{
	return ColorRGBA(
		mix(From.r, To.r, Amount),
		mix(From.g, To.g, Amount),
		mix(From.b, To.b, Amount),
		Alpha);
}

#if defined(CONF_FAMILY_WINDOWS)
bool LoadTextureFromImageBytes(IGraphics *pGraphics, const uint8_t *pData, size_t DataSize, IGraphics::CTextureHandle &Texture, const char *pName)
{
	if(pData == nullptr || DataSize == 0)
		return false;

	IWICImagingFactory *pFactory = nullptr;
	IWICStream *pStream = nullptr;
	IWICBitmapDecoder *pDecoder = nullptr;
	IWICBitmapFrameDecode *pFrame = nullptr;
	IWICFormatConverter *pConverter = nullptr;
	bool Success = false;

	const HRESULT FactoryResult = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pFactory));
	if(FAILED(FactoryResult))
		return false;

	if(SUCCEEDED(pFactory->CreateStream(&pStream)) &&
		SUCCEEDED(pStream->InitializeFromMemory(const_cast<BYTE *>(pData), (DWORD)DataSize)) &&
		SUCCEEDED(pFactory->CreateDecoderFromStream(pStream, nullptr, WICDecodeMetadataCacheOnLoad, &pDecoder)) &&
		SUCCEEDED(pDecoder->GetFrame(0, &pFrame)) &&
		SUCCEEDED(pFactory->CreateFormatConverter(&pConverter)))
	{
		if(SUCCEEDED(pConverter->Initialize(
			   pFrame,
			   GUID_WICPixelFormat32bppRGBA,
			   WICBitmapDitherTypeNone,
			   nullptr,
			   0.0f,
			   WICBitmapPaletteTypeMedianCut)))
		{
			UINT Width = 0;
			UINT Height = 0;
			if(SUCCEEDED(pConverter->GetSize(&Width, &Height)) && Width > 0 && Height > 0)
			{
				CImageInfo Image;
				Image.m_Width = Width;
				Image.m_Height = Height;
				Image.m_Format = CImageInfo::FORMAT_RGBA;
				Image.m_pData = static_cast<uint8_t *>(malloc(Image.DataSize()));
				if(Image.m_pData != nullptr)
				{
					if(SUCCEEDED(pConverter->CopyPixels(nullptr, Width * 4, (UINT)Image.DataSize(), Image.m_pData)))
					{
						if(Texture.IsValid())
							pGraphics->UnloadTexture(&Texture);
						Texture = pGraphics->LoadTextureRawMove(Image, 0, pName);
						Success = Texture.IsValid();
					}
					else
						Image.Free();
				}
			}
		}
	}

	if(pConverter)
		pConverter->Release();
	if(pFrame)
		pFrame->Release();
	if(pDecoder)
		pDecoder->Release();
	if(pStream)
		pStream->Release();
	if(pFactory)
		pFactory->Release();

	return Success;
}
#endif

void AddLineSegment(std::vector<IGraphics::CLineItem> &vSegments, vec2 From, vec2 To)
{
	if(distance(From, To) > 0.01f)
		vSegments.emplace_back(From.x, From.y, To.x, To.y);
}

struct SScreenPixelGrid
{
	float m_ScreenX0;
	float m_ScreenY0;
	float m_PixelSizeX;
	float m_PixelSizeY;
};

SScreenPixelGrid GetScreenPixelGrid(const IGraphics *pGraphics)
{
	float ScreenX0, ScreenY0, ScreenX1, ScreenY1;
	pGraphics->GetScreen(&ScreenX0, &ScreenY0, &ScreenX1, &ScreenY1);

	return {
		ScreenX0,
		ScreenY0,
		(ScreenX1 - ScreenX0) / maximum(1, pGraphics->ScreenWidth()),
		(ScreenY1 - ScreenY0) / maximum(1, pGraphics->ScreenHeight())};
}

vec2 SnapToScreenPixel(const SScreenPixelGrid &Grid, vec2 Pos)
{
	return vec2(
		Grid.m_ScreenX0 + roundf((Pos.x - Grid.m_ScreenX0) / Grid.m_PixelSizeX) * Grid.m_PixelSizeX,
		Grid.m_ScreenY0 + roundf((Pos.y - Grid.m_ScreenY0) / Grid.m_PixelSizeY) * Grid.m_PixelSizeY);
}

float SnapToPixelSpan(float Span, float PixelSize)
{
	return maximum(PixelSize, roundf(Span / PixelSize) * PixelSize);
}

void BuildEllipsizedUtf8(char *pDst, int DstSize, const char *pSrc, int MaxChars)
{
	if(DstSize <= 0)
		return;

	pDst[0] = '\0';
	if(pSrc == nullptr || pSrc[0] == '\0')
		return;

	str_utf8_truncate(pDst, DstSize, pSrc, MaxChars);
	if(str_length(pDst) < str_length(pSrc))
		str_append(pDst, "...", DstSize);
}

void RenderLineSegments(IGraphics *pGraphics, const std::vector<IGraphics::CLineItem> &vSegments, ColorRGBA Color, float Thickness, float Zoom)
{
	if(vSegments.empty() || Color.a <= 0.0f)
		return;

	const SScreenPixelGrid Grid = GetScreenPixelGrid(pGraphics);
	const float PixelSize = maximum(Grid.m_PixelSizeX, Grid.m_PixelSizeY);
	const float LineWidth = SnapToPixelSpan(maximum(PixelSize, (0.8f + (Thickness - 1.0f) * 0.5f) * Zoom), PixelSize);

	pGraphics->TextureClear();
	std::vector<IGraphics::CFreeformItem> vLineQuadSegments;
	vLineQuadSegments.reserve(vSegments.size());

	for(const auto &LineSegment : vSegments)
	{
		const vec2 InitPos = SnapToScreenPixel(Grid, vec2(LineSegment.m_X0, LineSegment.m_Y0));
		const vec2 FinishPos = SnapToScreenPixel(Grid, vec2(LineSegment.m_X1, LineSegment.m_Y1));
		const vec2 Diff = FinishPos - InitPos;
		if(length(Diff) < 0.001f)
			continue;

		const vec2 Perp = normalize(vec2(Diff.y, -Diff.x)) * (LineWidth * 0.5f);
		const vec2 Pos0 = FinishPos - Perp;
		const vec2 Pos1 = FinishPos + Perp;
		const vec2 Pos2 = InitPos - Perp;
		const vec2 Pos3 = InitPos + Perp;
		vLineQuadSegments.emplace_back(Pos0.x, Pos0.y, Pos1.x, Pos1.y, Pos2.x, Pos2.y, Pos3.x, Pos3.y);
	}

	if(vLineQuadSegments.empty())
		return;

	pGraphics->QuadsBegin();
	pGraphics->SetColor(Color);
	pGraphics->QuadsDrawFreeform(vLineQuadSegments.data(), vLineQuadSegments.size());
	pGraphics->QuadsEnd();
}

void RenderPointMarkers(IGraphics *pGraphics, IGraphics::CTextureHandle Texture, const std::vector<vec2> &vPoints, ColorRGBA Color, float Size)
{
	if(vPoints.empty() || Color.a <= 0.0f || Size <= 0.0f)
		return;

	const SScreenPixelGrid Grid = GetScreenPixelGrid(pGraphics);
	const float SizeX = SnapToPixelSpan(Size, Grid.m_PixelSizeX);
	const float SizeY = SnapToPixelSpan(Size, Grid.m_PixelSizeY);

	if(Texture.IsValid())
		pGraphics->TextureSet(Texture);
	else
		pGraphics->TextureClear();

	pGraphics->QuadsBegin();
	pGraphics->SetColor(Color);
	for(const vec2 &Point : vPoints)
	{
		const vec2 TopLeft = SnapToScreenPixel(Grid, Point - vec2(SizeX * 0.5f, SizeY * 0.5f));
		const IGraphics::CQuadItem Quad(TopLeft.x, TopLeft.y, SizeX, SizeY);
		pGraphics->QuadsDrawTL(&Quad, 1);
	}
	pGraphics->QuadsEnd();
}

void AddProjectilePath(CCollision *pCollision, std::vector<IGraphics::CLineItem> &vSegments, vec2 StartPos, vec2 StartVel, float Curvature, float Speed, float MaxTime, int NumSteps, bool Dotted)
{
	if(length(StartVel) < 0.001f || MaxTime <= 0.0f || NumSteps <= 0)
		return;

	vec2 PrevPos = StartPos;
	for(int Step = 1; Step <= NumSteps; ++Step)
	{
		const float T = MaxTime * Step / (float)NumSteps;
		const vec2 Pos = CalcPos(StartPos, StartVel, Curvature, Speed, T);

		vec2 ColPos;
		vec2 NewPos;
		if(pCollision->IntersectLine(PrevPos, Pos, &ColPos, &NewPos))
		{
			if(!Dotted || Step % 2 == 0)
				AddLineSegment(vSegments, PrevPos, ColPos);
			break;
		}

		if(!Dotted || Step % 2 == 0)
			AddLineSegment(vSegments, PrevPos, Pos);
		PrevPos = Pos;
	}
}

void AddLaserPath(CCollision *pCollision, std::vector<IGraphics::CLineItem> &vSegments, vec2 StartPos, vec2 Direction, float Reach, const CTuningParams *pTuning)
{
	if(length(Direction) < 0.001f || Reach <= 0.0f || pTuning == nullptr)
		return;

	vec2 Pos = StartPos;
	vec2 Dir = normalize(Direction);
	float Energy = Reach;
	int Bounces = 0;
	bool ZeroEnergyBounceInLastTick = false;

	while(Energy > 0.0f)
	{
		vec2 Coltile;
		vec2 To = Pos + Dir * Energy;
		vec2 BeforeCollision = To;
		const int Res = pCollision->IntersectLineTeleWeapon(Pos, To, &Coltile, &BeforeCollision);
		if(Res == 0)
		{
			AddLineSegment(vSegments, Pos, To);
			break;
		}

		AddLineSegment(vSegments, Pos, BeforeCollision);

		vec2 TempPos = BeforeCollision;
		vec2 TempDir = Dir * 4.0f;
		int OriginalTile = 0;
		if(Res == -1)
		{
			OriginalTile = pCollision->GetTile(round_to_int(Coltile.x), round_to_int(Coltile.y));
			pCollision->SetCollisionAt(round_to_int(Coltile.x), round_to_int(Coltile.y), TILE_SOLID);
		}
		pCollision->MovePoint(&TempPos, &TempDir, 1.0f, nullptr);
		if(Res == -1)
			pCollision->SetCollisionAt(round_to_int(Coltile.x), round_to_int(Coltile.y), OriginalTile);

		const float Distance = distance(Pos, TempPos);
		if(Distance == 0.0f && ZeroEnergyBounceInLastTick)
			break;

		Energy -= Distance + pTuning->m_LaserBounceCost;
		ZeroEnergyBounceInLastTick = Distance == 0.0f;
		Pos = TempPos;
		if(length(TempDir) < 0.001f)
			break;
		Dir = normalize(TempDir);

		++Bounces;
		if(Bounces > pTuning->m_LaserBounceNum)
			break;
	}
}

vec2 GetLocalAimDirection(CGameClient *pGameClient)
{
	vec2 Direction = pGameClient->m_Controls.GetRenderMousePos(g_Config.m_ClDummy);
	if(g_Config.m_TcScaleMouseDistance)
	{
		const int MaxDistance = g_Config.m_ClDyncam ? g_Config.m_ClDyncamMaxDistance : g_Config.m_ClMouseMaxDistance;
		if(MaxDistance > 5 && MaxDistance < 1000)
			Direction *= 1000.0f / (float)MaxDistance;
	}

	return length(Direction) > 0.001f ? normalize(Direction) : vec2(0.0f, -1.0f);
}

vec2 GetCharacterAimDirection(CGameClient *pGameClient, int ClientId)
{
	if(ClientId < 0 || ClientId >= MAX_CLIENTS)
		return vec2(0.0f, 0.0f);

	const auto &CharInfo = pGameClient->m_Snap.m_aCharacters[ClientId];
	if(CharInfo.m_HasExtendedDisplayInfo)
	{
		const vec2 Direction(CharInfo.m_ExtendedData.m_TargetX, CharInfo.m_ExtendedData.m_TargetY);
		return length(Direction) > 0.001f ? normalize(Direction) : vec2(0.0f, 0.0f);
	}

	const float Angle = pGameClient->m_aClients[ClientId].m_RenderCur.m_Angle / 256.0f;
	return direction(Angle);
}

bool IsFrozenCharacter(const CCharacter *pCharacter)
{
	return pCharacter->m_FreezeTime > 0 || pCharacter->Core()->m_DeepFrozen || pCharacter->Core()->m_LiveFrozen;
}

float GetPastaOverlayFovForSlot(int Slot)
{
	switch(Slot)
	{
	case -1: return (float)g_Config.m_PastaAimbotHookFov;
	case WEAPON_HAMMER: return (float)g_Config.m_PastaAimbotHammerFov;
	case WEAPON_GUN: return (float)g_Config.m_PastaAimbotPistolFov;
	case WEAPON_SHOTGUN: return (float)g_Config.m_PastaAimbotShotgunFov;
	case WEAPON_GRENADE: return (float)g_Config.m_PastaAimbotGrenadeFov;
	case WEAPON_LASER: return (float)g_Config.m_PastaAimbotLaserFov;
	default: return 90.0f;
	}
}

ColorRGBA GetPastaOverlayColorForSlot(int Slot)
{
	switch(Slot)
	{
	case -1: return ColorRGBA(1.0f, 0.28f, 0.25f, 0.82f);
	case WEAPON_HAMMER: return ColorRGBA(1.0f, 0.88f, 0.35f, 0.78f);
	case WEAPON_GUN: return ColorRGBA(0.92f, 0.92f, 0.92f, 0.75f);
	case WEAPON_SHOTGUN: return ColorRGBA(1.0f, 0.66f, 0.24f, 0.78f);
	case WEAPON_GRENADE: return ColorRGBA(1.0f, 0.42f, 0.30f, 0.8f);
	case WEAPON_LASER: return ColorRGBA(0.40f, 0.92f, 1.0f, 0.82f);
	default: return ColorRGBA(1.0f, 1.0f, 1.0f, 0.65f);
	}
}

void AddFovBoundary(std::vector<IGraphics::CLineItem> &vSegments, vec2 LocalPos, vec2 MouseAim, float FovDegrees, float Length)
{
	const float Fov = FovDegrees * pi / 180.0f;
	const vec2 LeftDir = direction(angle(MouseAim) - Fov * 0.5f);
	const vec2 RightDir = direction(angle(MouseAim) + Fov * 0.5f);
	AddLineSegment(vSegments, LocalPos, LocalPos + LeftDir * Length);
	AddLineSegment(vSegments, LocalPos, LocalPos + RightDir * Length);
}
}

void CPastaVisuals::PushNotification(const SWarning &Warning)
{
	SNotification Notification;
	str_copy(Notification.m_aTitle, Warning.m_aWarningTitle[0] ? Warning.m_aWarningTitle : Localize("Warning"));
	str_copy(Notification.m_aMessage, Warning.m_aWarningMsg);
	Notification.m_StartTime = Client()->LocalTime();
	Notification.m_Duration = Warning.m_AutoHide ? 4.0 : 7.0;
	m_vNotifications.push_back(Notification);

	while(m_vNotifications.size() > 5)
		m_vNotifications.pop_front();
}

void CPastaVisuals::UpdateMenuTheme()
{
	if(g_Config.m_PastaCustomTheme)
	{
		if(!m_PastaThemeActive)
		{
			m_StoredUiColor = g_Config.m_UiColor;
			m_PastaThemeActive = true;
		}

		if(g_Config.m_PastaRainbowMenu)
		{
			const ColorRGBA Col = EvaluatePastaTextColor(g_Config.m_PastaCustomAccentColor, g_Config.m_PastaCustomAccentColor, true, g_Config.m_PastaCustomColorRainbowSpeed, 0.0f);
			g_Config.m_UiColor = color_cast<ColorHSLA>(Col).Pack(false);
		}
		else
			g_Config.m_UiColor = g_Config.m_PastaCustomAccentColor;
	}
	else if(m_PastaThemeActive)
	{
		g_Config.m_UiColor = m_StoredUiColor;
		m_PastaThemeActive = false;
	}
}

void CPastaVisuals::EnsureNowPlayingWorker()
{
	if(m_NowPlayingWorkerStarted)
		return;
	m_NowPlayingWorkerStarted = true;

	m_NowPlayingThread = std::jthread([this](std::stop_token StopToken) {
#if defined(CONF_FAMILY_WINDOWS)
		IMMDeviceEnumerator *pDeviceEnumerator = nullptr;
		IMMDevice *pDefaultRenderDevice = nullptr;
		IAudioMeterInformation *pAudioMeter = nullptr;

		const auto ReleaseAudioMeter = [&]() {
			if(pAudioMeter)
			{
				pAudioMeter->Release();
				pAudioMeter = nullptr;
			}
			if(pDefaultRenderDevice)
			{
				pDefaultRenderDevice->Release();
				pDefaultRenderDevice = nullptr;
			}
			if(pDeviceEnumerator)
			{
				pDeviceEnumerator->Release();
				pDeviceEnumerator = nullptr;
			}
		};

		const auto EnsureAudioMeter = [&]() -> bool {
			if(pAudioMeter)
				return true;
			if(!pDeviceEnumerator)
			{
				if(FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&pDeviceEnumerator))))
					return false;
			}
			if(!pDefaultRenderDevice)
			{
				if(FAILED(pDeviceEnumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &pDefaultRenderDevice)))
					return false;
			}
			if(!pAudioMeter)
			{
				if(FAILED(pDefaultRenderDevice->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr, (void **)&pAudioMeter)))
					return false;
			}
			return pAudioMeter != nullptr;
		};

		try
		{
			winrt::init_apartment(winrt::apartment_type::multi_threaded);
		}
		catch(...)
		{
		}
#endif

		using namespace std::chrono_literals;
		auto NextMediaRefresh = std::chrono::steady_clock::now();
		bool CachedAvailable = false;
		std::string CachedDisplay;
		std::string CachedTrackKey;
		std::vector<uint8_t> vCachedCoverBytes;

		while(!StopToken.stop_requested())
		{
			if(!g_Config.m_PastaNowPlayingHud)
			{
				{
					std::lock_guard<std::mutex> Lock(m_NowPlayingMutex);
					m_PendingNowPlayingAvailable = false;
					m_PendingNowPlayingText.clear();
					m_PendingNowPlayingTrackKey.clear();
					m_PendingNowPlayingPeakLevel = 0.0f;
					m_vPendingNowPlayingCover.clear();
					++m_NowPlayingPendingVersion;
				}
				std::this_thread::sleep_for(250ms);
				continue;
			}

			float PeakLevel = 0.0f;

#if defined(CONF_FAMILY_WINDOWS)
			if(EnsureAudioMeter())
			{
				float RawPeakLevel = 0.0f;
				if(SUCCEEDED(pAudioMeter->GetPeakValue(&RawPeakLevel)))
					PeakLevel = std::clamp(RawPeakLevel, 0.0f, 1.0f);
				else
					ReleaseAudioMeter();
			}

			if(std::chrono::steady_clock::now() >= NextMediaRefresh)
			try
			{
				using namespace winrt::Windows::Media::Control;
				GlobalSystemMediaTransportControlsSessionManager Manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
				GlobalSystemMediaTransportControlsSession Session = Manager ? Manager.GetCurrentSession() : nullptr;
				bool Available = false;
				std::string Display;
				std::string TrackKey;
				std::vector<uint8_t> vCoverBytes;
				if(Session)
				{
					GlobalSystemMediaTransportControlsSessionMediaProperties MediaProperties = Session.TryGetMediaPropertiesAsync().get();
					const std::string Title = winrt::to_string(MediaProperties.Title());
					const std::string Artist = winrt::to_string(MediaProperties.Artist());
					const std::string SourceApp = winrt::to_string(Session.SourceAppUserModelId());

					if(!Title.empty() && !Artist.empty())
						Display = Artist + " - " + Title;
					else if(!Title.empty())
						Display = Title;
					else if(!Artist.empty())
						Display = Artist;
					else if(!SourceApp.empty())
						Display = SourceApp;

					if(!Display.empty())
					{
						TrackKey = Display;
						Available = true;

						try
						{
							auto Thumbnail = MediaProperties.Thumbnail();
							if(Thumbnail)
							{
								using namespace winrt::Windows::Storage::Streams;
								IRandomAccessStreamWithContentType Stream = Thumbnail.OpenReadAsync().get();
								const uint64_t Size64 = Stream.Size();
								if(Size64 > 0 && Size64 <= 16 * 1024 * 1024)
								{
									Buffer TempBuffer((uint32_t)Size64);
									IBuffer ReadBuffer = Stream.ReadAsync(TempBuffer, (uint32_t)Size64, InputStreamOptions::None).get();
									if(ReadBuffer.Length() > 0)
									{
										DataReader Reader = DataReader::FromBuffer(ReadBuffer);
										vCoverBytes.resize(ReadBuffer.Length());
										Reader.ReadBytes(vCoverBytes);
									}
								}
							}
						}
						catch(...)
						{
							vCoverBytes.clear();
						}
					}
				}

				CachedAvailable = Available;
				CachedDisplay = Display;
				CachedTrackKey = TrackKey;
				vCachedCoverBytes = std::move(vCoverBytes);
				NextMediaRefresh = std::chrono::steady_clock::now() + 1500ms;
			}
			catch(...)
			{
				CachedAvailable = false;
				CachedDisplay.clear();
				CachedTrackKey.clear();
				vCachedCoverBytes.clear();
				NextMediaRefresh = std::chrono::steady_clock::now() + 1500ms;
			}
#endif

			{
				std::lock_guard<std::mutex> Lock(m_NowPlayingMutex);
				m_PendingNowPlayingPeakLevel = PeakLevel;
				if(m_PendingNowPlayingAvailable != CachedAvailable ||
					m_PendingNowPlayingText != CachedDisplay ||
					m_PendingNowPlayingTrackKey != CachedTrackKey)
				{
					m_PendingNowPlayingAvailable = CachedAvailable;
					m_PendingNowPlayingText = CachedDisplay;
					m_PendingNowPlayingTrackKey = CachedTrackKey;
					m_vPendingNowPlayingCover = vCachedCoverBytes;
					++m_NowPlayingPendingVersion;
				}
			}

			std::this_thread::sleep_for(65ms);
		}

#if defined(CONF_FAMILY_WINDOWS)
		ReleaseAudioMeter();
#endif
	});
}

void CPastaVisuals::UpdateNowPlaying()
{
	EnsureNowPlayingWorker();

	if(!g_Config.m_PastaNowPlayingHud)
	{
		if(m_NowPlayingCoverTexture.IsValid())
			Graphics()->UnloadTexture(&m_NowPlayingCoverTexture);
		m_aNowPlayingText[0] = '\0';
		m_aNowPlayingTrackKey[0] = '\0';
		m_NowPlayingAvailable = false;
		m_NowPlayingPeakLevel = 0.0f;
		return;
	}

	uint64_t PendingVersion = 0;
	bool PendingAvailable = false;
	float PendingPeakLevel = 0.0f;
	std::string PendingText;
	std::string PendingTrackKey;
	std::vector<uint8_t> vPendingCover;
	{
		std::lock_guard<std::mutex> Lock(m_NowPlayingMutex);
		PendingVersion = m_NowPlayingPendingVersion;
		PendingAvailable = m_PendingNowPlayingAvailable;
		PendingPeakLevel = m_PendingNowPlayingPeakLevel;
		PendingText = m_PendingNowPlayingText;
		PendingTrackKey = m_PendingNowPlayingTrackKey;
		vPendingCover = m_vPendingNowPlayingCover;
	}
	if(PendingPeakLevel > m_NowPlayingPeakLevel)
		m_NowPlayingPeakLevel = mix(m_NowPlayingPeakLevel, PendingPeakLevel, 0.82f);
	else
		m_NowPlayingPeakLevel = mix(m_NowPlayingPeakLevel, PendingPeakLevel, 0.24f);

	if(PendingVersion == m_NowPlayingAppliedVersion)
		return;

	m_NowPlayingAppliedVersion = PendingVersion;
	if(!PendingAvailable || PendingText.empty())
	{
		if(m_NowPlayingCoverTexture.IsValid())
			Graphics()->UnloadTexture(&m_NowPlayingCoverTexture);
		m_aNowPlayingText[0] = '\0';
		m_aNowPlayingTrackKey[0] = '\0';
		m_NowPlayingAvailable = false;
		return;
	}

	const bool CoverChanged = PendingTrackKey != m_aNowPlayingTrackKey;
	if(CoverChanged)
	{
		if(m_NowPlayingCoverTexture.IsValid())
			Graphics()->UnloadTexture(&m_NowPlayingCoverTexture);
		if(!vPendingCover.empty())
			LoadTextureFromImageBytes(Graphics(), vPendingCover.data(), vPendingCover.size(), m_NowPlayingCoverTexture, "pasta-now-playing-cover");
	}

	str_copy(m_aNowPlayingText, PendingText.c_str());
	str_copy(m_aNowPlayingTrackKey, PendingTrackKey.c_str());
	m_NowPlayingAvailable = true;
}

void CPastaVisuals::RenderAimLines()
{
	if(!g_Config.m_PastaAimLines)
		return;

	const float Alpha = g_Config.m_PastaAimLinesAlpha / 100.0f;
	if(Alpha <= 0.0f)
		return;

	const CTuningParams *pTuning = GameClient()->GetTuning(GameClient()->m_aLocalTuneZone[g_Config.m_ClDummy]);
	const bool IsDdrace = GameClient()->m_GameWorld.m_WorldConfig.m_IsDDRace;

	std::vector<IGraphics::CLineItem> vGunSegments;
	std::vector<IGraphics::CLineItem> vShotgunSegments;
	std::vector<IGraphics::CLineItem> vGrenadeSegments;
	std::vector<IGraphics::CLineItem> vLaserSegments;

	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		if(!GameClient()->m_Snap.m_aCharacters[ClientId].m_Active)
			continue;

		const bool Local = ClientId == GameClient()->m_Snap.m_LocalClientId;
		if(g_Config.m_PastaAimLinesSelfOnly && !Local)
			continue;

		vec2 Direction = Local && !GameClient()->m_Snap.m_SpecInfo.m_Active && Client()->State() != IClient::STATE_DEMOPLAYBACK ?
			GetLocalAimDirection(GameClient()) :
			GetCharacterAimDirection(GameClient(), ClientId);
		if(length(Direction) < 0.001f)
			continue;

		const int Weapon = GameClient()->m_aClients[ClientId].m_RenderCur.m_Weapon;
		const vec2 StartPos = GameClient()->m_aClients[ClientId].m_RenderPos + Direction * 21.0f;

		if(Weapon == WEAPON_GUN && g_Config.m_PastaAimLinesGun)
		{
			AddProjectilePath(Collision(), vGunSegments, StartPos, Direction, pTuning->m_GunCurvature, pTuning->m_GunSpeed, pTuning->m_GunLifetime, 28, false);
		}
		else if(Weapon == WEAPON_SHOTGUN && g_Config.m_PastaAimLinesShotgun)
		{
			if(IsDdrace)
			{
				AddLaserPath(Collision(), vShotgunSegments, GameClient()->m_aClients[ClientId].m_RenderPos, Direction, pTuning->m_LaserReach, pTuning);
			}
			else
			{
				static const float s_aSpreading[] = {-0.185f, -0.070f, 0.0f, 0.070f, 0.185f};
				for(int i = -2; i <= 2; ++i)
				{
					const float SpreadAngle = angle(Direction) + s_aSpreading[i + 2];
					const float V = 1.0f - (absolute(i) / 2.0f);
					const float SpeedFactor = mix((float)pTuning->m_ShotgunSpeeddiff, 1.0f, V);
					AddProjectilePath(Collision(), vShotgunSegments, StartPos, direction(SpreadAngle) * SpeedFactor, pTuning->m_ShotgunCurvature, pTuning->m_ShotgunSpeed, pTuning->m_ShotgunLifetime, 20, false);
				}
			}
		}
		else if(Weapon == WEAPON_GRENADE && g_Config.m_PastaAimLinesGrenade)
		{
			AddProjectilePath(Collision(), vGrenadeSegments, StartPos, Direction, pTuning->m_GrenadeCurvature, pTuning->m_GrenadeSpeed, pTuning->m_GrenadeLifetime, 36, false);
		}
		else if(Weapon == WEAPON_LASER && g_Config.m_PastaAimLinesLaser)
		{
			AddLaserPath(Collision(), vLaserSegments, GameClient()->m_aClients[ClientId].m_RenderPos, Direction, pTuning->m_LaserReach, pTuning);
		}
	}

	RenderLineSegments(Graphics(), vGunSegments, ColorRGBA(1.0f, 1.0f, 1.0f, Alpha), 1.0f, GameClient()->m_Camera.m_Zoom);
	RenderLineSegments(Graphics(), vShotgunSegments, ColorRGBA(1.0f, 0.75f, 0.25f, Alpha), 1.0f, GameClient()->m_Camera.m_Zoom);
	RenderLineSegments(Graphics(), vGrenadeSegments, ColorRGBA(1.0f, 0.4f, 0.3f, Alpha), 1.0f, GameClient()->m_Camera.m_Zoom);
	RenderLineSegments(Graphics(), vLaserSegments, ColorRGBA(0.45f, 0.95f, 1.0f, Alpha), 1.0f, GameClient()->m_Camera.m_Zoom);
}

void CPastaVisuals::RenderAimbotOverlay()
{
	if(!g_Config.m_PastaAimbot)
		return;

	const int Dummy = g_Config.m_ClDummy;
	if(GameClient()->m_Snap.m_LocalClientId < 0 || !GameClient()->m_Snap.m_aCharacters[GameClient()->m_Snap.m_LocalClientId].m_Active)
		return;
	CCharacter *pLocalCharacter = GameClient()->m_PredictedWorld.GetCharacterById(GameClient()->m_Snap.m_LocalClientId);
	if(pLocalCharacter == nullptr)
		return;

	std::vector<IGraphics::CLineItem> vSegments;
	std::vector<IGraphics::CLineItem> vHookFovSegments;
	std::vector<IGraphics::CLineItem> vWeaponFovSegments;
	const vec2 LocalPos = GameClient()->m_aClients[GameClient()->m_Snap.m_LocalClientId].m_RenderPos;
	const vec2 RawAim = GameClient()->m_Controls.m_aMousePos[Dummy];
	const vec2 MouseAim = normalize(length(RawAim) > 0.001f ? RawAim : vec2(1.0f, 0.0f));

	if(g_Config.m_PastaAimbotDrawFov)
	{
		const int Weapon = pLocalCharacter->GetActiveWeapon();
		const float FovLength = 190.0f;
		const bool ShowHookFov = g_Config.m_PastaAimbotHook;
		if(ShowHookFov)
			AddFovBoundary(vHookFovSegments, LocalPos, MouseAim, GetPastaOverlayFovForSlot(-1), FovLength);
		AddFovBoundary(vWeaponFovSegments, LocalPos, MouseAim, GetPastaOverlayFovForSlot(Weapon), FovLength);
	}

	if(g_Config.m_PastaAimbotTargetBox)
	{
		auto AddTargetBox = [&](int ClientId, ColorRGBA Color) {
			if(ClientId < 0 || !GameClient()->m_Snap.m_aCharacters[ClientId].m_Active)
				return;
			const vec2 Pos = GameClient()->m_aClients[ClientId].m_RenderPos;
			const float HalfW = 18.0f;
			const float HalfH = 26.0f;
			AddLineSegment(vSegments, Pos + vec2(-HalfW, -HalfH), Pos + vec2(HalfW, -HalfH));
			AddLineSegment(vSegments, Pos + vec2(HalfW, -HalfH), Pos + vec2(HalfW, HalfH));
			AddLineSegment(vSegments, Pos + vec2(HalfW, HalfH), Pos + vec2(-HalfW, HalfH));
			AddLineSegment(vSegments, Pos + vec2(-HalfW, HalfH), Pos + vec2(-HalfW, -HalfH));
			RenderLineSegments(Graphics(), vSegments, Color, 2.0f, GameClient()->m_Camera.m_Zoom);
			vSegments.clear();
		};

		AddTargetBox(GameClient()->m_Controls.m_aPastaAimbotHookTargetId[Dummy], ColorRGBA(1.0f, 0.25f, 0.2f, 0.95f));
		AddTargetBox(GameClient()->m_Controls.m_aPastaAimbotWeaponTargetId[Dummy], ColorRGBA(1.0f, 0.65f, 0.15f, 0.95f));
	}

	RenderLineSegments(Graphics(), vHookFovSegments, GetPastaOverlayColorForSlot(-1), 1.35f, GameClient()->m_Camera.m_Zoom);
	RenderLineSegments(Graphics(), vWeaponFovSegments, GetPastaOverlayColorForSlot(pLocalCharacter->GetActiveWeapon()), 1.35f, GameClient()->m_Camera.m_Zoom);
	RenderLineSegments(Graphics(), vSegments, ColorRGBA(1.0f, 1.0f, 1.0f, 0.55f), 1.2f, GameClient()->m_Camera.m_Zoom);
}

void CPastaVisuals::RenderTrajectory()
{
	if(!g_Config.m_PastaTrajectoryEsp)
		return;
	if(GameClient()->m_PastaTas.IsPlaying())
		return;

	CGameWorld FutureWorld;
	FutureWorld.CopyWorldClean(GameClient()->Predict() ? &GameClient()->m_PredictedWorld : &GameClient()->m_GameWorld);

	struct STrajectoryState
	{
		int m_ClientId;
		vec2 m_PreviousPos;
		vec2 m_Offset;
		bool m_Local;
		bool m_Active;
	};

	std::vector<STrajectoryState> vStates;
	for(int ClientId = 0; ClientId < MAX_CLIENTS; ++ClientId)
	{
		if(!GameClient()->m_Snap.m_aCharacters[ClientId].m_Active)
			continue;

		const bool Local = ClientId == GameClient()->m_Snap.m_LocalClientId;
		if(g_Config.m_PastaTrajectoryEspSelfOnly && !Local)
			continue;

		CCharacter *pCharacter = FutureWorld.GetCharacterById(ClientId);
		if(!pCharacter)
			continue;

		const vec2 StartPos = GameClient()->m_aClients[ClientId].m_RenderPos;
		vStates.push_back({ClientId, StartPos, StartPos - pCharacter->GetPos(), Local, true});
	}

	if(vStates.empty())
		return;

	std::array<std::vector<IGraphics::CLineItem>, 3> aTrajectoryBuckets;
	std::array<std::vector<vec2>, 3> aTrajectoryDots;
	const bool Dotted = g_Config.m_PastaTrajectoryEspMode == 1;

	for(int Tick = 0; Tick < g_Config.m_PastaTrajectoryEspTicks; ++Tick)
	{
		FutureWorld.Tick();
		for(auto &State : vStates)
		{
			if(!State.m_Active)
				continue;

			CCharacter *pCharacter = FutureWorld.GetCharacterById(State.m_ClientId);
			if(!pCharacter)
			{
				State.m_Active = false;
				continue;
			}

			const vec2 CurrentPos = pCharacter->GetPos() + State.m_Offset;
			const int Bucket = IsFrozenCharacter(pCharacter) ? 1 : (State.m_Local ? 0 : 2);
			if(Dotted)
			{
				if(Tick % 2 == 0)
					aTrajectoryDots[Bucket].push_back(CurrentPos);
			}
			else
			{
				AddLineSegment(aTrajectoryBuckets[Bucket], State.m_PreviousPos, CurrentPos);
			}
			State.m_PreviousPos = CurrentPos;
		}
	}

	const float Thickness = (float)g_Config.m_PastaTrajectoryEspThickness;
	if(Dotted)
	{
		const float DotSize = maximum(3.0f, Thickness * 2.5f);
		const auto DotTexture = GameClient()->m_ParticlesSkin.m_aSpriteParticles[SPRITE_PART_BALL - SPRITE_PART_SLICE];
		RenderPointMarkers(Graphics(), DotTexture, aTrajectoryDots[0], PackedToColor(g_Config.m_PastaTrajectoryEspColorLocal), DotSize);
		RenderPointMarkers(Graphics(), DotTexture, aTrajectoryDots[1], PackedToColor(g_Config.m_PastaTrajectoryEspColorFrozen), DotSize);
		RenderPointMarkers(Graphics(), DotTexture, aTrajectoryDots[2], PackedToColor(g_Config.m_PastaTrajectoryEspColorOthers), DotSize);
	}
	else
	{
		RenderLineSegments(Graphics(), aTrajectoryBuckets[0], PackedToColor(g_Config.m_PastaTrajectoryEspColorLocal), Thickness, GameClient()->m_Camera.m_Zoom);
		RenderLineSegments(Graphics(), aTrajectoryBuckets[1], PackedToColor(g_Config.m_PastaTrajectoryEspColorFrozen), Thickness, GameClient()->m_Camera.m_Zoom);
		RenderLineSegments(Graphics(), aTrajectoryBuckets[2], PackedToColor(g_Config.m_PastaTrajectoryEspColorOthers), Thickness, GameClient()->m_Camera.m_Zoom);
	}
}

void CPastaVisuals::RenderAutoEdge()
{
	if(!g_Config.m_PastaAutoEdge || (!g_Config.m_PastaAutoEdgeShowFound && !g_Config.m_PastaAutoEdgeShowLocked))
		return;

	const int Dummy = g_Config.m_ClDummy;
	std::vector<IGraphics::CLineItem> vFoundSegments;
	std::vector<IGraphics::CLineItem> vLockedSegments;
	if(g_Config.m_PastaAutoEdgeShowFound)
	{
		for(int Index = 0; Index < GameClient()->m_Controls.m_aPastaAutoEdgeFoundCount[Dummy]; ++Index)
		{
			const vec2 Pos = GameClient()->m_Controls.m_aaPastaAutoEdgeFoundPos[Dummy][Index];
			const float Inward = Index == 0 ? 10.0f : -10.0f;
			AddLineSegment(vFoundSegments, Pos + vec2(0.0f, -10.0f), Pos + vec2(0.0f, 8.0f));
			AddLineSegment(vFoundSegments, Pos + vec2(0.0f, -10.0f), Pos + vec2(Inward, -5.0f));
			AddLineSegment(vFoundSegments, Pos + vec2(0.0f, 8.0f), Pos + vec2(Inward, 3.0f));
			AddLineSegment(vFoundSegments, Pos + vec2(Inward, -5.0f), Pos + vec2(Inward, 3.0f));
		}
	}

	if(g_Config.m_PastaAutoEdgeShowLocked && GameClient()->m_Controls.m_aPastaAutoEdgeLocked[Dummy])
	{
		const vec2 Pos = GameClient()->m_Controls.m_aPastaAutoEdgeLockedPos[Dummy];
		CCharacter *pLocalCharacter = GameClient()->m_PredictedWorld.GetCharacterById(GameClient()->m_Snap.m_LocalClientId);
		const bool SafeSideLeft = pLocalCharacter != nullptr ? pLocalCharacter->GetPos().x < Pos.x : true;
		const float Inward = SafeSideLeft ? -13.0f : 13.0f;
		AddLineSegment(vLockedSegments, Pos + vec2(0.0f, -14.0f), Pos + vec2(0.0f, 12.0f));
		AddLineSegment(vLockedSegments, Pos + vec2(0.0f, -14.0f), Pos + vec2(Inward, -8.0f));
		AddLineSegment(vLockedSegments, Pos + vec2(0.0f, 12.0f), Pos + vec2(Inward, 6.0f));
		AddLineSegment(vLockedSegments, Pos + vec2(Inward, -8.0f), Pos + vec2(Inward, 6.0f));
		AddLineSegment(vLockedSegments, Pos + vec2(Inward, -1.0f), Pos + vec2(Inward + (SafeSideLeft ? -6.0f : 6.0f), -1.0f));
	}

	RenderLineSegments(Graphics(), vFoundSegments, ColorRGBA(1.0f, 0.76f, 0.25f, 0.92f), 2.0f, GameClient()->m_Camera.m_Zoom);
	RenderLineSegments(Graphics(), vLockedSegments, ColorRGBA(0.96f, 0.2f, 0.18f, 0.98f), 3.0f, GameClient()->m_Camera.m_Zoom);
}

void CPastaVisuals::RenderSelfHitEsp()
{
	const int Dummy = g_Config.m_ClDummy;
	if(!GameClient()->m_Controls.m_aPastaAssistAimActive[Dummy])
		return;

	const int Weapon = GameClient()->m_Controls.m_aPastaAssistAimWeapon[Dummy];
	if((Weapon == WEAPON_LASER && !g_Config.m_PastaUnfreezeBotEsp) ||
		(Weapon == WEAPON_SHOTGUN && !g_Config.m_PastaAutoShotgun))
		return;

	CCharacter *pLocalCharacter = GameClient()->m_PredictedWorld.GetCharacterById(GameClient()->m_Snap.m_LocalClientId);
	if(pLocalCharacter == nullptr)
		return;

	const CTuningParams *pTuning = GameClient()->GetTuning(pLocalCharacter->GetOverriddenTuneZone());
	if(pTuning == nullptr)
		return;

	std::vector<IGraphics::CLineItem> vSegments;
	AddLaserPath(Collision(), vSegments, pLocalCharacter->GetPos(), GameClient()->m_Controls.m_aPastaAssistAimPos[Dummy], pTuning->m_LaserReach, pTuning);
	const ColorRGBA Color = Weapon == WEAPON_LASER ? ColorRGBA(0.45f, 0.95f, 1.0f, 0.9f) : ColorRGBA(1.0f, 0.72f, 0.28f, 0.9f);
	RenderLineSegments(Graphics(), vSegments, Color, 1.6f, GameClient()->m_Camera.m_Zoom);
}

void CPastaVisuals::RenderWorldVisuals()
{
	const bool HasSelfHitEsp =
		GameClient()->m_Controls.m_aPastaAssistAimActive[g_Config.m_ClDummy] &&
		((GameClient()->m_Controls.m_aPastaAssistAimWeapon[g_Config.m_ClDummy] == WEAPON_LASER && g_Config.m_PastaUnfreezeBotEsp) ||
			(GameClient()->m_Controls.m_aPastaAssistAimWeapon[g_Config.m_ClDummy] == WEAPON_SHOTGUN && g_Config.m_PastaAutoShotgun));
	if(!g_Config.m_PastaAimLines && !g_Config.m_PastaTrajectoryEsp &&
		!g_Config.m_PastaAimbot &&
		(!g_Config.m_PastaAutoEdge || (!g_Config.m_PastaAutoEdgeShowFound && !g_Config.m_PastaAutoEdgeShowLocked)) &&
		!HasSelfHitEsp)
		return;

	Graphics()->MapScreenToInterface(GameClient()->m_Camera.m_Center.x, GameClient()->m_Camera.m_Center.y, GameClient()->m_Camera.m_Zoom);
	RenderAimbotOverlay();
	RenderAimLines();
	RenderTrajectory();
	RenderAutoEdge();
	RenderSelfHitEsp();
}

void CPastaVisuals::RenderFeaturesHud(float Width, float Height)
{
	if(!g_Config.m_PastaFeaturesHud)
		return;

	std::vector<const char *> vFeatures;
	if(g_Config.m_PastaAimbot)
		vFeatures.push_back("Aim");
	if(g_Config.m_PastaAimbotAutoShoot || g_Config.m_PastaAimbotAutoShootKey)
		vFeatures.push_back("Auto Shoot");
	if(g_Config.m_PastaAimbotAutoHookKey)
		vFeatures.push_back("Auto Hook");
	if(g_Config.m_PastaAimbotDrawFov)
		vFeatures.push_back("Draw FOV");
	if(g_Config.m_PastaAimbotTargetBox)
		vFeatures.push_back("Target Box");
	if(g_Config.m_PastaAimbotTargetGlow)
		vFeatures.push_back("Target Glow");
	if(g_Config.m_PastaAvoidfreeze)
		vFeatures.push_back("Avoid");
	if(g_Config.m_PastaAutoFire)
		vFeatures.push_back("Auto Fire");
	if(g_Config.m_PastaAutoRehook)
		vFeatures.push_back("Auto Rehook");
	if(g_Config.m_PastaAutoJumpSave)
		vFeatures.push_back("Jump Save");
	if(g_Config.m_PastaQuickStop)
		vFeatures.push_back("Quick Stop");
	if(g_Config.m_PastaAutoAled)
		vFeatures.push_back("Auto Aled");
	if(g_Config.m_PastaUnfreezeBot)
		vFeatures.push_back("Auto Unfreeze");
	if(g_Config.m_PastaAutoShotgun)
		vFeatures.push_back("Auto Shotgun");
	if(g_Config.m_PastaFakeAim)
		vFeatures.push_back("Fake Aim");
	if(g_Config.m_PastaAutoEdge)
		vFeatures.push_back("Auto Edge");
	if(g_Config.m_PastaWatermark)
		vFeatures.push_back("Watermark");
	if(g_Config.m_PastaNowPlayingHud)
		vFeatures.push_back("Now Playing");
	if(g_Config.m_PastaNotifications)
		vFeatures.push_back("Notifications");
	if(g_Config.m_PastaCustomColorTee)
		vFeatures.push_back("Rainbow Tee");
	if(g_Config.m_PastaCustomColorHook)
		vFeatures.push_back("Rainbow Hook");
	if(g_Config.m_PastaCustomColorWeapon)
		vFeatures.push_back("Rainbow Weapon");
	if(g_Config.m_PastaTrail)
		vFeatures.push_back("Trail");
	if(g_Config.m_PastaSmoothCam || g_Config.m_PastaSuperDyncam)
		vFeatures.push_back("Smooth Cam");
	if(g_Config.m_PastaAimLines)
		vFeatures.push_back("Aim Lines");
	if(g_Config.m_PastaBulletLines)
		vFeatures.push_back("Bullet Lines");
	if(g_Config.m_PastaTrajectoryEsp)
		vFeatures.push_back("Trajectory");
	if(g_Config.m_PastaEmoteSpam)
		vFeatures.push_back("Emote Spam");

	if(vFeatures.empty())
		return;

	const float FontSize = std::clamp(3.5f + g_Config.m_PastaFeaturesHudFontSize * 0.7f, 4.0f, 18.0f);
	const float TagPaddingX = 5.0f;
	const float TagPaddingY = 2.0f;
	const float TagSpacing = 2.0f;
	float Y = 34.0f;
	const float BackgroundAlpha = std::clamp(g_Config.m_PastaFeaturesHudOpacity / 160.0f, 0.12f, 0.85f);

	for(size_t i = 0; i < vFeatures.size(); ++i)
	{
		const float TextWidth = TextRender()->TextWidth(FontSize, vFeatures[i]);
		const float TagWidth = TextWidth + TagPaddingX * 2.0f;
		const float TagHeight = FontSize + TagPaddingY * 2.0f;
		CUIRect TagRect = {Width - TagWidth, Y, TagWidth, TagHeight};
		TagRect.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, BackgroundAlpha), IGraphics::CORNER_L, 3.0f);

		RenderPastaColoredText(
			TextRender(),
			TagRect.x + TagPaddingX,
			TagRect.y + TagPaddingY - 0.25f,
			FontSize,
			vFeatures[i],
			g_Config.m_PastaFeaturesHudGradientStartCol,
			g_Config.m_PastaFeaturesHudGradientEndCol,
			g_Config.m_PastaFeaturesHudGradientRainbow,
			g_Config.m_PastaFeaturesHudSegmented,
			(float)g_Config.m_PastaFeaturesHudGradientSpeed);

		Y += TagHeight + TagSpacing;
	}
}

void CPastaVisuals::RenderWatermark(float Width, float Height)
{
	if(!g_Config.m_PastaWatermark)
		return;

	char aTimeBuf[32];
	str_timestamp_format(aTimeBuf, sizeof(aTimeBuf), "%H:%M");

	char aBuf[128];
	str_format(aBuf, sizeof(aBuf), "Pasta %s", aTimeBuf);

	const float FontSize = 7.25f;
	const float TextWidth = TextRender()->TextWidth(FontSize, aBuf);
	const float BoxPaddingX = 8.0f;
	const float BoxPaddingY = 2.5f;
	const float BoxWidth = TextWidth + BoxPaddingX * 2.0f + 2.0f;
	const float BoxHeight = FontSize + BoxPaddingY * 2.0f;
	const float X = minimum(Width - BoxWidth - 8.0f, Width * 0.5f + 56.0f);
	const float Y = 0.0f;
	(void)Height;

	CUIRect WatermarkRect = {X, Y, BoxWidth, BoxHeight};
	WatermarkRect.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.34f), IGraphics::CORNER_B, 3.0f);

	const float CenteredTextX = X + (BoxWidth - TextWidth) * 0.5f;
	RenderPastaColoredText(
		TextRender(),
		CenteredTextX,
		Y + BoxPaddingY - 0.5f,
		FontSize,
		aBuf,
		g_Config.m_PastaWatermarkGradientStartCol,
		g_Config.m_PastaWatermarkGradientEndCol,
		g_Config.m_PastaWatermarkGradientRainbow,
		g_Config.m_PastaWatermarkSegmented,
		(float)g_Config.m_PastaWatermarkGradientSpeed);
}

void CPastaVisuals::RenderNowPlayingHud(float Width, float Height)
{
	if(!g_Config.m_PastaNowPlayingHud)
		return;

	UpdateNowPlaying();
	if(!m_NowPlayingAvailable || m_aNowPlayingText[0] == '\0')
		return;

	char aDisplayText[160];
	BuildEllipsizedUtf8(aDisplayText, sizeof(aDisplayText), m_aNowPlayingText, 26);

	const float FontSize = 7.1f;
	const float BoxPaddingX = 6.0f;
	const float BoxPaddingY = 3.5f;
	const float CoverSize = 13.0f;
	const float VisualizerBarWidth = 2.2f;
	const float VisualizerBarGap = 1.35f;
	const int VisualizerBarCount = 4;
	const float VisualizerWidth = VisualizerBarCount * VisualizerBarWidth + (VisualizerBarCount - 1) * VisualizerBarGap + 3.0f;
	const float TextWidth = TextRender()->TextWidth(FontSize, aDisplayText);
	const float BoxWidth = minimum(Width * 0.42f, TextWidth + BoxPaddingX * 2.0f + CoverSize + VisualizerWidth + 11.0f);
	const float BoxHeight = maximum(CoverSize + BoxPaddingY * 2.0f, FontSize + BoxPaddingY * 2.0f);
	const float X = Width * 0.5f - BoxWidth * 0.5f;
	const float Y = 27.0f;
	(void)Height;

	CUIRect BoxRect = {X, Y, BoxWidth, BoxHeight};
	BoxRect.Draw(ColorRGBA(0.0f, 0.0f, 0.0f, 0.28f), IGraphics::CORNER_ALL, 3.0f);

	CUIRect InnerRect = BoxRect;
	InnerRect.VMargin(BoxPaddingX, &InnerRect);
	InnerRect.HMargin(BoxPaddingY, &InnerRect);

	CUIRect CoverRect = InnerRect;
	CoverRect.VSplitLeft(CoverSize, &CoverRect, &InnerRect);
	InnerRect.VSplitLeft(5.0f, nullptr, &InnerRect);
	CUIRect VisualizerRect;
	InnerRect.VSplitRight(VisualizerWidth, &InnerRect, &VisualizerRect);

	CoverRect.Draw(ColorRGBA(1.0f, 1.0f, 1.0f, 0.08f), IGraphics::CORNER_ALL, 2.5f);
	if(m_NowPlayingCoverTexture.IsValid())
	{
		Graphics()->TextureSet(m_NowPlayingCoverTexture);
		Graphics()->QuadsBegin();
		Graphics()->SetColor(1.0f, 1.0f, 1.0f, 1.0f);
		IGraphics::CQuadItem CoverQuad(CoverRect.x, CoverRect.y, CoverRect.w, CoverRect.h);
		Graphics()->QuadsDrawTL(&CoverQuad, 1);
		Graphics()->QuadsEnd();
	}
	else
	{
		Graphics()->TextureClear();
		Graphics()->QuadsBegin();
		Graphics()->SetColor(0.35f, 0.35f, 0.35f, 0.35f);
		IGraphics::CQuadItem CoverQuad(CoverRect.x + 3.0f, CoverRect.y + 3.0f, CoverRect.w - 6.0f, CoverRect.h - 6.0f);
		Graphics()->QuadsDrawTL(&CoverQuad, 1);
		Graphics()->QuadsEnd();
	}

	SLabelProperties LabelProps;
	LabelProps.m_MaxWidth = InnerRect.w;
	LabelProps.m_EllipsisAtEnd = true;

	if(!g_Config.m_PastaNowPlayingSegmented)
	{
		TextRender()->TextColor(EvaluatePastaTextColor(
			g_Config.m_PastaNowPlayingGradientStartCol,
			g_Config.m_PastaNowPlayingGradientEndCol,
			g_Config.m_PastaNowPlayingGradientRainbow,
			(float)g_Config.m_PastaNowPlayingGradientSpeed,
			0.0f));
		Ui()->DoLabel(&InnerRect, aDisplayText, FontSize, TEXTALIGN_ML, LabelProps);
		TextRender()->TextColor(TextRender()->DefaultTextColor());
	}
	else if(TextWidth > InnerRect.w)
	{
		TextRender()->TextColor(EvaluatePastaTextColor(
			g_Config.m_PastaNowPlayingGradientStartCol,
			g_Config.m_PastaNowPlayingGradientEndCol,
			g_Config.m_PastaNowPlayingGradientRainbow,
			(float)g_Config.m_PastaNowPlayingGradientSpeed,
			0.0f));
		Ui()->DoLabel(&InnerRect, aDisplayText, FontSize, TEXTALIGN_ML, LabelProps);
		TextRender()->TextColor(TextRender()->DefaultTextColor());
	}
	else
	{
		RenderPastaColoredText(
			TextRender(),
			InnerRect.x,
			InnerRect.y + (InnerRect.h - FontSize) * 0.5f - 0.75f,
			FontSize,
			aDisplayText,
			g_Config.m_PastaNowPlayingGradientStartCol,
			g_Config.m_PastaNowPlayingGradientEndCol,
			g_Config.m_PastaNowPlayingGradientRainbow,
			true,
			(float)g_Config.m_PastaNowPlayingGradientSpeed);
	}

	const float Time = (float)Client()->LocalTime();
	const float PeakLevel = std::clamp(powf(m_NowPlayingPeakLevel, 0.52f) * 2.35f, 0.0f, 1.0f);
	const bool RainbowBars = g_Config.m_PastaNowPlayingGradientRainbow != 0;
	const auto CapTexture = GameClient()->m_ParticlesSkin.m_aSpriteParticles[SPRITE_PART_BALL - SPRITE_PART_SLICE];

	Graphics()->TextureClear();
	Graphics()->QuadsBegin();
	for(int i = 0; i < VisualizerBarCount; ++i)
	{
		const float Phase = Time * 6.3f + i * 0.82f;
		const float Motion = 0.5f + 0.5f * std::sin(Phase);
		const float HeightFactor = std::clamp(0.3f + PeakLevel * (0.46f + 0.34f * Motion), 0.28f, 0.82f);
		const float BarHeight = mix(4.4f, VisualizerRect.h, HeightFactor);
		const float BarX = VisualizerRect.x + i * (VisualizerBarWidth + VisualizerBarGap);
		const float BarY = VisualizerRect.y + VisualizerRect.h - BarHeight;
		ColorRGBA BarColor = EvaluatePastaTextColor(
			g_Config.m_PastaNowPlayingGradientStartCol,
			g_Config.m_PastaNowPlayingGradientEndCol,
			RainbowBars,
			(float)g_Config.m_PastaNowPlayingGradientSpeed,
			RainbowBars ? 0.0f : (VisualizerBarCount > 1 ? i / (float)(VisualizerBarCount - 1) : 0.0f));
		BarColor.a = 0.92f;
		Graphics()->SetColor(BarColor);
		const float CapSize = VisualizerBarWidth;
		const float CenterInset = CapSize * 0.38f;
		const float CenterHeight = maximum(0.0f, BarHeight - CenterInset * 2.0f);
		if(CenterHeight > 0.0f)
		{
			const IGraphics::CQuadItem BarQuad(BarX, BarY + CenterInset, VisualizerBarWidth, CenterHeight);
			Graphics()->QuadsDrawTL(&BarQuad, 1);
		}
	}
	Graphics()->QuadsEnd();

	if(CapTexture.IsValid())
	{
		Graphics()->TextureSet(CapTexture);
		Graphics()->QuadsBegin();
		for(int i = 0; i < VisualizerBarCount; ++i)
		{
			const float Phase = Time * 6.3f + i * 0.82f;
			const float Motion = 0.5f + 0.5f * std::sin(Phase);
			const float HeightFactor = std::clamp(0.3f + PeakLevel * (0.46f + 0.34f * Motion), 0.28f, 0.82f);
			const float BarHeight = mix(4.4f, VisualizerRect.h, HeightFactor);
			const float BarX = VisualizerRect.x + i * (VisualizerBarWidth + VisualizerBarGap);
			const float BarY = VisualizerRect.y + VisualizerRect.h - BarHeight;
			ColorRGBA BarColor = EvaluatePastaTextColor(
				g_Config.m_PastaNowPlayingGradientStartCol,
				g_Config.m_PastaNowPlayingGradientEndCol,
				RainbowBars,
				(float)g_Config.m_PastaNowPlayingGradientSpeed,
				RainbowBars ? 0.0f : (VisualizerBarCount > 1 ? i / (float)(VisualizerBarCount - 1) : 0.0f));
			BarColor.a = 0.92f;
			Graphics()->SetColor(BarColor);

			const IGraphics::CQuadItem TopCap(BarX, BarY, VisualizerBarWidth, VisualizerBarWidth);
			const IGraphics::CQuadItem BottomCap(BarX, BarY + maximum(0.0f, BarHeight - VisualizerBarWidth), VisualizerBarWidth, VisualizerBarWidth);
			Graphics()->QuadsDrawTL(&TopCap, 1);
			if(BarHeight > VisualizerBarWidth)
				Graphics()->QuadsDrawTL(&BottomCap, 1);
		}
		Graphics()->QuadsEnd();
	}
}

void CPastaVisuals::RenderNotifications(float Width, float Height)
{
	if(!g_Config.m_PastaNotifications)
		return;

	const double Now = Client()->LocalTime();
	while(!m_vNotifications.empty())
	{
		const SNotification &Notification = m_vNotifications.front();
		if(Now - Notification.m_StartTime > Notification.m_Duration)
			m_vNotifications.pop_front();
		else
			break;
	}

	float Y = 18.0f;
	for(const SNotification &Notification : m_vNotifications)
	{
		const float Elapsed = (float)(Now - Notification.m_StartTime);
		const float FadeIn = minimum(Elapsed / 0.25f, 1.0f);
		const float FadeOut = minimum((float)((Notification.m_StartTime + Notification.m_Duration - Now) / 0.35), 1.0f);
		const float Alpha = std::clamp(minimum(FadeIn, FadeOut), 0.0f, 1.0f);
		const float Slide = (1.0f - FadeIn) * 30.0f;

		const float TitleSize = 9.0f;
		const float TextSize = 7.5f;
		const float TitleWidth = TextRender()->TextWidth(TitleSize, Notification.m_aTitle);
		const float TextWidth = TextRender()->TextWidth(TextSize, Notification.m_aMessage);
		const float BoxWidth = maximum(TitleWidth, TextWidth) + 16.0f;
		const float BoxHeight = 26.0f;
		const float X = Width - BoxWidth - 18.0f + Slide;
		(void)Height;

		Graphics()->TextureClear();
		Graphics()->QuadsBegin();
		Graphics()->SetColor(0.08f, 0.08f, 0.08f, 0.82f * Alpha);
		IGraphics::CQuadItem Quad(X, Y, BoxWidth, BoxHeight);
		Graphics()->QuadsDrawTL(&Quad, 1);
		Graphics()->QuadsEnd();

		TextRender()->TextColor(ColorRGBA(1.0f, 0.85f, 0.35f, Alpha));
		TextRender()->Text(X + 8.0f, Y + 4.0f, TitleSize, Notification.m_aTitle);
		TextRender()->TextColor(ColorRGBA(1.0f, 1.0f, 1.0f, Alpha));
		TextRender()->Text(X + 8.0f, Y + 13.0f, TextSize, Notification.m_aMessage);
		TextRender()->TextColor(TextRender()->DefaultTextColor());

		Y += BoxHeight + 6.0f;
	}
}

void CPastaVisuals::OnRender()
{
	UpdateMenuTheme();

	if(GameClient()->m_Menus.IsActive())
		return;

	if(Client()->State() != IClient::STATE_ONLINE && Client()->State() != IClient::STATE_DEMOPLAYBACK)
		return;

	RenderWorldVisuals();

	const float Width = 300.0f * Graphics()->ScreenAspect();
	const float Height = 300.0f;
	Graphics()->MapScreen(0, 0, Width, Height);

	RenderFeaturesHud(Width, Height);
	RenderWatermark(Width, Height);
	RenderNowPlayingHud(Width, Height);
	RenderNotifications(Width, Height);
}
