#pragma once

#include <map>
#include <optional>
#include <vector>
#include <wil\resource.h>
#include <winrt/base.h>
#include <d2d1.h>
#include <dwrite.h>

#include "util.h"
#include "Zone.h"
#include "FancyZones.h"
#include "Colors.h"
#include "LayoutConfigurator.h"
#include "Settings.h"

class ZonesOverlay
{
public:
    // Glow along a work area edge that the cursor is heading for
    struct EdgeGuide
    {
        enum class Side
        {
            Left,
            Top,
            Right,
            Bottom
        } side;
        float intensity; // 0..1, 1 when the cursor is inside the dock band
        float depth; // Pixels the glow reaches into the work area
        COLORREF color;
    };

private:
    struct DrawableRect
    {
        D2D1_RECT_F rect;
        D2D1_COLOR_F borderColor;
        D2D1_COLOR_F fillColor;
        D2D1_COLOR_F textColor;
        ZoneIndex id;
        bool showText;
        bool styled = false; // Rounded, gradient and glowing instead of a flat rectangle
    };

    struct AnimationInfo
    {
        std::chrono::steady_clock::time_point tStart;
        bool autoHide;
    };

    struct LayoutNameLabel
    {
        std::wstring text;
        D2D1_COLOR_F textColor;
        D2D1_COLOR_F backgroundColor;
        float fontSize;
        float paddingX;
        float paddingY;
        int durationMillis;
        LayoutNameLabelPlacement placement;
        std::chrono::steady_clock::time_point tStart;
    };

    enum class RotationDirection
    {
        None,
        Left,
        Right,
        Both,
    };

    enum struct RenderResult
    {
        Ok,
        AnimationEnded,
        Failed,
    };

    HWND m_window = nullptr;
    RECT m_clientRect{};
    ID2D1HwndRenderTarget* m_renderTarget = nullptr;
    std::optional<AnimationInfo> m_animation;
    std::optional<LayoutNameLabel> m_layoutNameLabel;

    std::mutex m_mutex;
    std::vector<DrawableRect> m_sceneRects;
    bool m_drawBackdrop = false;
    RotationDirection m_rotationDirection = RotationDirection::None;
    bool m_animateRotation = false;
    std::optional<size_t> m_monitorNumber;
    std::optional<std::chrono::steady_clock::time_point> m_rotationPulseStart;
    std::optional<EdgeGuide> m_edgeGuide;

    float GetAnimationAlpha();
    float GetLayoutNameLabelAlpha();
    static IDWriteFactory* GetWriteFactory();
    static D2D1_COLOR_F ConvertColor(COLORREF color);
    static D2D1_RECT_F ConvertRect(RECT rect);
    static D2D1_RECT_F OffsetRect(D2D1_RECT_F rect, float x, float y);
    RenderResult Render();
    void RenderLoop();

    std::atomic<bool> m_shouldRender = false;
    std::atomic<bool> m_abortThread = false;
    std::condition_variable m_cv;
    std::thread m_renderThread;

public:
    struct LayoutNameLabelOptions
    {
        COLORREF textColor;
        COLORREF backgroundColor;
        int fontSize;
        int padding;
        int durationMillis;
        LayoutNameLabelPlacement placement;
    };

    ~ZonesOverlay();
    ZonesOverlay(HWND window);
    void Hide();
    void Show();
    void Flash();
    void SetEdgeGuide(std::optional<EdgeGuide> guide);
    void DrawActiveZoneSet(const ZonesMap& zones,
                           const ZoneIndexSet& highlightZones,
                           const Colors::ZoneColors& colors,
                           const bool showZoneText,
                           const bool styled = false);
    void ShowLayoutName(const std::wstring& text, const LayoutNameLabelOptions& options);
    void DrawMonitorRotationPreview(const std::vector<RECT>& windowRects, size_t monitorNumber, std::optional<bool> reverse, bool animateRotation);
};
