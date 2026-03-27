#include <base/math.h>

#include <engine/graphics.h>
#include <engine/shared/config.h>
#include <engine/shared/localization.h>

#include <game/client/components/menus.h>
#include <game/client/ui.h>
#include <game/client/ui_scrollregion.h>
#include <game/localization.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
constexpr float gs_PastaFontSize = 14.0f;
constexpr float gs_PastaHeadlineFontSize = 20.0f;
constexpr float gs_PastaHeadlineHeight = gs_PastaHeadlineFontSize;
constexpr float gs_PastaLineSize = 20.0f;
constexpr float gs_PastaColorPickerLineSize = 25.0f;
constexpr float gs_PastaColorPickerLabelSize = 13.0f;
constexpr float gs_PastaColorPickerSpacing = 5.0f;
constexpr float gs_PastaMargin = 10.0f;
constexpr float gs_PastaMarginSmall = 5.0f;
constexpr float gs_PastaSectionSpacing = 30.0f;
constexpr float gs_PastaColumnSpacing = 30.0f;
constexpr float gs_PastaSectionBoxPadding = 20.0f;

enum
{
	PASTA_TAB_AIMBOT = 0,
	PASTA_TAB_MISC,
	PASTA_TAB_VISUALS,
	PASTA_TAB_AVOID,
	PASTA_TAB_TAS,
	PASTA_TAB_SETTINGS,
	NUM_PASTA_TABS,
};

struct CPastaScrollState
{
	CScrollRegion m_ScrollRegion;
	std::vector<CUIRect> m_vSectionBoxes;
	vec2 m_PrevScrollOffset = vec2(0.0f, 0.0f);
};

struct SBoolEntry
{
	const char *m_pLabel;
	int *m_pValue;
};

ColorRGBA GetPastaAccentColor()
{
	return color_cast<ColorRGBA>(ColorHSLA(g_Config.m_PastaRainbowMenu ? g_Config.m_UiColor : g_Config.m_PastaCustomAccentColor, true));
}

ColorRGBA GetPastaBackgroundColor()
{
	ColorRGBA Bg = color_cast<ColorRGBA>(ColorHSLA(g_Config.m_PastaCustomBgColor, true));
	Bg.a = 1.0f;
	return Bg;
}

ColorRGBA GetPastaPanelColor(float AccentAmount, float Alpha)
{
	if(!g_Config.m_PastaCustomTheme)
		return ColorRGBA(0.0f, 0.0f, 0.0f, Alpha);

	const ColorRGBA Bg = GetPastaBackgroundColor();
	const ColorRGBA Accent = GetPastaAccentColor();
	return ColorRGBA(
		mix(Bg.r, Accent.r, AccentAmount),
		mix(Bg.g, Accent.g, AccentAmount),
		mix(Bg.b, Accent.b, AccentAmount),
		Alpha);
}

template<size_t N>
bool DoPastaDropDown(CMenus *pMenus, CUIRect &View, const char *pLabel, int &Value, const std::array<const char *, N> &aNames, CUi::SDropDownState &State, CScrollRegion &ScrollRegion, float LabelWidth = 170.0f)
{
	State.m_SelectionPopupContext.m_pScrollRegion = &ScrollRegion;
	CUIRect Row, Label, DropDown;
	View.HSplitTop(gs_PastaLineSize, &Row, &View);
	Row.VSplitLeft(LabelWidth, &Label, &DropDown);
	pMenus->MenuUi()->DoLabel(&Label, pLabel, gs_PastaFontSize, TEXTALIGN_ML);
	auto aDropDownNames = aNames;
	const int NewValue = pMenus->MenuUi()->DoDropDown(&DropDown, Value, aDropDownNames.data(), aDropDownNames.size(), State);
	if(NewValue != Value)
	{
		Value = NewValue;
		return true;
	}
	return false;
}

bool DoPastaDropDownDynamic(CMenus *pMenus, CUIRect &View, const char *pLabel, int &Value, const std::vector<const char *> &vNames, CUi::SDropDownState &State, CScrollRegion &ScrollRegion, float LabelWidth = 170.0f)
{
	State.m_SelectionPopupContext.m_pScrollRegion = &ScrollRegion;
	CUIRect Row, Label, DropDown;
	View.HSplitTop(gs_PastaLineSize, &Row, &View);
	Row.VSplitLeft(LabelWidth, &Label, &DropDown);
	pMenus->MenuUi()->DoLabel(&Label, pLabel, gs_PastaFontSize, TEXTALIGN_ML);
	std::vector<const char *> vDropDownNames = vNames;
	const int NewValue = pMenus->MenuUi()->DoDropDown(&DropDown, Value, vDropDownNames.empty() ? nullptr : &vDropDownNames[0], vDropDownNames.size(), State);
	if(NewValue != Value)
	{
		Value = NewValue;
		return true;
	}
	return false;
}

static void BeginPastaScrollColumns(CUIRect MainView, CPastaScrollState &State, CUIRect &LeftView, CUIRect &RightView);
static void EndPastaScrollColumns(CUIRect MainView, CPastaScrollState &State, const CUIRect &LeftView, const CUIRect &RightView);
static size_t BeginPastaSection(CMenus *pMenus, CUIRect &Column, CPastaScrollState &State, const char *pTitle);
static void FinishPastaSection(CUIRect &Column, CPastaScrollState &State, size_t Index);
static void DoPastaCheckBoxes(CMenus *pMenus, CUIRect &View, const SBoolEntry *pEntries, size_t Count);
static void DoPastaSlider(CMenus *pMenus, CUIRect &View, int *pValue, const char *pLabel, int Min, int Max, const char *pSuffix = "");
static bool DoPastaButton(CMenus *pMenus, CUIRect &View, CButtonContainer &ButtonId, const char *pLabel, ColorRGBA Color = ColorRGBA(1.0f, 1.0f, 1.0f, 0.25f));
static void DoPastaEditBox(CMenus *pMenus, CUIRect &View, CLineInput *pInput, const char *pLabel, const char *pDefault, char *pBuf, size_t BufSize);
static void DoPastaColorPicker(CMenus *pMenus, CUIRect &View, CButtonContainer &ResetId, const char *pLabel, unsigned *pColorValue, ColorRGBA DefaultColor);

struct SPastaWeaponConfig
{
	const char *m_pTitle;
	int *m_pEnable;
	int *m_pFov;
	int *m_pPriority;
	int *m_pSilent;
	int *m_pIgnoreFriends;
	int *m_pAccuracy;
};

static void RenderPastaWeaponSection(CMenus *pMenus, CUIRect &Column, CPastaScrollState &State, const SPastaWeaponConfig &Weapon, bool First);
static void RenderPastaGradientSection(CMenus *pMenus, CUIRect &Column, CPastaScrollState &State, const char *pTitle, int *pEnable, int *pSegmented, int *pSpeed, unsigned *pStartColor, unsigned *pEndColor, int *pRainbow, bool First);

void RenderPastaAimbot(CMenus *pMenus, CUIRect MainView);
void RenderPastaMisc(CMenus *pMenus, CUIRect MainView);
void RenderPastaVisuals(CMenus *pMenus, CUIRect MainView);
void RenderPastaAvoid(CMenus *pMenus, CUIRect MainView);
void RenderPastaTas(CMenus *pMenus, CUIRect MainView);
void RenderPastaSettings(CMenus *pMenus, CUIRect MainView);
}

void CMenus::RenderSettingsPasta(CUIRect MainView)
{
	static int s_CurTab = PASTA_TAB_AIMBOT;
	static CButtonContainer s_aTabButtons[NUM_PASTA_TABS];
	const std::array<const char *, NUM_PASTA_TABS> aTabNames = {
		Localize("Aimbot"),
		Localize("Misc"),
		Localize("Visuals & HUD"),
		Localize("Avoid"),
		"TAS",
		Localize("Settings"),
	};

	CUIRect TabBar, Button;
	MainView.HSplitTop(gs_PastaLineSize, &TabBar, &MainView);
	const float TabWidth = TabBar.w / NUM_PASTA_TABS;
	for(int i = 0; i < NUM_PASTA_TABS; ++i)
	{
		TabBar.VSplitLeft(TabWidth, &Button, &TabBar);
		const int Corners = i == 0 ? IGraphics::CORNER_L : i == NUM_PASTA_TABS - 1 ? IGraphics::CORNER_R : IGraphics::CORNER_NONE;
		if(DoButton_MenuTab(&s_aTabButtons[i], aTabNames[i], s_CurTab == i, &Button, Corners, nullptr, nullptr, nullptr, nullptr, 4.0f))
			s_CurTab = i;
	}

	MainView.HSplitTop(gs_PastaMargin, nullptr, &MainView);

	if(s_CurTab == PASTA_TAB_AIMBOT)
		RenderPastaAimbot(this, MainView);
	else if(s_CurTab == PASTA_TAB_MISC)
		RenderPastaMisc(this, MainView);
	else if(s_CurTab == PASTA_TAB_VISUALS)
		RenderPastaVisuals(this, MainView);
	else if(s_CurTab == PASTA_TAB_AVOID)
		RenderPastaAvoid(this, MainView);
	else if(s_CurTab == PASTA_TAB_TAS)
		RenderPastaTas(this, MainView);
	else
		RenderPastaSettings(this, MainView);
}

namespace
{
static void BeginPastaScrollColumns(CUIRect MainView, CPastaScrollState &State, CUIRect &LeftView, CUIRect &RightView)
{
	vec2 ScrollOffset(0.0f, 0.0f);
	CScrollRegionParams ScrollParams;
	ScrollParams.m_ScrollUnit = 60.0f;
	ScrollParams.m_Flags = CScrollRegionParams::FLAG_CONTENT_STATIC_WIDTH;
	ScrollParams.m_ScrollbarMargin = 5.0f;
	State.m_ScrollRegion.Begin(&MainView, &ScrollOffset, &ScrollParams);

	MainView.y += ScrollOffset.y;
	MainView.VSplitRight(5.0f, &MainView, nullptr);
	MainView.VSplitLeft(5.0f, nullptr, &MainView);
	MainView.VSplitMid(&LeftView, &RightView, gs_PastaColumnSpacing);
	LeftView.VSplitLeft(gs_PastaMarginSmall, nullptr, &LeftView);
	RightView.VSplitRight(gs_PastaMarginSmall, &RightView, nullptr);

	for(CUIRect &Section : State.m_vSectionBoxes)
	{
		Section.w += gs_PastaSectionBoxPadding;
		Section.h += gs_PastaSectionBoxPadding;
		Section.x -= gs_PastaSectionBoxPadding * 0.5f;
		Section.y -= gs_PastaSectionBoxPadding * 0.5f;
		Section.y -= State.m_PrevScrollOffset.y - ScrollOffset.y;
		Section.Draw(GetPastaPanelColor(0.08f, 0.25f), IGraphics::CORNER_ALL, 10.0f);
	}

	State.m_PrevScrollOffset = ScrollOffset;
	State.m_vSectionBoxes.clear();
}

static void EndPastaScrollColumns(CUIRect MainView, CPastaScrollState &State, const CUIRect &LeftView, const CUIRect &RightView)
{
	CUIRect ScrollRegionRect;
	ScrollRegionRect.x = MainView.x;
	float ContentBottom = maximum(LeftView.y, RightView.y);
	for(const CUIRect &Section : State.m_vSectionBoxes)
		ContentBottom = maximum(ContentBottom, Section.y + Section.h);
	ScrollRegionRect.y = ContentBottom + gs_PastaMarginSmall * 2.0f;
	ScrollRegionRect.w = MainView.w;
	ScrollRegionRect.h = 0.0f;
	State.m_ScrollRegion.AddRect(ScrollRegionRect);
	State.m_ScrollRegion.End();
}

static size_t BeginPastaSection(CMenus *pMenus, CUIRect &Column, CPastaScrollState &State, const char *pTitle)
{
	State.m_vSectionBoxes.push_back(Column);
	CUIRect Label;
	Column.HSplitTop(gs_PastaHeadlineHeight, &Label, &Column);
	pMenus->MenuUi()->DoLabel(&Label, pTitle, gs_PastaHeadlineFontSize, TEXTALIGN_ML);
	Column.HSplitTop(gs_PastaMarginSmall, nullptr, &Column);
	return State.m_vSectionBoxes.size() - 1;
}

static void FinishPastaSection(CUIRect &Column, CPastaScrollState &State, size_t Index)
{
	State.m_vSectionBoxes[Index].h = Column.y - State.m_vSectionBoxes[Index].y;
}

static void DoPastaCheckBoxes(CMenus *pMenus, CUIRect &View, const SBoolEntry *pEntries, size_t Count)
{
	for(size_t i = 0; i < Count; ++i)
		pMenus->DoButton_CheckBoxAutoVMarginAndSet(pEntries[i].m_pValue, pEntries[i].m_pLabel, pEntries[i].m_pValue, &View, gs_PastaLineSize);
}

static void DoPastaSlider(CMenus *pMenus, CUIRect &View, int *pValue, const char *pLabel, int Min, int Max, const char *pSuffix)
{
	CUIRect Button;
	View.HSplitTop(gs_PastaLineSize, &Button, &View);
	pMenus->MenuUi()->DoScrollbarOption(pValue, pValue, &Button, pLabel, Min, Max, &CUi::ms_LinearScrollbarScale, 0, pSuffix);
}

static bool DoPastaButton(CMenus *pMenus, CUIRect &View, CButtonContainer &ButtonId, const char *pLabel, ColorRGBA Color)
{
	CUIRect Button;
	View.HSplitTop(gs_PastaLineSize, &Button, &View);
	if(g_Config.m_PastaCustomTheme && Color.r == 1.0f && Color.g == 1.0f && Color.b == 1.0f)
		Color = GetPastaPanelColor(0.20f, 0.62f);
	return pMenus->DoButtonLineSize_MenuHelper(&ButtonId, pLabel, 0, &Button, gs_PastaLineSize, false, nullptr, IGraphics::CORNER_ALL, 5.0f, 0.0f, Color) != 0;
}

static void DoPastaEditBox(CMenus *pMenus, CUIRect &View, CLineInput *pInput, const char *pLabel, const char *pDefault, char *pBuf, size_t BufSize)
{
	CUIRect Row;
	View.HSplitTop(gs_PastaLineSize, &Row, &View);
	pMenus->DoEditBoxWithLabel(pInput, &Row, pLabel, pDefault, pBuf, BufSize);
}

static void DoPastaColorPicker(CMenus *pMenus, CUIRect &View, CButtonContainer &ResetId, const char *pLabel, unsigned *pColorValue, ColorRGBA DefaultColor)
{
	pMenus->DoLine_ColorPickerHelper(&ResetId, gs_PastaColorPickerLineSize, gs_PastaColorPickerLabelSize, gs_PastaColorPickerSpacing, &View, pLabel, pColorValue, DefaultColor, false);
}

static void RenderPastaWeaponSection(CMenus *pMenus, CUIRect &Column, CPastaScrollState &State, const SPastaWeaponConfig &Weapon, bool First)
{
	if(!First)
		Column.HSplitTop(gs_PastaSectionSpacing, nullptr, &Column);
	const size_t SectionIndex = BeginPastaSection(pMenus, Column, State, Weapon.m_pTitle);

	pMenus->DoButton_CheckBoxAutoVMarginAndSet(Weapon.m_pEnable, Localize("Enable"), Weapon.m_pEnable, &Column, gs_PastaLineSize);
	if(*Weapon.m_pEnable)
	{
		DoPastaSlider(pMenus, Column, Weapon.m_pFov, Localize("FOV"), 1, 360);
		static std::array<const char *, 2> s_aPriorityNames = {Localize("Closest to Crosshair"), Localize("Closest to Player")};
		static CUi::SDropDownState s_aPriorityStates[6];
		static CScrollRegion s_aPriorityScrollRegions[6];
		const int WeaponIndex =
			Weapon.m_pEnable == &g_Config.m_PastaAimbotHook ? 0 :
			Weapon.m_pEnable == &g_Config.m_PastaAimbotHammer ? 1 :
			Weapon.m_pEnable == &g_Config.m_PastaAimbotPistol ? 2 :
			Weapon.m_pEnable == &g_Config.m_PastaAimbotShotgun ? 3 :
			Weapon.m_pEnable == &g_Config.m_PastaAimbotGrenade ? 4 : 5;
		DoPastaDropDown(pMenus, Column, Localize("Target Priority"), *Weapon.m_pPriority, s_aPriorityNames, s_aPriorityStates[WeaponIndex], s_aPriorityScrollRegions[WeaponIndex]);
		pMenus->DoButton_CheckBoxAutoVMarginAndSet(Weapon.m_pSilent, Localize("Silent"), Weapon.m_pSilent, &Column, gs_PastaLineSize);
		pMenus->DoButton_CheckBoxAutoVMarginAndSet(Weapon.m_pIgnoreFriends, Localize("Ignore Friends"), Weapon.m_pIgnoreFriends, &Column, gs_PastaLineSize);
		if(Weapon.m_pAccuracy != nullptr)
			DoPastaSlider(pMenus, Column, Weapon.m_pAccuracy, Localize("Accuracy"), 1, 20);
	}

	FinishPastaSection(Column, State, SectionIndex);
}

static void RenderPastaGradientSection(CMenus *pMenus, CUIRect &Column, CPastaScrollState &State, const char *pTitle, int *pEnable, int *pSegmented, int *pSpeed, unsigned *pStartColor, unsigned *pEndColor, int *pRainbow, bool First)
{
	if(!First)
		Column.HSplitTop(gs_PastaSectionSpacing, nullptr, &Column);
	const size_t SectionIndex = BeginPastaSection(pMenus, Column, State, pTitle);

	pMenus->DoButton_CheckBoxAutoVMarginAndSet(pEnable, Localize("Enable"), pEnable, &Column, gs_PastaLineSize);
	if(*pEnable)
	{
		pMenus->DoButton_CheckBoxAutoVMarginAndSet(pSegmented, Localize("Segmented"), pSegmented, &Column, gs_PastaLineSize);
		DoPastaSlider(pMenus, Column, pSpeed, Localize("Speed"), 0, 20);
		static CButtonContainer s_aStartIds[3];
		static CButtonContainer s_aEndIds[3];
		const int Index = pEnable == &g_Config.m_PastaWatermark ? 0 : pEnable == &g_Config.m_PastaNowPlayingHud ? 1 : 2;
		DoPastaColorPicker(pMenus, Column, s_aStartIds[Index], Localize("Start"), pStartColor, ColorRGBA(0.0f, 1.0f, 0.0f));
		DoPastaColorPicker(pMenus, Column, s_aEndIds[Index], Localize("End"), pEndColor, ColorRGBA(0.0f, 0.5f, 0.0f));
		pMenus->DoButton_CheckBoxAutoVMarginAndSet(pRainbow, Localize("Rainbow"), pRainbow, &Column, gs_PastaLineSize);
	}

	FinishPastaSection(Column, State, SectionIndex);
}

void RenderPastaAimbot(CMenus *pMenus, CUIRect MainView)
{
	static CPastaScrollState s_ScrollState;
	CUIRect LeftView, RightView;
	BeginPastaScrollColumns(MainView, s_ScrollState, LeftView, RightView);

	CUIRect Column = LeftView;
	const size_t MainSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Main"));
	static const SBoolEntry s_aMainChecks[] = {
		{Localize("Enable"), &g_Config.m_PastaAimbot},
		{Localize("Draw FOV"), &g_Config.m_PastaAimbotDrawFov},
		{Localize("Target Box"), &g_Config.m_PastaAimbotTargetBox},
		{Localize("Target Glow"), &g_Config.m_PastaAimbotTargetGlow},
		{Localize("Auto Shoot"), &g_Config.m_PastaAimbotAutoShoot},
		{Localize("Perfect Silent"), &g_Config.m_PastaAimbotPerfectSilent},
		{Localize("Advanced Settings"), &g_Config.m_PastaAimbotAdvancedSettings},
	};
	DoPastaCheckBoxes(pMenus, Column, s_aMainChecks, std::size(s_aMainChecks));

	static std::array<const char *, 2> s_aPriorityNames = {Localize("Closest to Crosshair"), Localize("Closest to Player")};
	static CUi::SDropDownState s_GlobalPriorityState;
	static CScrollRegion s_GlobalPriorityScrollRegion;
	DoPastaDropDown(pMenus, Column, Localize("Target Priority"), g_Config.m_PastaAimbotTargetPriority, s_aPriorityNames, s_GlobalPriorityState, s_GlobalPriorityScrollRegion);
	if(g_Config.m_PastaAimbotAdvancedSettings)
	{
		pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaAimbotGrenadeMovePrediction, Localize("Grenade Move Prediction"), &g_Config.m_PastaAimbotGrenadeMovePrediction, &Column, gs_PastaLineSize);
		DoPastaSlider(pMenus, Column, &g_Config.m_PastaAimbotEdgeAccuracy, Localize("Edge Accuracy"), 1, 20);
		DoPastaSlider(pMenus, Column, &g_Config.m_PastaAimbotMinShootDelay, Localize("Min Shoot Delay"), 1, 30);
		DoPastaSlider(pMenus, Column, &g_Config.m_PastaAimbotMaxShootDelay, Localize("Max Shoot Delay"), 1, 30);
	}
	FinishPastaSection(Column, s_ScrollState, MainSection);

	Column.HSplitTop(gs_PastaSectionSpacing, nullptr, &Column);
	const size_t HotkeySection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Hotkeys"));
	static CButtonContainer s_AimbotReader;
	static CButtonContainer s_AimbotClear;
	static CButtonContainer s_AutoHookReader;
	static CButtonContainer s_AutoHookClear;
	static CButtonContainer s_AutoShootReader;
	static CButtonContainer s_AutoShootClear;
	pMenus->DoLine_KeyReader(Column, s_AimbotReader, s_AimbotClear, "Aimbot", "toggle pasta_aimbot 1 0");
	pMenus->DoLine_KeyReader(Column, s_AutoHookReader, s_AutoHookClear, "Aimbot Auto Hook", "+toggle pasta_aimbot_autohook_key 1 0");
	pMenus->DoLine_KeyReader(Column, s_AutoShootReader, s_AutoShootClear, "Aimbot Auto Shoot", "+toggle pasta_aimbot_autoshoot_key 1 0");
	FinishPastaSection(Column, s_ScrollState, HotkeySection);

	Column = RightView;
	const SPastaWeaponConfig aWeapons[] = {
		{"Hook", &g_Config.m_PastaAimbotHook, &g_Config.m_PastaAimbotHookFov, &g_Config.m_PastaAimbotHookTargetPriority, &g_Config.m_PastaAimbotHookSilent, &g_Config.m_PastaAimbotHookIgnoreFriends, &g_Config.m_PastaAimbotHookAccuracy},
		{"Hammer", &g_Config.m_PastaAimbotHammer, &g_Config.m_PastaAimbotHammerFov, &g_Config.m_PastaAimbotHammerTargetPriority, &g_Config.m_PastaAimbotHammerSilent, &g_Config.m_PastaAimbotHammerIgnoreFriends, nullptr},
		{"Pistol", &g_Config.m_PastaAimbotPistol, &g_Config.m_PastaAimbotPistolFov, &g_Config.m_PastaAimbotPistolTargetPriority, &g_Config.m_PastaAimbotPistolSilent, &g_Config.m_PastaAimbotPistolIgnoreFriends, nullptr},
		{"Shotgun", &g_Config.m_PastaAimbotShotgun, &g_Config.m_PastaAimbotShotgunFov, &g_Config.m_PastaAimbotShotgunTargetPriority, &g_Config.m_PastaAimbotShotgunSilent, &g_Config.m_PastaAimbotShotgunIgnoreFriends, nullptr},
		{"Grenade", &g_Config.m_PastaAimbotGrenade, &g_Config.m_PastaAimbotGrenadeFov, &g_Config.m_PastaAimbotGrenadeTargetPriority, &g_Config.m_PastaAimbotGrenadeSilent, &g_Config.m_PastaAimbotGrenadeIgnoreFriends, nullptr},
		{"Laser", &g_Config.m_PastaAimbotLaser, &g_Config.m_PastaAimbotLaserFov, &g_Config.m_PastaAimbotLaserTargetPriority, &g_Config.m_PastaAimbotLaserSilent, &g_Config.m_PastaAimbotLaserIgnoreFriends, nullptr},
	};
	for(size_t i = 0; i < std::size(aWeapons); ++i)
		RenderPastaWeaponSection(pMenus, Column, s_ScrollState, aWeapons[i], i == 0);

	EndPastaScrollColumns(MainView, s_ScrollState, LeftView, RightView);
}

void RenderPastaMisc(CMenus *pMenus, CUIRect MainView)
{
	static CPastaScrollState s_ScrollState;
	CUIRect LeftView, RightView;
	BeginPastaScrollColumns(MainView, s_ScrollState, LeftView, RightView);

	CUIRect Column = LeftView;
	const size_t BotsSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Bots"));
	static const SBoolEntry s_aBotChecks[] = {
		{Localize("Moonwalk"), &g_Config.m_PastaMoonwalk},
		{Localize("Auto Fire"), &g_Config.m_PastaAutoFire},
		{Localize("Auto Rehook"), &g_Config.m_PastaAutoRehook},
		{Localize("Auto Jump Save"), &g_Config.m_PastaAutoJumpSave},
		{Localize("Quick Stop"), &g_Config.m_PastaQuickStop},
		{Localize("Avoid Freeze (Basic)"), &g_Config.m_PastaAvoidfreeze},
		{Localize("Jet Ride"), &g_Config.m_PastaJetRide},
		{Localize("Auto Aled"), &g_Config.m_PastaAutoAled},
	};
	DoPastaCheckBoxes(pMenus, Column, s_aBotChecks, std::size(s_aBotChecks));
	FinishPastaSection(Column, s_ScrollState, BotsSection);

	Column.HSplitTop(gs_PastaSectionSpacing, nullptr, &Column);
	const size_t ProtectionSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Protections"));
	static const SBoolEntry s_aProtectionChecks[] = {
		{Localize("Random Timeout Seed"), &g_Config.m_PastaRandomTimeoutSeed},
		{Localize("Version Spoofer"), &g_Config.m_PastaSpoofVersion},
		{Localize("Advanced Options"), &g_Config.m_PastaSpoofVersionAdvancedSettings},
	};
	DoPastaCheckBoxes(pMenus, Column, s_aProtectionChecks, std::size(s_aProtectionChecks));
	if(g_Config.m_PastaSpoofVersion || g_Config.m_PastaSpoofVersionAdvancedSettings)
		DoPastaSlider(pMenus, Column, &g_Config.m_PastaSpoofVersionNr, Localize("Version Nr"), 0, 99999);
	FinishPastaSection(Column, s_ScrollState, ProtectionSection);

	Column.HSplitTop(gs_PastaSectionSpacing, nullptr, &Column);
	const size_t ModSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Mod Detector"));
	pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaModDetector, Localize("Enable"), &g_Config.m_PastaModDetector, &Column, gs_PastaLineSize);
	if(g_Config.m_PastaModDetector)
	{
		static const SBoolEntry s_aModChecks[] = {
			{Localize("Detect by Names"), &g_Config.m_PastaModDetectorNames},
			{Localize("Detect Suspicious Players"), &g_Config.m_PastaModDetectorWarn},
			{Localize("Leave on Mod Detection"), &g_Config.m_PastaModDetectorLeave},
			{Localize("Leave on Warn Detected"), &g_Config.m_PastaModDetectorWarnLeave},
		};
		DoPastaCheckBoxes(pMenus, Column, s_aModChecks, std::size(s_aModChecks));
	}
	FinishPastaSection(Column, s_ScrollState, ModSection);

	Column.HSplitTop(gs_PastaSectionSpacing, nullptr, &Column);
	const size_t EdgeSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Auto Edge"));
	static const SBoolEntry s_aEdgeChecks[] = {
		{Localize("Enable"), &g_Config.m_PastaAutoEdge},
		{Localize("Detect Freeze"), &g_Config.m_PastaAutoEdgeDetectFreeze},
		{Localize("Detect Death"), &g_Config.m_PastaAutoEdgeDetectDeath},
		{Localize("Detect Teleporters"), &g_Config.m_PastaAutoEdgeDetectTele},
		{Localize("Show Found Edges"), &g_Config.m_PastaAutoEdgeShowFound},
		{Localize("Show Locked Edge"), &g_Config.m_PastaAutoEdgeShowLocked},
		{Localize("Advanced Settings"), &g_Config.m_PastaAutoEdgeAdvancedSettings},
	};
	DoPastaCheckBoxes(pMenus, Column, s_aEdgeChecks, std::size(s_aEdgeChecks));
	if(g_Config.m_PastaAutoEdgeAdvancedSettings)
	{
		DoPastaSlider(pMenus, Column, &g_Config.m_PastaAutoEdgeDetectionRange, Localize("Detection Range"), 1, 40);
		DoPastaSlider(pMenus, Column, &g_Config.m_PastaAutoEdgePopulationSize, Localize("Population Size"), 10, 1000);
		DoPastaSlider(pMenus, Column, &g_Config.m_PastaAutoEdgeExplorationDepth, Localize("Exploration Depth"), 1, 40);
		DoPastaSlider(pMenus, Column, &g_Config.m_PastaAutoEdgeTopK, Localize("Top-K Candidates"), 1, 128);
		DoPastaSlider(pMenus, Column, &g_Config.m_PastaAutoEdgeSequenceLength, Localize("Sequence Length"), 1, 32);
	}
	FinishPastaSection(Column, s_ScrollState, EdgeSection);

	Column.HSplitTop(gs_PastaSectionSpacing, nullptr, &Column);
	const size_t ShotgunSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Auto Shotgun"));
	pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaAutoShotgun, Localize("Enable"), &g_Config.m_PastaAutoShotgun, &Column, gs_PastaLineSize);
	if(g_Config.m_PastaAutoShotgun)
	{
		pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaAutoShotgunAdvancedSettings, Localize("Advanced Settings"), &g_Config.m_PastaAutoShotgunAdvancedSettings, &Column, gs_PastaLineSize);
		if(g_Config.m_PastaAutoShotgunAdvancedSettings)
		{
			static std::array<const char *, 2> s_aBounceNames = {Localize("Least"), Localize("Most")};
			static std::array<const char *, 2> s_aSpeedNames = {Localize("Least Speed"), Localize("Most Speed")};
			static CUi::SDropDownState s_BounceState;
			static CUi::SDropDownState s_SpeedState;
			static CScrollRegion s_BounceScroll;
			static CScrollRegion s_SpeedScroll;
			DoPastaDropDown(pMenus, Column, Localize("Bounces"), g_Config.m_PastaAutoShotgunMostBounces, s_aBounceNames, s_BounceState, s_BounceScroll);
			DoPastaDropDown(pMenus, Column, Localize("Speed"), g_Config.m_PastaAutoShotgunHighestVel, s_aSpeedNames, s_SpeedState, s_SpeedScroll);
			pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaAutoShotgunSilent, Localize("Silent"), &g_Config.m_PastaAutoShotgunSilent, &Column, gs_PastaLineSize);
			DoPastaSlider(pMenus, Column, &g_Config.m_PastaAutoShotgunPoints, Localize("Points"), 1, 64);
			DoPastaSlider(pMenus, Column, &g_Config.m_PastaAutoShotgunTicks, Localize("Ticks"), 1, 64);
			DoPastaSlider(pMenus, Column, &g_Config.m_PastaAutoShotgunFov, Localize("FOV"), 1, 360);
		}
	}
	FinishPastaSection(Column, s_ScrollState, ShotgunSection);

	Column.HSplitTop(gs_PastaSectionSpacing, nullptr, &Column);
	const size_t UnfreezeSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Auto Unfreeze"));
	pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaUnfreezeBot, Localize("Enable"), &g_Config.m_PastaUnfreezeBot, &Column, gs_PastaLineSize);
	if(g_Config.m_PastaUnfreezeBot)
	{
		pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaUnfreezeBotEsp, Localize("ESP"), &g_Config.m_PastaUnfreezeBotEsp, &Column, gs_PastaLineSize);
		pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaUnfreezeBotAdvancedSettings, Localize("Advanced Settings"), &g_Config.m_PastaUnfreezeBotAdvancedSettings, &Column, gs_PastaLineSize);
		if(g_Config.m_PastaUnfreezeBotAdvancedSettings)
		{
			DoPastaSlider(pMenus, Column, &g_Config.m_PastaUnfreezeBotPoints, Localize("Points"), 1, 64);
			DoPastaSlider(pMenus, Column, &g_Config.m_PastaUnfreezeBotCurDirTicks, Localize("Current Dir Ticks"), 1, 64);
			DoPastaSlider(pMenus, Column, &g_Config.m_PastaUnfreezeBotTicks, Localize("Ticks"), 1, 64);
			DoPastaSlider(pMenus, Column, &g_Config.m_PastaUnfreezeBotFov, Localize("FOV"), 1, 360);
		}
	}
	FinishPastaSection(Column, s_ScrollState, UnfreezeSection);

	Column.HSplitTop(gs_PastaSectionSpacing, nullptr, &Column);
	const size_t BenchmarkSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Prediction Benchmark"));
	static CButtonContainer s_BenchmarkButton;
	if(DoPastaButton(pMenus, Column, s_BenchmarkButton, g_Config.m_PastaPredictionBenchmark ? "Stop Benchmark" : "Start Benchmark", g_Config.m_PastaPredictionBenchmark ? ColorRGBA(0.8f, 0.1f, 0.1f, 0.85f) : ColorRGBA(1.0f, 1.0f, 1.0f, 0.25f)))
		g_Config.m_PastaPredictionBenchmark ^= 1;
	DoPastaSlider(pMenus, Column, &g_Config.m_PastaPredictionBenchmarkRuns, Localize("Runs"), 1, 1000);
	DoPastaSlider(pMenus, Column, &g_Config.m_PastaPredictionBenchmarkThreads, Localize("Threads"), 1, 128);
	FinishPastaSection(Column, s_ScrollState, BenchmarkSection);

	Column = RightView;
	const size_t OtherSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Other"));
	static const SBoolEntry s_aOtherChecks[] = {
		{Localize("Auto Team"), &g_Config.m_PastaAutoTeam},
		{Localize("Anti AFK"), &g_Config.m_PastaAntiAfk},
		{Localize("Kill on Freeze"), &g_Config.m_PastaKillOnFreeze},
		{Localize("Ignore Replay Warnings"), &g_Config.m_PastaIgnoreReplayWarnings},
		{Localize("Hide Chat Bubble"), &g_Config.m_PastaHideBubble},
	};
	DoPastaCheckBoxes(pMenus, Column, s_aOtherChecks, std::size(s_aOtherChecks));
	FinishPastaSection(Column, s_ScrollState, OtherSection);

	Column.HSplitTop(gs_PastaSectionSpacing, nullptr, &Column);
	const size_t TrollSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Troll"));
	static const SBoolEntry s_aTrollChecks[] = {
		{Localize("Emote Spam"), &g_Config.m_PastaEmoteSpam},
		{Localize("Killsay/Deathsays"), &g_Config.m_PastaKillsay},
		{Localize("Fancy Chat Font"), &g_Config.m_PastaFancyChatFont},
		{Localize("Mass Mention Spam"), &g_Config.m_PastaMassMentionSpam},
		{Localize("Chat Repeater"), &g_Config.m_PastaChatRepeater},
	};
	DoPastaCheckBoxes(pMenus, Column, s_aTrollChecks, std::size(s_aTrollChecks));
	if(g_Config.m_PastaChatRepeater)
		DoPastaSlider(pMenus, Column, &g_Config.m_PastaChatRepeaterLength, Localize("Min Length"), 1, 256);
	FinishPastaSection(Column, s_ScrollState, TrollSection);

	Column.HSplitTop(gs_PastaSectionSpacing, nullptr, &Column);
	const size_t IdSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("ID Stealer"));
	pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaIdStealer, Localize("Enable"), &g_Config.m_PastaIdStealer, &Column, gs_PastaLineSize);
	if(g_Config.m_PastaIdStealer)
	{
		static const SBoolEntry s_aIdChecks[] = {
			{Localize("Closest Player"), &g_Config.m_PastaIdStealerClosest},
			{Localize("Steal Name"), &g_Config.m_PastaIdStealerName},
			{Localize("Steal Clan"), &g_Config.m_PastaIdStealerClan},
			{Localize("Steal Skin"), &g_Config.m_PastaIdStealerSkin},
			{Localize("Steal Flag"), &g_Config.m_PastaIdStealerFlag},
			{Localize("Steal Eye Emote"), &g_Config.m_PastaIdStealerEmote},
		};
		DoPastaCheckBoxes(pMenus, Column, s_aIdChecks, std::size(s_aIdChecks));
		DoPastaSlider(pMenus, Column, &g_Config.m_PastaIdStealerSpeed, Localize("Stealer Speed"), 1, 30, "s");
	}
	FinishPastaSection(Column, s_ScrollState, IdSection);

	Column.HSplitTop(gs_PastaSectionSpacing, nullptr, &Column);
	const size_t GhostSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Ghost Move"));
	pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaGhostMove, Localize("Enable"), &g_Config.m_PastaGhostMove, &Column, gs_PastaLineSize);
	if(g_Config.m_PastaGhostMove)
	{
		static const SBoolEntry s_aGhostChecks[] = {
			{Localize("Direction"), &g_Config.m_PastaGhostMoveDirection},
			{Localize("Jump"), &g_Config.m_PastaGhostMoveJump},
			{Localize("Hook"), &g_Config.m_PastaGhostMoveHook},
			{Localize("Hook Closest"), &g_Config.m_PastaGhostMoveHookClosest},
		};
		DoPastaCheckBoxes(pMenus, Column, s_aGhostChecks, std::size(s_aGhostChecks));
	}
	FinishPastaSection(Column, s_ScrollState, GhostSection);

	Column.HSplitTop(gs_PastaSectionSpacing, nullptr, &Column);
	const size_t DummySection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Dummy Fly"));
	pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaDummyFly, Localize("Enable"), &g_Config.m_PastaDummyFly, &Column, gs_PastaLineSize);
	FinishPastaSection(Column, s_ScrollState, DummySection);

	Column.HSplitTop(gs_PastaSectionSpacing, nullptr, &Column);
	const size_t AutoVoteSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Auto Vote"));
	pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaAutoVote, Localize("Enable"), &g_Config.m_PastaAutoVote, &Column, gs_PastaLineSize);
	if(g_Config.m_PastaAutoVote)
	{
		static std::array<const char *, 2> s_aVoteModes = {Localize("Vote Yes (F3)"), Localize("Vote No (F4)")};
		static CUi::SDropDownState s_VoteModeState;
		static CScrollRegion s_VoteModeScroll;
		DoPastaDropDown(pMenus, Column, Localize("Vote"), g_Config.m_PastaAutoVoteF4, s_aVoteModes, s_VoteModeState, s_VoteModeScroll);
	}
	FinishPastaSection(Column, s_ScrollState, AutoVoteSection);

	Column.HSplitTop(gs_PastaSectionSpacing, nullptr, &Column);
	const size_t AutoVoteKickSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Auto Vote Kick"));
	pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaAutoVoteKick, Localize("Enable"), &g_Config.m_PastaAutoVoteKick, &Column, gs_PastaLineSize);
	if(g_Config.m_PastaAutoVoteKick)
	{
		static std::array<const char *, 2> s_aKickModes = {Localize("Random Target"), Localize("Specific ID")};
		static CUi::SDropDownState s_KickModeState;
		static CScrollRegion s_KickModeScroll;
		DoPastaDropDown(pMenus, Column, Localize("Mode"), g_Config.m_PastaAutoVoteKickMode, s_aKickModes, s_KickModeState, s_KickModeScroll);
		if(g_Config.m_PastaAutoVoteKickMode == 1)
			DoPastaSlider(pMenus, Column, &g_Config.m_PastaAutoVoteKickTarget, Localize("Target ID"), 0, 63);
		static CLineInput s_KickReasonInput(g_Config.m_PastaAutoVoteKickReason, sizeof(g_Config.m_PastaAutoVoteKickReason));
		DoPastaEditBox(pMenus, Column, &s_KickReasonInput, "Kick Reason", "pasta detected", g_Config.m_PastaAutoVoteKickReason, sizeof(g_Config.m_PastaAutoVoteKickReason));
	}
	FinishPastaSection(Column, s_ScrollState, AutoVoteKickSection);

	Column.HSplitTop(gs_PastaSectionSpacing, nullptr, &Column);
	const size_t FakeAimSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Fake Aim"));
	pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaFakeAim, Localize("Enable"), &g_Config.m_PastaFakeAim, &Column, gs_PastaLineSize);
	if(g_Config.m_PastaFakeAim)
	{
		static const SBoolEntry s_aFakeAimChecks[] = {
			{Localize("Send Always"), &g_Config.m_PastaFakeAimSendAlways},
			{Localize("Visible"), &g_Config.m_PastaFakeAimVisible},
		};
		DoPastaCheckBoxes(pMenus, Column, s_aFakeAimChecks, std::size(s_aFakeAimChecks));
		static std::array<const char *, 6> s_aFakeAimModes = {
			Localize("Mouse Pos"),
			Localize("Robot Aim"),
			Localize("Spinbot"),
			Localize("Random"),
			Localize("Fake Angles"),
			Localize("Aimbot Troll Aim"),
		};
		static CUi::SDropDownState s_FakeAimModeState;
		static CScrollRegion s_FakeAimModeScroll;
		DoPastaDropDown(pMenus, Column, Localize("Mode"), g_Config.m_PastaFakeAimMode, s_aFakeAimModes, s_FakeAimModeState, s_FakeAimModeScroll);
		if(g_Config.m_PastaFakeAimMode == 2 || g_Config.m_PastaFakeAimMode == 3)
			DoPastaSlider(pMenus, Column, &g_Config.m_PastaFakeAimSpeed, Localize("Speed"), 1, 50);
	}
	FinishPastaSection(Column, s_ScrollState, FakeAimSection);

	EndPastaScrollColumns(MainView, s_ScrollState, LeftView, RightView);
}

void RenderPastaVisuals(CMenus *pMenus, CUIRect MainView)
{
	static CPastaScrollState s_ScrollState;
	CUIRect LeftView, RightView;
	BeginPastaScrollColumns(MainView, s_ScrollState, LeftView, RightView);

	CUIRect Column = LeftView;
	const size_t HudSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("HUD"));
	pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaFeaturesHud, Localize("Features HUD"), &g_Config.m_PastaFeaturesHud, &Column, gs_PastaLineSize);
	if(g_Config.m_PastaFeaturesHud)
	{
		pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaFeaturesHudSegmented, Localize("Segmented"), &g_Config.m_PastaFeaturesHudSegmented, &Column, gs_PastaLineSize);
		DoPastaSlider(pMenus, Column, &g_Config.m_PastaFeaturesHudOpacity, Localize("Opacity"), 0, 100);
		DoPastaSlider(pMenus, Column, &g_Config.m_PastaFeaturesHudFontSize, Localize("Font Size"), 1, 20);
		DoPastaSlider(pMenus, Column, &g_Config.m_PastaFeaturesHudGradientSpeed, Localize("Speed"), 0, 20);
		static CButtonContainer s_HudStartReset;
		static CButtonContainer s_HudEndReset;
		DoPastaColorPicker(pMenus, Column, s_HudStartReset, Localize("Start"), &g_Config.m_PastaFeaturesHudGradientStartCol, ColorRGBA(0.0f, 1.0f, 0.0f));
		DoPastaColorPicker(pMenus, Column, s_HudEndReset, Localize("End"), &g_Config.m_PastaFeaturesHudGradientEndCol, ColorRGBA(0.0f, 0.5f, 0.0f));
		pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaFeaturesHudGradientRainbow, Localize("Rainbow"), &g_Config.m_PastaFeaturesHudGradientRainbow, &Column, gs_PastaLineSize);
	}
	pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaNotifications, Localize("Notifications"), &g_Config.m_PastaNotifications, &Column, gs_PastaLineSize);
	pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaTasHud, Localize("TAS HUD"), &g_Config.m_PastaTasHud, &Column, gs_PastaLineSize);
	FinishPastaSection(Column, s_ScrollState, HudSection);

	RenderPastaGradientSection(pMenus, Column, s_ScrollState, Localize("Watermark"), &g_Config.m_PastaWatermark, &g_Config.m_PastaWatermarkSegmented, &g_Config.m_PastaWatermarkGradientSpeed, &g_Config.m_PastaWatermarkGradientStartCol, &g_Config.m_PastaWatermarkGradientEndCol, &g_Config.m_PastaWatermarkGradientRainbow, false);
	RenderPastaGradientSection(pMenus, Column, s_ScrollState, Localize("Now Playing HUD"), &g_Config.m_PastaNowPlayingHud, &g_Config.m_PastaNowPlayingSegmented, &g_Config.m_PastaNowPlayingGradientSpeed, &g_Config.m_PastaNowPlayingGradientStartCol, &g_Config.m_PastaNowPlayingGradientEndCol, &g_Config.m_PastaNowPlayingGradientRainbow, false);

	Column.HSplitTop(gs_PastaSectionSpacing, nullptr, &Column);
	const size_t CustomMenuSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Custom Menu"));
	pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaCustomTheme, Localize("Enable"), &g_Config.m_PastaCustomTheme, &Column, gs_PastaLineSize);
	if(g_Config.m_PastaCustomTheme)
	{
		static CButtonContainer s_AccentReset;
		static CButtonContainer s_BgReset;
		DoPastaColorPicker(pMenus, Column, s_AccentReset, Localize("Accent Color"), &g_Config.m_PastaCustomAccentColor, ColorRGBA(0.0f, 1.0f, 0.0f));
		DoPastaColorPicker(pMenus, Column, s_BgReset, Localize("Background Color"), &g_Config.m_PastaCustomBgColor, ColorRGBA(0.1f, 0.1f, 0.1f));
		pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaRainbowMenu, Localize("Rainbow Menu"), &g_Config.m_PastaRainbowMenu, &Column, gs_PastaLineSize);
	}
	FinishPastaSection(Column, s_ScrollState, CustomMenuSection);

	Column = RightView;
	const size_t AimLinesSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Aim Lines"));
	pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaAimLines, Localize("Draw Aim Lines"), &g_Config.m_PastaAimLines, &Column, gs_PastaLineSize);
	if(g_Config.m_PastaAimLines)
	{
		static const SBoolEntry s_aAimChecks[] = {
			{Localize("Self Only"), &g_Config.m_PastaAimLinesSelfOnly},
			{Localize("Pistol"), &g_Config.m_PastaAimLinesGun},
			{Localize("Shotgun"), &g_Config.m_PastaAimLinesShotgun},
			{Localize("Grenade"), &g_Config.m_PastaAimLinesGrenade},
			{Localize("Laser"), &g_Config.m_PastaAimLinesLaser},
		};
		DoPastaCheckBoxes(pMenus, Column, s_aAimChecks, std::size(s_aAimChecks));
		DoPastaSlider(pMenus, Column, &g_Config.m_PastaAimLinesAlpha, Localize("Alpha"), 0, 100);
	}
	FinishPastaSection(Column, s_ScrollState, AimLinesSection);

	Column.HSplitTop(gs_PastaSectionSpacing, nullptr, &Column);
	const size_t BulletLinesSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Bullet Lines"));
	pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaBulletLines, Localize("Draw Bullet Lines"), &g_Config.m_PastaBulletLines, &Column, gs_PastaLineSize);
	if(g_Config.m_PastaBulletLines)
	{
		static const SBoolEntry s_aBulletChecks[] = {
			{Localize("Self Only"), &g_Config.m_PastaBulletLinesSelfOnly},
			{Localize("Pistol"), &g_Config.m_PastaBulletLinesGun},
			{Localize("Shotgun"), &g_Config.m_PastaBulletLinesShotgun},
			{Localize("Grenade"), &g_Config.m_PastaBulletLinesGrenade},
		};
		DoPastaCheckBoxes(pMenus, Column, s_aBulletChecks, std::size(s_aBulletChecks));
		DoPastaSlider(pMenus, Column, &g_Config.m_PastaBulletLinesAlpha, Localize("Alpha"), 0, 100);
	}
	FinishPastaSection(Column, s_ScrollState, BulletLinesSection);

	Column.HSplitTop(gs_PastaSectionSpacing, nullptr, &Column);
	const size_t CustomColorsSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Custom Colors"));
	static const SBoolEntry s_aColorChecks[] = {
		{Localize("Tee"), &g_Config.m_PastaCustomColorTee},
		{Localize("Hook"), &g_Config.m_PastaCustomColorHook},
		{Localize("Weapon"), &g_Config.m_PastaCustomColorWeapon},
		{Localize("Rainbow Tee"), &g_Config.m_PastaCustomColorTeeRainbow},
		{Localize("Rainbow Hook"), &g_Config.m_PastaCustomColorHookRainbow},
	};
	DoPastaCheckBoxes(pMenus, Column, s_aColorChecks, std::size(s_aColorChecks));
	DoPastaSlider(pMenus, Column, &g_Config.m_PastaCustomColorRainbowSpeed, Localize("Rainbow Speed"), 1, 500, "%");
	FinishPastaSection(Column, s_ScrollState, CustomColorsSection);

	Column.HSplitTop(gs_PastaSectionSpacing, nullptr, &Column);
	const size_t FrozenColorSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Custom Frozen Color"));
	pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaCustomFrozenColor, Localize("Enable"), &g_Config.m_PastaCustomFrozenColor, &Column, gs_PastaLineSize);
	if(g_Config.m_PastaCustomFrozenColor)
	{
		static CButtonContainer s_FrozenColorReset;
		DoPastaColorPicker(pMenus, Column, s_FrozenColorReset, Localize("Color"), &g_Config.m_PastaCustomFrozenColorValue, ColorRGBA(0.2f, 0.6f, 1.0f));
	}
	FinishPastaSection(Column, s_ScrollState, FrozenColorSection);

	Column.HSplitTop(gs_PastaSectionSpacing, nullptr, &Column);
	const size_t TrajectorySection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Trajectory"));
	pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaTrajectoryEsp, Localize("Enable"), &g_Config.m_PastaTrajectoryEsp, &Column, gs_PastaLineSize);
	if(g_Config.m_PastaTrajectoryEsp)
	{
		static std::array<const char *, 2> s_aTrajectoryModes = {Localize("Full Line"), Localize("Dotted")};
		static CUi::SDropDownState s_TrajectoryModeState;
		static CScrollRegion s_TrajectoryModeScroll;
		static CButtonContainer s_LocalTrajectoryReset;
		static CButtonContainer s_FrozenTrajectoryReset;
		static CButtonContainer s_OtherTrajectoryReset;
		pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaTrajectoryEspSelfOnly, Localize("Self Only"), &g_Config.m_PastaTrajectoryEspSelfOnly, &Column, gs_PastaLineSize);
		DoPastaSlider(pMenus, Column, &g_Config.m_PastaTrajectoryEspTicks, Localize("Ticks"), 1, 200);
		DoPastaSlider(pMenus, Column, &g_Config.m_PastaTrajectoryEspThickness, Localize("Path Thickness"), 1, 10);
		DoPastaDropDown(pMenus, Column, Localize("Path Mode"), g_Config.m_PastaTrajectoryEspMode, s_aTrajectoryModes, s_TrajectoryModeState, s_TrajectoryModeScroll);
		DoPastaColorPicker(pMenus, Column, s_LocalTrajectoryReset, Localize("Local Color"), &g_Config.m_PastaTrajectoryEspColorLocal, ColorRGBA(0.0f, 1.0f, 0.0f));
		DoPastaColorPicker(pMenus, Column, s_FrozenTrajectoryReset, Localize("Frozen Color"), &g_Config.m_PastaTrajectoryEspColorFrozen, ColorRGBA(0.0f, 1.0f, 1.0f));
		DoPastaColorPicker(pMenus, Column, s_OtherTrajectoryReset, Localize("Others Color"), &g_Config.m_PastaTrajectoryEspColorOthers, ColorRGBA(0.0f, 0.5f, 0.0f));
	}
	FinishPastaSection(Column, s_ScrollState, TrajectorySection);

	Column.HSplitTop(gs_PastaSectionSpacing, nullptr, &Column);
	const size_t TrailSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Trail"));
	pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaTrail, Localize("Enable"), &g_Config.m_PastaTrail, &Column, gs_PastaLineSize);
	if(g_Config.m_PastaTrail)
	{
		static std::array<const char *, 5> s_aTrailTypes = {Localize("Smoke"), Localize("Bullet"), Localize("Powerup"), Localize("Sparkle"), "Tater"};
		static CUi::SDropDownState s_TrailTypeState;
		static CScrollRegion s_TrailTypeScroll;
		DoPastaDropDown(pMenus, Column, Localize("Type"), g_Config.m_PastaTrailType, s_aTrailTypes, s_TrailTypeState, s_TrailTypeScroll);
		pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaTrailColorRainbow, Localize("Rainbow"), &g_Config.m_PastaTrailColorRainbow, &Column, gs_PastaLineSize);
		pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaTrailLowTaper, Localize("Low Taper"), &g_Config.m_PastaTrailLowTaper, &Column, gs_PastaLineSize);
		pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaTrailFade, Localize("Fade"), &g_Config.m_PastaTrailFade, &Column, gs_PastaLineSize);
		DoPastaSlider(pMenus, Column, &g_Config.m_PastaTrailWidth, Localize("Width"), 1, 64);
		DoPastaSlider(pMenus, Column, &g_Config.m_PastaTrailLength, Localize("Length"), 1, 128);
		DoPastaSlider(pMenus, Column, &g_Config.m_PastaTrailAlpha, Localize("Alpha"), 0, 100);
	}
	FinishPastaSection(Column, s_ScrollState, TrailSection);

	Column.HSplitTop(gs_PastaSectionSpacing, nullptr, &Column);
	const size_t CamSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Cam"));
	pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaSmoothCam, Localize("Smooth Cam"), &g_Config.m_PastaSmoothCam, &Column, gs_PastaLineSize);
	pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaSuperDyncam, Localize("Super Smooth Cam"), &g_Config.m_PastaSuperDyncam, &Column, gs_PastaLineSize);
	if(g_Config.m_PastaSmoothCam || g_Config.m_PastaSuperDyncam)
		DoPastaSlider(pMenus, Column, &g_Config.m_PastaSmoothCamSmoothness, Localize("Smoothness"), 1, 100);
	FinishPastaSection(Column, s_ScrollState, CamSection);

	EndPastaScrollColumns(MainView, s_ScrollState, LeftView, RightView);
}

void RenderPastaAvoid(CMenus *pMenus, CUIRect MainView)
{
	static std::array<const char *, 5> s_aAvoidModes = {Localize("Basic"), Localize("Legit"), Localize("Blatant"), Localize("Fent Bot"), Localize("Pilot Bot")};
	static CUi::SDropDownState s_AvoidModeState;
	static CScrollRegion s_AvoidModeScroll;
	DoPastaDropDown(pMenus, MainView, "", g_Config.m_PastaAvoidMode, s_aAvoidModes, s_AvoidModeState, s_AvoidModeScroll, 0.0f);
	MainView.HSplitTop(gs_PastaMargin + gs_PastaMarginSmall, nullptr, &MainView);

	if(g_Config.m_PastaAvoidMode == 0)
		return;

	static CPastaScrollState s_ScrollState;
	CUIRect LeftView, RightView;
	BeginPastaScrollColumns(MainView, s_ScrollState, LeftView, RightView);

	CUIRect Column = LeftView;
	const size_t MainSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Main"));
	static CButtonContainer s_StartStopButton;
	const bool IsFent = g_Config.m_PastaAvoidMode == 3;
	const char *pToggleLabel = IsFent ? (g_Config.m_PastaAvoidfreeze ? "Overdose" : "Inject Fent") : (g_Config.m_PastaAvoidfreeze ? "Stop" : "Start");
	const ColorRGBA ToggleColor = g_Config.m_PastaAvoidfreeze ? ColorRGBA(0.8f, 0.1f, 0.1f, 0.85f) : ColorRGBA(1.0f, 1.0f, 1.0f, 0.25f);
	if(DoPastaButton(pMenus, Column, s_StartStopButton, pToggleLabel, ToggleColor))
		g_Config.m_PastaAvoidfreeze ^= 1;
	Column.HSplitTop(gs_PastaMarginSmall, nullptr, &Column);

	if(g_Config.m_PastaAvoidMode == 1)
	{
		static const SBoolEntry s_aChecks[] = {
			{Localize("Player Prediction"), &g_Config.m_PastaAvoidLegitPlayerPrediction},
			{Localize("Afk Protection"), &g_Config.m_PastaAvoidLegitAfkProtection},
		};
		DoPastaCheckBoxes(pMenus, Column, s_aChecks, std::size(s_aChecks));
		if(g_Config.m_PastaAvoidLegitAfkProtection)
			DoPastaSlider(pMenus, Column, &g_Config.m_PastaAvoidLegitAfkTime, Localize("Seconds"), 1, 120);
	}
	else if(g_Config.m_PastaAvoidMode == 2)
	{
		static const SBoolEntry s_aChecks[] = {
			{Localize("Player Prediction"), &g_Config.m_PastaAvoidBlatantPlayerPrediction},
		};
		DoPastaCheckBoxes(pMenus, Column, s_aChecks, std::size(s_aChecks));
	}
	else if(g_Config.m_PastaAvoidMode == 3)
	{
		static const SBoolEntry s_aChecks[] = {
			{Localize("Player Prediction"), &g_Config.m_PastaAvoidFentPlayerPrediction},
			{Localize("Unfreeze"), &g_Config.m_PastaAvoidFentUnfreeze},
			{Localize("Advanced Settings"), &g_Config.m_PastaAvoidFentAdvancedSettings},
		};
		DoPastaCheckBoxes(pMenus, Column, s_aChecks, std::size(s_aChecks));
		if(g_Config.m_PastaAvoidFentUnfreeze)
			DoPastaSlider(pMenus, Column, &g_Config.m_PastaAvoidFentUnfreezeRadius, Localize("Radius"), 1, 20);
		if(g_Config.m_PastaAvoidFentAdvancedSettings)
		{
			DoPastaSlider(pMenus, Column, &g_Config.m_PastaAvoidFentTicks, Localize("Fent Ticks"), 10, 10000);
			DoPastaSlider(pMenus, Column, &g_Config.m_PastaAvoidFentTweakerInputs, Localize("Tweaker Inputs"), 1, 5000);
			DoPastaSlider(pMenus, Column, &g_Config.m_PastaAvoidFentTweakerTicks, Localize("Tweaker Ticks"), 1, 64);
			DoPastaSlider(pMenus, Column, &g_Config.m_PastaAvoidFentTweakerDosage, Localize("Tweaker Dosage"), 1, 1000);
		}
		else
		{
			static std::array<const char *, 3> s_aQuality = {Localize("Low"), Localize("Mid"), Localize("Max")};
			static CUi::SDropDownState s_QualityState;
			static CScrollRegion s_QualityScroll;
			DoPastaDropDown(pMenus, Column, Localize("Quality"), g_Config.m_PastaAvoidFentQualitySetting, s_aQuality, s_QualityState, s_QualityScroll);
		}
	}
	else
	{
		static const SBoolEntry s_aChecks[] = {
			{Localize("Player Prediction"), &g_Config.m_PastaAvoidPilotPlayerPrediction},
		};
		DoPastaCheckBoxes(pMenus, Column, s_aChecks, std::size(s_aChecks));
		static std::array<const char *, 3> s_aPilotModes = {Localize("Autonomous"), Localize("Follow Cursor"), Localize("Follow Player")};
		static CUi::SDropDownState s_PilotModeState;
		static CScrollRegion s_PilotModeScroll;
		DoPastaDropDown(pMenus, Column, Localize("Mode"), g_Config.m_PastaAvoidPilotMode, s_aPilotModes, s_PilotModeState, s_PilotModeScroll);
	}
	FinishPastaSection(Column, s_ScrollState, MainSection);

	if(g_Config.m_PastaAvoidMode == 1 || g_Config.m_PastaAvoidMode == 2 || g_Config.m_PastaAvoidMode == 4)
	{
		Column.HSplitTop(gs_PastaSectionSpacing, nullptr, &Column);
		const size_t SettingsSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Settings"));
		if(g_Config.m_PastaAvoidMode == 1)
		{
			DoPastaSlider(pMenus, Column, &g_Config.m_PastaAvoidLegitCheckTicks, Localize("Check Ticks"), 1, 60);
			pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaAvoidLegitHook, Localize("Hook"), &g_Config.m_PastaAvoidLegitHook, &Column, gs_PastaLineSize);
			pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaAvoidLegitDirection, Localize("Direction"), &g_Config.m_PastaAvoidLegitDirection, &Column, gs_PastaLineSize);
		}
		else if(g_Config.m_PastaAvoidMode == 2)
		{
			DoPastaSlider(pMenus, Column, &g_Config.m_PastaAvoidBlatantMaxTicks, Localize("Check Ticks"), 0, 30);
			DoPastaSlider(pMenus, Column, &g_Config.m_PastaAvoidBlatantMinTicks, Localize("Kick in Ticks"), 0, 30);
			pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaAvoidBlatantHook, Localize("Hook"), &g_Config.m_PastaAvoidBlatantHook, &Column, gs_PastaLineSize);
			pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaAvoidBlatantCanSetDir, Localize("Direction"), &g_Config.m_PastaAvoidBlatantCanSetDir, &Column, gs_PastaLineSize);
		}
		else
		{
			DoPastaSlider(pMenus, Column, &g_Config.m_PastaAvoidPilotPopulationSize, Localize("Population Size"), 1, 1000);
			DoPastaSlider(pMenus, Column, &g_Config.m_PastaAvoidPilotExplorationDepth, Localize("Exploration Depth"), 1, 50);
			DoPastaSlider(pMenus, Column, &g_Config.m_PastaAvoidPilotTopKCandidates, Localize("Top-K Candidates"), 1, 128);
			DoPastaSlider(pMenus, Column, &g_Config.m_PastaAvoidPilotSequenceLength, Localize("Sequence Length"), 1, 32);
		}
		FinishPastaSection(Column, s_ScrollState, SettingsSection);
	}

	Column = RightView;
	if(g_Config.m_PastaAvoidMode == 1)
	{
		const size_t TilesSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Tiles"));
		static const SBoolEntry s_aTiles[] = {
			{Localize("Teles"), &g_Config.m_PastaAvoidLegitTeles},
			{Localize("Death"), &g_Config.m_PastaAvoidLegitDeath},
			{Localize("Unfreeze"), &g_Config.m_PastaAvoidLegitUnfreeze},
		};
		DoPastaCheckBoxes(pMenus, Column, s_aTiles, std::size(s_aTiles));
		if(g_Config.m_PastaAvoidLegitUnfreeze)
			DoPastaSlider(pMenus, Column, &g_Config.m_PastaAvoidLegitUnfreezeTicks, Localize("Ticks"), 1, 30);
		FinishPastaSection(Column, s_ScrollState, TilesSection);

		Column.HSplitTop(gs_PastaSectionSpacing, nullptr, &Column);
		const size_t PrioritySection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Priority"));
		DoPastaSlider(pMenus, Column, &g_Config.m_PastaAvoidLegitQuality, Localize("Quality"), 1, 64);
		DoPastaSlider(pMenus, Column, &g_Config.m_PastaAvoidLegitRandomness, Localize("Randomness"), 0, 64);
		DoPastaSlider(pMenus, Column, &g_Config.m_PastaAvoidLegitDirectionPriority, Localize("Direction Priority"), 0, 1000);
		DoPastaSlider(pMenus, Column, &g_Config.m_PastaAvoidLegitHookPriority, Localize("Hook Priority"), 0, 1000);
		DoPastaSlider(pMenus, Column, &g_Config.m_PastaAvoidLegitLifePriority, Localize("Life Priority"), 0, 100);
		FinishPastaSection(Column, s_ScrollState, PrioritySection);
	}
	else if(g_Config.m_PastaAvoidMode == 2)
	{
		const size_t FreezeResetSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Freeze Reset"));
		pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaAvoidBlatantFreezeReset, Localize("Freeze Reset"), &g_Config.m_PastaAvoidBlatantFreezeReset, &Column, gs_PastaLineSize);
		DoPastaSlider(pMenus, Column, &g_Config.m_PastaAvoidBlatantFreezeResetWait, Localize("Freeze Reset Wait"), 0, 10);
		DoPastaSlider(pMenus, Column, &g_Config.m_PastaAvoidBlatantFreezeResetFriendDist, Localize("Freeze Reset Friend Radius"), 0, 30);
		FinishPastaSection(Column, s_ScrollState, FreezeResetSection);

		Column.HSplitTop(gs_PastaSectionSpacing, nullptr, &Column);
		const size_t GoresSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Gores"));
		DoPastaSlider(pMenus, Column, &g_Config.m_PastaAvoidBlatantVelocityTickFactor, Localize("Velocity Push"), 1, 50);
		DoPastaSlider(pMenus, Column, &g_Config.m_PastaAvoidBlatantAimbotFov, Localize("Avoid freeze fov"), 0, 360);
		pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaAvoidBlatantOnlyHorizontally, Localize("Avoid Freeze Only Horizontally"), &g_Config.m_PastaAvoidBlatantOnlyHorizontally, &Column, gs_PastaLineSize);
		FinishPastaSection(Column, s_ScrollState, GoresSection);
	}
	else if(g_Config.m_PastaAvoidMode == 3 || g_Config.m_PastaAvoidMode == 4)
	{
		const size_t VisualsSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Visuals"));
		if(g_Config.m_PastaAvoidMode == 3)
		{
			pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaAvoidFentRenderPath, Localize("Render Path"), &g_Config.m_PastaAvoidFentRenderPath, &Column, gs_PastaLineSize);
			pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaAvoidFentRenderPathfinding, Localize("Render Pathfinding"), &g_Config.m_PastaAvoidFentRenderPathfinding, &Column, gs_PastaLineSize);
			pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaAvoidFentSpectateScan, Localize("Spectate Scan"), &g_Config.m_PastaAvoidFentSpectateScan, &Column, gs_PastaLineSize);
			static CButtonContainer s_SaveFentReplayButton;
			Column.HSplitTop(gs_PastaMarginSmall, nullptr, &Column);
			if(DoPastaButton(pMenus, Column, s_SaveFentReplayButton, "Save Replay"))
				pMenus->PastaFentSaveReplay();
		}
		else
		{
			pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaAvoidPilotRenderPath, Localize("Render Path"), &g_Config.m_PastaAvoidPilotRenderPath, &Column, gs_PastaLineSize);
			pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaAvoidPilotRenderPathfinding, Localize("Render Pathfinding"), &g_Config.m_PastaAvoidPilotRenderPathfinding, &Column, gs_PastaLineSize);
		}
		FinishPastaSection(Column, s_ScrollState, VisualsSection);

		Column.HSplitTop(gs_PastaSectionSpacing, nullptr, &Column);
		const size_t EditorSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Tile Editor"));
		static CButtonContainer s_RecalculateButton;
		static CButtonContainer s_AutoFinishButton;
		static CButtonContainer s_AutoTunnelButton;
		static CButtonContainer s_ClearButton;
		DoPastaButton(pMenus, Column, s_RecalculateButton, "Recalculate");
		Column.HSplitTop(gs_PastaMarginSmall, nullptr, &Column);
		DoPastaButton(pMenus, Column, s_AutoFinishButton, "Auto Finish");
		Column.HSplitTop(gs_PastaMarginSmall, nullptr, &Column);
		if(g_Config.m_PastaAvoidMode == 3)
			pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaAvoidFentEnableEditor, Localize("Enable Editor"), &g_Config.m_PastaAvoidFentEnableEditor, &Column, gs_PastaLineSize);
		else
			pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaAvoidPilotEnableEditor, Localize("Enable Editor"), &g_Config.m_PastaAvoidPilotEnableEditor, &Column, gs_PastaLineSize);
		static std::array<const char *, 2> s_aTileTypes = {Localize("Tunnel"), Localize("Finish")};
		static CUi::SDropDownState s_TileTypeState;
		static CScrollRegion s_TileTypeScroll;
		if(g_Config.m_PastaAvoidMode == 3)
			DoPastaDropDown(pMenus, Column, Localize("Tile Type"), g_Config.m_PastaAvoidFentTileType, s_aTileTypes, s_TileTypeState, s_TileTypeScroll);
		else
			DoPastaDropDown(pMenus, Column, Localize("Tile Type"), g_Config.m_PastaAvoidPilotTileType, s_aTileTypes, s_TileTypeState, s_TileTypeScroll);
		Column.HSplitTop(gs_PastaMarginSmall, nullptr, &Column);
		DoPastaButton(pMenus, Column, s_AutoTunnelButton, "Auto Tunnels");
		if(g_Config.m_PastaAvoidMode == 3)
			DoPastaSlider(pMenus, Column, &g_Config.m_PastaAvoidFentAutoTunnelWidth, Localize("Auto Tunnel Width"), 1, 16);
		else
			DoPastaSlider(pMenus, Column, &g_Config.m_PastaAvoidPilotAutoTunnelWidth, Localize("Auto Tunnel Width"), 1, 16);
		Column.HSplitTop(gs_PastaMarginSmall, nullptr, &Column);
		DoPastaButton(pMenus, Column, s_ClearButton, "Clear All Tiles");
		FinishPastaSection(Column, s_ScrollState, EditorSection);
	}

	EndPastaScrollColumns(MainView, s_ScrollState, LeftView, RightView);
}

void RenderPastaTas(CMenus *pMenus, CUIRect MainView)
{
	static CPastaScrollState s_ScrollState;
	CUIRect LeftView, RightView;
	BeginPastaScrollColumns(MainView, s_ScrollState, LeftView, RightView);

	auto DoPastaDoubleButtonRow = [&](CUIRect &View, CButtonContainer &LeftId, const char *pLeftLabel, CButtonContainer &RightId, const char *pRightLabel, bool *pLeftPressed = nullptr, bool *pRightPressed = nullptr, ColorRGBA LeftColor = ColorRGBA(1.0f, 1.0f, 1.0f, 0.25f), ColorRGBA RightColor = ColorRGBA(1.0f, 1.0f, 1.0f, 0.25f)) {
		CUIRect Row, LeftButton, RightButton;
		View.HSplitTop(gs_PastaLineSize, &Row, &View);
		Row.VSplitMid(&LeftButton, &RightButton, 5.0f);
		if(pLeftPressed != nullptr)
			*pLeftPressed = pMenus->DoButtonLineSize_MenuHelper(&LeftId, pLeftLabel, 0, &LeftButton, gs_PastaLineSize, false, nullptr, IGraphics::CORNER_ALL, 5.0f, 0.0f, LeftColor) != 0;
		else
			pMenus->DoButtonLineSize_MenuHelper(&LeftId, pLeftLabel, 0, &LeftButton, gs_PastaLineSize, false, nullptr, IGraphics::CORNER_ALL, 5.0f, 0.0f, LeftColor);
		if(pRightPressed != nullptr)
			*pRightPressed = pMenus->DoButtonLineSize_MenuHelper(&RightId, pRightLabel, 0, &RightButton, gs_PastaLineSize, false, nullptr, IGraphics::CORNER_ALL, 5.0f, 0.0f, RightColor) != 0;
		else
			pMenus->DoButtonLineSize_MenuHelper(&RightId, pRightLabel, 0, &RightButton, gs_PastaLineSize, false, nullptr, IGraphics::CORNER_ALL, 5.0f, 0.0f, RightColor);
	};

	CUIRect Column = LeftView;
	const size_t MainSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Main"));
	DoPastaSlider(pMenus, Column, &g_Config.m_PastaTasTps, "TPS", 1, 50);
	static CButtonContainer s_PauseButton;
	static CButtonContainer s_PlaybackButton;
	static CButtonContainer s_RecordButton;
	static CButtonContainer s_ClearReplayButton;
	Column.HSplitTop(gs_PastaMarginSmall, nullptr, &Column);
	bool PausePressed = false;
	bool PlaybackPressed = false;
	DoPastaDoubleButtonRow(Column, s_PauseButton, g_Config.m_PastaTasPause ? "Resume" : "Pause", s_PlaybackButton, g_Config.m_PastaLoadReplay ? "Stop Playback" : "Playback Replay", &PausePressed, &PlaybackPressed, ColorRGBA(1.0f, 1.0f, 1.0f, 0.25f), g_Config.m_PastaLoadReplay ? ColorRGBA(0.8f, 0.1f, 0.1f, 0.85f) : ColorRGBA(1.0f, 1.0f, 1.0f, 0.25f));
	if(PausePressed)
		g_Config.m_PastaTasPause ^= 1;
	if(PlaybackPressed)
		g_Config.m_PastaLoadReplay ^= 1;
	Column.HSplitTop(gs_PastaMarginSmall + 1.0f, nullptr, &Column);
	bool RecordPressed = false;
	bool ClearPressed = false;
	DoPastaDoubleButtonRow(Column, s_RecordButton, g_Config.m_PastaRecordReplay ? "Stop Recording" : "Record Replay", s_ClearReplayButton, "Clear Replay", &RecordPressed, &ClearPressed, g_Config.m_PastaRecordReplay ? ColorRGBA(0.8f, 0.1f, 0.1f, 0.85f) : ColorRGBA(1.0f, 1.0f, 1.0f, 0.25f));
	if(RecordPressed)
		g_Config.m_PastaRecordReplay ^= 1;
	if(ClearPressed)
		g_Config.m_PastaTasRespawn ^= 1;
	FinishPastaSection(Column, s_ScrollState, MainSection);

	Column.HSplitTop(gs_PastaSectionSpacing, nullptr, &Column);
	const size_t SettingsSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Settings"));
	static const SBoolEntry s_aTasChecks[] = {
		{Localize("Enable Sound"), &g_Config.m_PastaTasUseSound},
		{Localize("Enable Effects"), &g_Config.m_PastaTasShowEffects},
		{Localize("Show Real Aim"), &g_Config.m_PastaTasShowAim},
		{Localize("Predict Players"), &g_Config.m_PastaTasPlayerPrediction},
		{Localize("Auto Replay"), &g_Config.m_PastaTasAutoReplay},
		{Localize("Auto Save Replay"), &g_Config.m_PastaAutoSaveReplay},
	};
	DoPastaCheckBoxes(pMenus, Column, s_aTasChecks, std::size(s_aTasChecks));
	if(g_Config.m_PastaAutoSaveReplay)
		DoPastaSlider(pMenus, Column, &g_Config.m_PastaAutoSaveReplayInterval, Localize("Interval"), 1, 300, "s");
	FinishPastaSection(Column, s_ScrollState, SettingsSection);

	Column.HSplitTop(gs_PastaSectionSpacing, nullptr, &Column);
	const size_t HotkeysSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Hotkeys"));
	static CButtonContainer s_RecordReader;
	static CButtonContainer s_RecordClear;
	static CButtonContainer s_LoadReader;
	static CButtonContainer s_LoadClear;
	static CButtonContainer s_ClearReader;
	static CButtonContainer s_ClearClear;
	static CButtonContainer s_PauseReader;
	static CButtonContainer s_PauseClear;
	static CButtonContainer s_RewindReader;
	static CButtonContainer s_RewindClear;
	static CButtonContainer s_ForwardReader;
	static CButtonContainer s_ForwardClear;
	pMenus->DoLine_KeyReader(Column, s_RecordReader, s_RecordClear, "Record", "toggle pasta_recordreplay 1 0");
	pMenus->DoLine_KeyReader(Column, s_LoadReader, s_LoadClear, "Load", "toggle pasta_loadreplay 1 0");
	pMenus->DoLine_KeyReader(Column, s_ClearReader, s_ClearClear, "Clear", "+toggle pasta_tasrespawn 1 0");
	pMenus->DoLine_KeyReader(Column, s_PauseReader, s_PauseClear, "Pause", "toggle pasta_taspause 0 1");
	pMenus->DoLine_KeyReader(Column, s_RewindReader, s_RewindClear, "Rewind", "+toggle pasta_tasrewind 1 0");
	pMenus->DoLine_KeyReader(Column, s_ForwardReader, s_ForwardClear, "Forward", "+toggle pasta_tasforward 1 0");
	FinishPastaSection(Column, s_ScrollState, HotkeysSection);

	Column = RightView;
	const size_t ReplaySection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Replay"));
	static CLineInput s_ReplayNameInput(g_Config.m_PastaTasReplayName, sizeof(g_Config.m_PastaTasReplayName));
	static CButtonContainer s_SaveReplayInline;
	static CButtonContainer s_ReloadReplayInline;
	static CUi::SDropDownState s_SavedReplayState;
	static CScrollRegion s_SavedReplayScroll;
	static int s_SelectedReplay = 0;
	static int s_ReplayRefreshNonce = 0;
	CUIRect ReplayNameRow, ReplayNameBox, ReplaySaveButton, ReplayReloadButton;
	Column.HSplitTop(gs_PastaLineSize, &ReplayNameRow, &Column);
	ReplayNameRow.VSplitRight(70.0f, &ReplayNameRow, &ReplayReloadButton);
	ReplayNameRow.VSplitRight(5.0f, &ReplayNameRow, nullptr);
	ReplayNameRow.VSplitRight(90.0f, &ReplayNameRow, &ReplaySaveButton);
	ReplayNameBox = ReplayNameRow;
	pMenus->MenuUi()->DoClearableEditBox(&s_ReplayNameInput, &ReplayNameBox, gs_PastaFontSize, IGraphics::CORNER_ALL);
	const bool SaveInlinePressed = pMenus->DoButtonLineSize_MenuHelper(&s_SaveReplayInline, "Save", 0, &ReplaySaveButton, gs_PastaLineSize, false, nullptr, IGraphics::CORNER_ALL, 5.0f, 0.0f, ColorRGBA(0.45f, 0.45f, 0.1f, 0.85f)) != 0;
	const bool ReloadInlinePressed = pMenus->DoButtonLineSize_MenuHelper(&s_ReloadReplayInline, "Reload", 0, &ReplayReloadButton, gs_PastaLineSize, false, nullptr, IGraphics::CORNER_ALL, 5.0f, 0.0f, ColorRGBA(1.0f, 1.0f, 1.0f, 0.25f)) != 0;
	if(SaveInlinePressed)
		pMenus->PastaTasSaveReplay();
	if(ReloadInlinePressed)
	{
		++s_ReplayRefreshNonce;
		s_SavedReplayState = CUi::SDropDownState();
		s_SelectedReplay = 0;
	}
	Column.HSplitTop(gs_PastaMarginSmall, nullptr, &Column);
	std::vector<std::string> vReplayNameStorage;
	if(const char *pAppData = std::getenv("APPDATA"))
	{
		const std::filesystem::path ReplayRoot = std::filesystem::path(pAppData) / "DDNet" / "pastateam.com";
		if(std::filesystem::exists(ReplayRoot))
		{
			for(const auto &Entry : std::filesystem::recursive_directory_iterator(ReplayRoot))
			{
				if(Entry.is_regular_file() && Entry.path().extension() == ".tas")
					vReplayNameStorage.push_back(std::filesystem::relative(Entry.path(), ReplayRoot).generic_string());
			}
			std::sort(vReplayNameStorage.begin(), vReplayNameStorage.end());
		}
	}
	(void)s_ReplayRefreshNonce;
	if(vReplayNameStorage.empty())
		vReplayNameStorage.push_back("No replays found");
	std::vector<const char *> vReplayNames;
	vReplayNames.reserve(vReplayNameStorage.size());
	for(const std::string &Name : vReplayNameStorage)
		vReplayNames.push_back(Name.c_str());
	s_SelectedReplay = std::clamp(s_SelectedReplay, 0, (int)vReplayNames.size() - 1);
	s_SavedReplayState.m_SelectionPopupContext.m_pScrollRegion = &s_SavedReplayScroll;
	CUIRect ReplayDropDownRow;
	Column.HSplitTop(gs_PastaLineSize, &ReplayDropDownRow, &Column);
	const int NewSelectedReplay = pMenus->MenuUi()->DoDropDown(&ReplayDropDownRow, s_SelectedReplay, vReplayNames.empty() ? nullptr : vReplayNames.data(), vReplayNames.size(), s_SavedReplayState);
	if(NewSelectedReplay != s_SelectedReplay)
	{
		s_SelectedReplay = NewSelectedReplay;
		if(vReplayNameStorage[s_SelectedReplay] != "No replays found")
		{
			const std::string ReplayStem = std::filesystem::path(vReplayNameStorage[s_SelectedReplay]).stem().string();
			str_copy(g_Config.m_PastaTasReplayName, ReplayStem.c_str(), sizeof(g_Config.m_PastaTasReplayName));
			pMenus->PastaTasLoadSelectedReplay();
		}
	}
	Column.HSplitTop(gs_PastaMarginSmall, nullptr, &Column);
	static CButtonContainer s_ValidateReplay;
	static CButtonContainer s_ReplayTime;
	static CButtonContainer s_RemoveUseless;
	bool ValidatePressed = false;
	bool ReplayTimePressed = false;
	DoPastaDoubleButtonRow(Column, s_ValidateReplay, "Validate Replay", s_ReplayTime, "Get Replay Time", &ValidatePressed, &ReplayTimePressed);
	if(ValidatePressed)
		pMenus->PastaTasValidateReplay();
	if(ReplayTimePressed)
		pMenus->PastaTasReportReplayTime();
	Column.HSplitTop(gs_PastaMarginSmall + 1.0f, nullptr, &Column);
	if(DoPastaButton(pMenus, Column, s_RemoveUseless, "Remove Useless"))
		pMenus->PastaTasRemoveUseless();
	FinishPastaSection(Column, s_ScrollState, ReplaySection);

	Column.HSplitTop(gs_PastaSectionSpacing, nullptr, &Column);
	const size_t VaultSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Replay Vault"));
	static CButtonContainer s_AutoSyncButton;
	static CButtonContainer s_SyncNowButton;
	bool AutoSyncPressed = false;
	bool SyncNowPressed = false;
	DoPastaDoubleButtonRow(
		Column,
		s_AutoSyncButton, g_Config.m_PastaTasReplayVaultAutoSync ? "Auto Sync" : "Enable Auto Sync",
		s_SyncNowButton, "Sync Now",
		&AutoSyncPressed, &SyncNowPressed,
		ColorRGBA(0.2f, 0.65f, 0.2f, 0.85f),
		ColorRGBA(1.0f, 1.0f, 1.0f, 0.25f));
	if(AutoSyncPressed)
		g_Config.m_PastaTasReplayVaultAutoSync ^= 1;
	if(SyncNowPressed)
		pMenus->PastaTasSyncNow();
	Column.HSplitTop(gs_PastaMarginSmall, nullptr, &Column);
	FinishPastaSection(Column, s_ScrollState, VaultSection);

	Column.HSplitTop(gs_PastaSectionSpacing, nullptr, &Column);
	const size_t ToolsSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Tools"));
	DoPastaSlider(pMenus, Column, &g_Config.m_PastaTasTickControlTicks, Localize("Ticks"), 1, 128);
	static const SBoolEntry s_aToolChecks[] = {
		{Localize("Auto Rewind"), &g_Config.m_PastaTasAutoRewind},
		{Localize("Auto Forward"), &g_Config.m_PastaTasAutoForward},
		{Localize("Auto Pause"), &g_Config.m_PastaTickControlPause},
		{Localize("Step"), &g_Config.m_PastaTickControlStep},
	};
	DoPastaCheckBoxes(pMenus, Column, s_aToolChecks, std::size(s_aToolChecks));
	FinishPastaSection(Column, s_ScrollState, ToolsSection);

	Column.HSplitTop(gs_PastaSectionSpacing, nullptr, &Column);
	const size_t FakeAimSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Fake Aim"));
	static std::array<const char *, 4> s_aTasFakeAimModes = {
		Localize("Robot Aim"),
		Localize("Spinbot"),
		Localize("Random"),
		Localize("Smooth"),
	};
	static CUi::SDropDownState s_TasFakeAimState;
	static CScrollRegion s_TasFakeAimScroll;
	DoPastaDropDown(pMenus, Column, Localize("Mode"), g_Config.m_PastaTasFakeAimMode, s_aTasFakeAimModes, s_TasFakeAimState, s_TasFakeAimScroll);
	Column.HSplitTop(gs_PastaMarginSmall, nullptr, &Column);
	static CButtonContainer s_AddTasFakeAimButton;
	if(DoPastaButton(pMenus, Column, s_AddTasFakeAimButton, "Add fake aim"))
		g_Config.m_PastaTasFakeAim = 1;
	FinishPastaSection(Column, s_ScrollState, FakeAimSection);

	Column.HSplitTop(gs_PastaSectionSpacing, nullptr, &Column);
	const size_t VisualsSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Visuals"));
	static const SBoolEntry s_aTasVisualChecks[] = {
		{Localize("Draw start/end pos"), &g_Config.m_PastaTasDrawStartEndPos},
		{Localize("Draw replay path"), &g_Config.m_PastaTasDrawPath},
		{Localize("Draw prediction path"), &g_Config.m_PastaTasDrawPredictionPath},
	};
	DoPastaCheckBoxes(pMenus, Column, s_aTasVisualChecks, std::size(s_aTasVisualChecks));
	static std::array<const char *, 2> s_aTasPathModes = {Localize("Dotted"), Localize("Full Line")};
	static CUi::SDropDownState s_TasPathModeState;
	static CScrollRegion s_TasPathModeScroll;
	static CUi::SDropDownState s_TasPredictionPathModeState;
	static CScrollRegion s_TasPredictionPathModeScroll;
	static CButtonContainer s_TasPathColorReset;
	static CButtonContainer s_TasPredictionLocalReset;
	static CButtonContainer s_TasPredictionFrozenReset;
	static CButtonContainer s_TasPredictionOtherReset;
	if(g_Config.m_PastaTasDrawPath)
	{
		pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaTasDrawPathSegmented, Localize("Segmented"), &g_Config.m_PastaTasDrawPathSegmented, &Column, gs_PastaLineSize);
		DoPastaDropDown(pMenus, Column, Localize("Mode"), g_Config.m_PastaTasDrawPathMode, s_aTasPathModes, s_TasPathModeState, s_TasPathModeScroll);
		DoPastaColorPicker(pMenus, Column, s_TasPathColorReset, Localize("Color"), &g_Config.m_PastaTasDrawPathColor, ColorRGBA(0.0f, 1.0f, 0.0f));
	}
	if(g_Config.m_PastaTasDrawPredictionPath)
	{
		DoPastaDropDown(pMenus, Column, Localize("Prediction Mode"), g_Config.m_PastaTasDrawPredictionPathMode, s_aTasPathModes, s_TasPredictionPathModeState, s_TasPredictionPathModeScroll);
		DoPastaColorPicker(pMenus, Column, s_TasPredictionLocalReset, Localize("Local Color"), &g_Config.m_PastaTasDrawPredictionPathColor, ColorRGBA(0.0f, 1.0f, 0.0f));
		DoPastaColorPicker(pMenus, Column, s_TasPredictionFrozenReset, Localize("Frozen Color"), &g_Config.m_PastaTasDrawPredictionPathColorFrozen, ColorRGBA(0.0f, 1.0f, 1.0f));
		DoPastaColorPicker(pMenus, Column, s_TasPredictionOtherReset, Localize("Others Color"), &g_Config.m_PastaTasDrawPredictionPathColorOthers, ColorRGBA(0.0f, 0.5f, 0.0f));
	}
	FinishPastaSection(Column, s_ScrollState, VisualsSection);

	EndPastaScrollColumns(MainView, s_ScrollState, LeftView, RightView);
}

void RenderPastaSettings(CMenus *pMenus, CUIRect MainView)
{
	static CPastaScrollState s_ScrollState;
	CUIRect LeftView, RightView;
	BeginPastaScrollColumns(MainView, s_ScrollState, LeftView, RightView);

	CUIRect Column = LeftView;
	const size_t HotkeysSection = BeginPastaSection(pMenus, Column, s_ScrollState, Localize("Hotkeys"));
	static CButtonContainer s_AimbotReader;
	static CButtonContainer s_AimbotClear;
	static CButtonContainer s_EmoteReader;
	static CButtonContainer s_EmoteClear;
	static CButtonContainer s_HookSpamReader;
	static CButtonContainer s_HookSpamClear;
	static CButtonContainer s_AvoidReader;
	static CButtonContainer s_AvoidClear;
	static CButtonContainer s_AutoEdgeReader;
	static CButtonContainer s_AutoEdgeClear;
	static CButtonContainer s_UnfreezeReader;
	static CButtonContainer s_UnfreezeClear;
	pMenus->DoLine_KeyReader(Column, s_AimbotReader, s_AimbotClear, "Aimbot", "toggle pasta_aimbot 1 0");
	pMenus->DoLine_KeyReader(Column, s_EmoteReader, s_EmoteClear, "Emote Spam", "toggle pasta_emotespam 1 0");
	pMenus->DoLine_KeyReader(Column, s_HookSpamReader, s_HookSpamClear, "Hook Spam", "toggle pasta_hookspam 1 0");
	pMenus->DoLine_KeyReader(Column, s_AvoidReader, s_AvoidClear, "Avoid Freeze", "toggle pasta_avoidfreeze 1 0");
	pMenus->DoLine_KeyReader(Column, s_AutoEdgeReader, s_AutoEdgeClear, "Auto Edge", "+toggle pasta_autoedge 1 0");
	pMenus->DoLine_KeyReader(Column, s_UnfreezeReader, s_UnfreezeClear, "Laser Unfreeze Bot", "toggle pasta_unfreezebot 1 0");
	FinishPastaSection(Column, s_ScrollState, HotkeysSection);

	CUIRect RightColumn = RightView;
	const size_t SettingsSection = BeginPastaSection(pMenus, RightColumn, s_ScrollState, Localize("Settings"));
	pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaPredictionTeleports, Localize("Teleport Prediction"), &g_Config.m_PastaPredictionTeleports, &RightColumn, gs_PastaLineSize);
	pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaPredictionAdvancedSettings, Localize("Advanced Settings"), &g_Config.m_PastaPredictionAdvancedSettings, &RightColumn, gs_PastaLineSize);
	DoPastaSlider(pMenus, RightColumn, &g_Config.m_ClPredictionMargin, Localize("Prediction Margin"), 0, 400, "ms");
	DoPastaSlider(pMenus, RightColumn, &g_Config.m_PastaBalanceBotOffset, Localize("Balance Bot Offset"), 0, 20);
	DoPastaSlider(pMenus, RightColumn, &g_Config.m_PastaHookNearestCollisionFov, Localize("Hook Nearest Fov"), 1, 360);
	if(g_Config.m_PastaPredictionAdvancedSettings)
	{
		static const SBoolEntry s_aPredictionChecks[] = {
			{Localize("Death Tile Prediction"), &g_Config.m_PastaPredictionDeathTile},
			{Localize("Move Restrictions Prediction"), &g_Config.m_PastaPredictionMoveRestriction},
			{Localize("Player Loop (Collision)"), &g_Config.m_PastaPredictionPlayers},
			{Localize("Heart Tile Prediction"), &g_Config.m_PastaPredictionHeart},
		};
		DoPastaCheckBoxes(pMenus, RightColumn, s_aPredictionChecks, std::size(s_aPredictionChecks));
	}
	FinishPastaSection(RightColumn, s_ScrollState, SettingsSection);

	RightColumn.HSplitTop(gs_PastaSectionSpacing, nullptr, &RightColumn);
	const size_t RpcSection = BeginPastaSection(pMenus, RightColumn, s_ScrollState, Localize("Discord RPC"));
	pMenus->DoButton_CheckBoxAutoVMarginAndSet(&g_Config.m_PastaDiscordRpc, Localize("Enable"), &g_Config.m_PastaDiscordRpc, &RightColumn, gs_PastaLineSize);
	if(g_Config.m_PastaDiscordRpc)
	{
		static std::array<const char *, 4> s_aRpcModes = {"KRX", "DDNet", "TClient", Localize("Custom")};
		static CUi::SDropDownState s_RpcModeState;
		static CScrollRegion s_RpcModeScroll;
		DoPastaDropDown(pMenus, RightColumn, Localize("Mode"), g_Config.m_PastaDiscordRpcMode, s_aRpcModes, s_RpcModeState, s_RpcModeScroll);
		if(g_Config.m_PastaDiscordRpcMode == 3)
		{
			static CLineInput s_CustomRpcInput(g_Config.m_PastaDiscordRpcIdCustom, sizeof(g_Config.m_PastaDiscordRpcIdCustom));
			DoPastaEditBox(pMenus, RightColumn, &s_CustomRpcInput, "Custom RPC ID", "", g_Config.m_PastaDiscordRpcIdCustom, sizeof(g_Config.m_PastaDiscordRpcIdCustom));
		}
	}
	FinishPastaSection(RightColumn, s_ScrollState, RpcSection);

	EndPastaScrollColumns(MainView, s_ScrollState, LeftView, RightView);
}
}
