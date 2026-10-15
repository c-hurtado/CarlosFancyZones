#include "pch.h"
#include "ZonesOverlay.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include <common/logger/logger.h>
#include <common/utils/MsWindowsSettings.h>

namespace
{
    const int FadeInDurationMillis = 200;

    const int LayoutNameFadeInMillis = 150;
    const int LayoutNameFadeOutMillis = 300;
    const float LayoutNameCornerRadius = 8.f;
    const float LayoutNameMarginXRatio = 0.02f; // distance from the side edges of the work area, relative to its width
    const float LayoutNameMarginYRatio = 0.08f; // distance from the top/bottom edges of the work area, relative to its height
    const int FlashZonesDurationMillis = 700;
    const int RotationPulseDurationMillis = 920;

    D2D1_RECT_F ShrinkRect(D2D1_RECT_F rect, float scale)
    {
        const float width = rect.right - rect.left;
        const float height = rect.bottom - rect.top;
        const float dx = width * (1.f - scale) / 2.f;
        const float dy = height * (1.f - scale) / 2.f;

        return D2D1::RectF(rect.left + dx, rect.top + dy, rect.right - dx, rect.bottom - dy);
    }

    constexpr float ClampDimension(float value, float min, float max) noexcept
    {
        return std::clamp(value, min, max);
    }

    float TriangleWave(float value) noexcept
    {
        value -= std::floor(value);
        return value < 0.5f ? value * 2.f : (1.f - value) * 2.f;
    }

    constexpr float SmoothStep(float value) noexcept
    {
        value = std::clamp(value, 0.f, 1.f);
        return value * value * (3.f - 2.f * value);
    }

    // Rotates the hue of a color, to pair an accent with a neighbor for gradients
    D2D1_COLOR_F ShiftHue(D2D1_COLOR_F color, float degrees, float valueScale) noexcept
    {
        const float maxC = (std::max)({ color.r, color.g, color.b });
        const float minC = (std::min)({ color.r, color.g, color.b });
        const float delta = maxC - minC;
        float hue = 0.f;
        if (delta > 0.0001f)
        {
            if (maxC == color.r)
            {
                hue = std::fmod((color.g - color.b) / delta + 6.f, 6.f);
            }
            else if (maxC == color.g)
            {
                hue = (color.b - color.r) / delta + 2.f;
            }
            else
            {
                hue = (color.r - color.g) / delta + 4.f;
            }
            hue *= 60.f;
        }
        const float sat = maxC <= 0.f ? 0.f : delta / maxC;
        hue = std::fmod(hue + degrees + 720.f, 360.f);
        const float value = (std::min)(1.f, maxC * valueScale);
        const float chroma = value * sat;
        const float x = chroma * (1.f - std::fabs(std::fmod(hue / 60.f, 2.f) - 1.f));
        const float m = value - chroma;
        float r = 0.f, g = 0.f, b = 0.f;
        switch (static_cast<int>(hue / 60.f) % 6)
        {
        case 0: r = chroma; g = x; break;
        case 1: r = x; g = chroma; break;
        case 2: g = chroma; b = x; break;
        case 3: g = x; b = chroma; break;
        case 4: r = x; b = chroma; break;
        default: r = chroma; b = x; break;
        }
        return D2D1::ColorF(r + m, g + m, b + m, color.a);
    }

    constexpr float ZoneCornerRadius = 8.f; // Matches Windows 11 window corners

    // Gradient fill with a thin highlight on the top edge
    void DrawStyledRect(ID2D1RenderTarget* target, const D2D1_RECT_F& rect, D2D1_COLOR_F fill, D2D1_COLOR_F border, float pulse) noexcept
    {
        // pulse 0..1 breathes the fill very slightly
        fill.a *= 0.92f + 0.16f * pulse;
        const auto rounded = D2D1::RoundedRect(rect, ZoneCornerRadius, ZoneCornerRadius);
        const auto partner = ShiftHue(fill, 40.f, 0.85f);

        ID2D1GradientStopCollection* stops = nullptr;
        ID2D1LinearGradientBrush* gradient = nullptr;
        const D2D1_GRADIENT_STOP stopData[2] = {
            { 0.f, D2D1::ColorF(fill.r, fill.g, fill.b, fill.a) },
            { 1.f, D2D1::ColorF(partner.r, partner.g, partner.b, fill.a * 0.8f) }
        };
        if (SUCCEEDED(target->CreateGradientStopCollection(stopData, 2, &stops)))
        {
            target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(rect.left, rect.top), D2D1::Point2F(rect.right, rect.bottom)), stops, &gradient);
            stops->Release();
        }

        ID2D1SolidColorBrush* brush = nullptr;
        target->CreateSolidColorBrush(border, &brush);
        if (!brush)
        {
            if (gradient)
            {
                gradient->Release();
            }
            return;
        }

        if (gradient)
        {
            target->FillRoundedRectangle(rounded, gradient);
            gradient->Release();
        }

        brush->SetColor(D2D1::ColorF(1.f, 1.f, 1.f, 0.28f * border.a));
        target->DrawLine(D2D1::Point2F(rect.left + ZoneCornerRadius, rect.top + 1.5f), D2D1::Point2F(rect.right - ZoneCornerRadius, rect.top + 1.5f), brush, 1.f);

        brush->SetColor(border);
        target->DrawRoundedRectangle(rounded, brush, 2.f);
        brush->Release();
    }

    // The strip is brightest on the screen edge and fades out inward.
    void DrawEdgeGuide(ID2D1RenderTarget* target, float width, float height, D2D1_COLOR_F color, ZonesOverlay::EdgeGuide::Side side, float intensity, float depth, float alpha) noexcept
    {
        using Side = ZonesOverlay::EdgeGuide::Side;
        D2D1_RECT_F strip;
        D2D1_POINT_2F from, to;
        switch (side)
        {
        case Side::Left:
            strip = D2D1::RectF(0.f, 0.f, depth, height);
            from = D2D1::Point2F(0.f, 0.f);
            to = D2D1::Point2F(depth, 0.f);
            break;
        case Side::Top:
            strip = D2D1::RectF(0.f, 0.f, width, depth);
            from = D2D1::Point2F(0.f, 0.f);
            to = D2D1::Point2F(0.f, depth);
            break;
        case Side::Right:
            strip = D2D1::RectF(width - depth, 0.f, width, height);
            from = D2D1::Point2F(width, 0.f);
            to = D2D1::Point2F(width - depth, 0.f);
            break;
        default:
            strip = D2D1::RectF(0.f, height - depth, width, height);
            from = D2D1::Point2F(0.f, height);
            to = D2D1::Point2F(0.f, height - depth);
            break;
        }

        const float a = intensity * alpha;
        const D2D1_GRADIENT_STOP stopData[3] = {
            { 0.f, D2D1::ColorF(color.r, color.g, color.b, 0.75f * a) },
            { 0.35f, D2D1::ColorF(color.r, color.g, color.b, 0.22f * a) },
            { 1.f, D2D1::ColorF(color.r, color.g, color.b, 0.f) }
        };
        ID2D1GradientStopCollection* stops = nullptr;
        ID2D1LinearGradientBrush* gradient = nullptr;
        if (SUCCEEDED(target->CreateGradientStopCollection(stopData, 3, &stops)))
        {
            target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(from, to), stops, &gradient);
            stops->Release();
        }
        if (gradient)
        {
            target->FillRectangle(strip, gradient);
            gradient->Release();
        }
    }

    D2D1_COLOR_F MonitorAccentColor(size_t monitorNumber, float alpha = 1.f) noexcept
    {
        switch ((monitorNumber - 1) % 6)
        {
        case 0:
            return D2D1::ColorF(0.38f, 0.76f, 1.f, alpha);
        case 1:
            return D2D1::ColorF(0.45f, 0.91f, 0.58f, alpha);
        case 2:
            return D2D1::ColorF(1.f, 0.70f, 0.28f, alpha);
        case 3:
            return D2D1::ColorF(0.86f, 0.56f, 1.f, alpha);
        case 4:
            return D2D1::ColorF(0.34f, 0.93f, 0.88f, alpha);
        default:
            return D2D1::ColorF(1.f, 0.48f, 0.55f, alpha);
        }
    }

    void DrawArrowLine(ID2D1RenderTarget* renderTarget, ID2D1Brush* brush, D2D1_POINT_2F start, D2D1_POINT_2F end, float strokeWidth) noexcept
    {
        renderTarget->DrawLine(start, end, brush, strokeWidth);

        const float dx = end.x - start.x;
        const float dy = end.y - start.y;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length <= 0.001f)
        {
            return;
        }

        const float unitX = dx / length;
        const float unitY = dy / length;
        const float normalX = -unitY;
        const float normalY = unitX;
        const float headLength = strokeWidth * 4.2f;
        const float headSpread = strokeWidth * 2.7f;
        const auto headA = D2D1::Point2F(end.x - unitX * headLength + normalX * headSpread, end.y - unitY * headLength + normalY * headSpread);
        const auto headB = D2D1::Point2F(end.x - unitX * headLength - normalX * headSpread, end.y - unitY * headLength - normalY * headSpread);

        renderTarget->DrawLine(end, headA, brush, strokeWidth);
        renderTarget->DrawLine(end, headB, brush, strokeWidth);
    }

    D2D1_RECT_F CenteredRect(float centerX, float centerY, float width, float height) noexcept
    {
        return D2D1::RectF(centerX - width / 2.f, centerY - height / 2.f, centerX + width / 2.f, centerY + height / 2.f);
    }
}

namespace NonLocalizable
{
    const wchar_t SegoeUiFont[] = L"Segoe ui";
}

float ZonesOverlay::GetAnimationAlpha()
{
    // Lock is held by the caller

    if (!m_animation)
    {
        return 0.f;
    }

    auto tNow = std::chrono::steady_clock().now();
    auto millis = (tNow - m_animation->tStart).count() / 1e6f;

    if (m_animation->autoHide && millis > FlashZonesDurationMillis)
    {
        return 0.f;
    }

    // Return a positive value to avoid hiding
    return std::clamp(millis / FadeInDurationMillis, 0.001f, 1.f);
}

float ZonesOverlay::GetLayoutNameLabelAlpha()
{
    // Lock is held by the caller

    if (!m_layoutNameLabel)
    {
        return 0.f;
    }

    auto tNow = std::chrono::steady_clock().now();
    auto millis = (tNow - m_layoutNameLabel->tStart).count() / 1e6f;

    const float duration = static_cast<float>(m_layoutNameLabel->durationMillis);
    const float fadeIn = std::min<float>(LayoutNameFadeInMillis, duration / 3.f);
    const float fadeOut = std::min<float>(LayoutNameFadeOutMillis, duration / 3.f);

    if (millis >= duration)
    {
        return 0.f;
    }

    if (millis < fadeIn)
    {
        return millis / fadeIn;
    }

    const auto remainingMillis = duration - millis;
    if (remainingMillis < fadeOut)
    {
        return remainingMillis / fadeOut;
    }

    return 1.f;
}

IDWriteFactory* ZonesOverlay::GetWriteFactory()
{
    static auto pDWriteFactory = [] {
        IUnknown* res = nullptr;
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), &res);
        return reinterpret_cast<IDWriteFactory*>(res);
    }();
    return pDWriteFactory;
}

D2D1_COLOR_F ZonesOverlay::ConvertColor(COLORREF color)
{
    return D2D1::ColorF(GetRValue(color) / 255.f,
                        GetGValue(color) / 255.f,
                        GetBValue(color) / 255.f,
                        1.f);
}

D2D1_RECT_F ZonesOverlay::ConvertRect(RECT rect)
{
    return D2D1::RectF(rect.left + 0.5f, rect.top + 0.5f, rect.right - 0.5f, rect.bottom - 0.5f);
}

D2D1_RECT_F ZonesOverlay::OffsetRect(D2D1_RECT_F rect, float x, float y)
{
    return D2D1::RectF(rect.left + x, rect.top + y, rect.right + x, rect.bottom + y);
}

ZonesOverlay::ZonesOverlay(HWND window)
{
    HRESULT hr;
    m_window = window;
    m_renderTarget = nullptr;
    m_shouldRender = false;

    // Obtain the size of the drawing area.
    if (!GetClientRect(window, &m_clientRect))
    {
        Logger::error(L"couldn't initialize ZonesOverlay: GetClientRect failed");
        return;
    }

    // Create a Direct2D render target
    // We should always use the DPI value of 96 since we're running in DPI aware mode
    auto renderTargetProperties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.f,
        96.f);

    auto renderTargetSize = D2D1::SizeU(m_clientRect.right - m_clientRect.left, m_clientRect.bottom - m_clientRect.top);
    auto hwndRenderTargetProperties = D2D1::HwndRenderTargetProperties(window, renderTargetSize);

    ID2D1Factory* factory = nullptr;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, &factory);
    hr = factory->CreateHwndRenderTarget(renderTargetProperties, hwndRenderTargetProperties, &m_renderTarget);
    factory->Release();
    factory = nullptr;

    if (!SUCCEEDED(hr))
    {
        Logger::error(L"couldn't initialize ZonesOverlay: CreateHwndRenderTarget failed with {}", hr);
        return;
    }

    m_renderThread = std::thread([this]() { RenderLoop(); });
}

ZonesOverlay::RenderResult ZonesOverlay::Render()
{
    std::unique_lock lock(m_mutex);

    if (!m_renderTarget)
    {
        return RenderResult::Failed;
    }

    float animationAlpha = GetAnimationAlpha();

    if (animationAlpha <= 0.f)
    {
        return RenderResult::AnimationEnded;
    }

    BOOL isEnabledAnimations = GetAnimationsEnabled();
    if (!isEnabledAnimations)
    {
        animationAlpha = 1.f;
    }
    const bool animateRotation = m_animateRotation && isEnabledAnimations;

    m_renderTarget->BeginDraw();

    const auto renderNow = std::chrono::steady_clock().now();
    float rotationPulse = 0.f;
    float rotationProgress = 0.f;
    if (animateRotation && m_rotationPulseStart)
    {
        const auto pulseMillis = (renderNow - *m_rotationPulseStart).count() / 1e6f;
        if (pulseMillis < RotationPulseDurationMillis)
        {
            rotationProgress = std::clamp(pulseMillis / RotationPulseDurationMillis, 0.f, 1.f);
            rotationPulse = 1.f - rotationProgress;
        }
        else
        {
            rotationProgress = 1.f;
            m_rotationPulseStart.reset();
        }
    }

    const auto width = static_cast<float>(m_clientRect.right - m_clientRect.left);
    const auto height = static_cast<float>(m_clientRect.bottom - m_clientRect.top);
    const float directionSign = m_rotationDirection == RotationDirection::Left ? -1.f : (m_rotationDirection == RotationDirection::Right ? 1.f : 0.f);
    const float easedRotationProgress = SmoothStep(rotationProgress);
    const float pulseOffset = animateRotation ? directionSign * easedRotationProgress * ClampDimension(width * 0.075f, 44.f, 132.f) : 0.f;
    const float pulseScale = 1.f + ((animateRotation ? rotationPulse : 0.f) * 0.05f);

    // Draw backdrop
    m_renderTarget->Clear(D2D1::ColorF(0.f, 0.f, 0.f, 0.f));
    if (m_drawBackdrop)
    {
        ID2D1SolidColorBrush* backdropBrush = nullptr;
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.f, 0.f, 0.f, 0.88f * animationAlpha), &backdropBrush);
        if (backdropBrush)
        {
            m_renderTarget->FillRectangle(ConvertRect(m_clientRect), backdropBrush);
            backdropBrush->Release();
        }
    }

    if (m_edgeGuide)
    {
        DrawEdgeGuide(m_renderTarget, width, height, ConvertColor(m_edgeGuide->color), m_edgeGuide->side, m_edgeGuide->intensity, m_edgeGuide->depth, animationAlpha);
    }

    IDWriteTextFormat* textFormat = nullptr;

    auto writeFactory = GetWriteFactory();

    if (writeFactory)
    {
        writeFactory->CreateTextFormat(NonLocalizable::SegoeUiFont, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 80.f, L"en-US", &textFormat);
    }

    // Slow breathing of styled zones, 2.4s per cycle. Held at mid-level when Windows animations are off.
    const float breathe = isEnabledAnimations ? 0.5f + 0.5f * std::sin(static_cast<float>(std::chrono::duration_cast<std::chrono::milliseconds>(renderNow.time_since_epoch()).count() % 2400) / 2400.f * 6.2831853f) : 0.5f;

    for (auto drawableRect : m_sceneRects)
    {
        ID2D1SolidColorBrush* borderBrush = nullptr;
        ID2D1SolidColorBrush* fillBrush = nullptr;

        // Need to copy the rect from m_sceneRects
        drawableRect.borderColor.a *= animationAlpha;
        drawableRect.fillColor.a *= animationAlpha;
        drawableRect.rect = OffsetRect(drawableRect.rect, pulseOffset, 0.f);

        if (drawableRect.styled)
        {
            DrawStyledRect(m_renderTarget, drawableRect.rect, drawableRect.fillColor, drawableRect.borderColor, breathe);
        }
        else
        {
            m_renderTarget->CreateSolidColorBrush(drawableRect.borderColor, &borderBrush);
            m_renderTarget->CreateSolidColorBrush(drawableRect.fillColor, &fillBrush);
        }

        if (fillBrush)
        {
            m_renderTarget->FillRectangle(drawableRect.rect, fillBrush);
            fillBrush->Release();
        }

        if (borderBrush)
        {
            m_renderTarget->DrawRectangle(drawableRect.rect, borderBrush);
            borderBrush->Release();
        }

        std::wstring idStr = std::to_wstring(drawableRect.id + 1);

        if (drawableRect.showText)
        {
            ID2D1SolidColorBrush* textBrush = nullptr;
            m_renderTarget->CreateSolidColorBrush(drawableRect.textColor, &textBrush);

            if (textFormat && textBrush)
            {
                textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                m_renderTarget->DrawTextW(idStr.c_str(), static_cast<UINT32>(idStr.size()), textFormat, drawableRect.rect, textBrush);
            }

            if (textBrush)
            {
                textBrush->Release();
            }
        }
    }

    if (textFormat)
    {
        textFormat->Release();
    }

    const auto accentColor = MonitorAccentColor(m_monitorNumber.value_or(1));

    if (animateRotation && (m_rotationDirection == RotationDirection::Left || m_rotationDirection == RotationDirection::Right))
    {
        constexpr int MarkerCount = 7;
        const float railWidth = ClampDimension(width * 0.58f, 260.f, 760.f);
        const float railLeft = (width - railWidth) / 2.f;
        const float railY = (height / 2.f) + ClampDimension(height * 0.20f, 104.f, 222.f);
        const float segmentWidth = railWidth / MarkerCount;
        const float markerSize = ClampDimension(width * 0.025f, 18.f, 34.f);
        const float strokeWidth = ClampDimension(width * 0.0038f, 3.4f, 6.4f);

        for (int markerIndex = 0; markerIndex < MarkerCount; markerIndex++)
        {
            const float order = m_rotationDirection == RotationDirection::Left ? static_cast<float>(MarkerCount - 1 - markerIndex) : static_cast<float>(markerIndex);
            const float wave = TriangleWave(rotationProgress * 2.4f - order * 0.14f);
            const float alpha = (0.18f + wave * 0.64f) * animationAlpha;
            ID2D1SolidColorBrush* markerBrush = nullptr;
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(accentColor.r, accentColor.g, accentColor.b, alpha), &markerBrush);

            if (markerBrush)
            {
                const float centerX = railLeft + segmentWidth * (markerIndex + 0.5f) + directionSign * wave * 9.f;
                const float headX = centerX + directionSign * markerSize;
                const auto head = D2D1::Point2F(headX, railY);
                const auto wingTop = D2D1::Point2F(centerX - directionSign * markerSize * 0.45f, railY - markerSize * 0.62f);
                const auto wingBottom = D2D1::Point2F(centerX - directionSign * markerSize * 0.45f, railY + markerSize * 0.62f);
                m_renderTarget->DrawLine(wingTop, head, markerBrush, strokeWidth);
                m_renderTarget->DrawLine(wingBottom, head, markerBrush, strokeWidth);
                markerBrush->Release();
            }
        }
    }

    if (m_monitorNumber && writeFactory)
    {
        const float smallerDimension = width < height ? width : height;
        const bool isRotating = animateRotation && (m_rotationDirection == RotationDirection::Left || m_rotationDirection == RotationDirection::Right);
        const float radius = ClampDimension(smallerDimension * 0.14f, 82.f, 170.f) * pulseScale;
        const float centerX = width / 2.f;
        const float centerY = height / 2.f;

        if (isRotating)
        {
            ID2D1SolidColorBrush* ghostFillBrush = nullptr;
            ID2D1SolidColorBrush* ghostBorderBrush = nullptr;
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(accentColor.r, accentColor.g, accentColor.b, 0.10f * animationAlpha), &ghostFillBrush);
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(accentColor.r, accentColor.g, accentColor.b, 0.42f * animationAlpha), &ghostBorderBrush);

            if (ghostFillBrush && ghostBorderBrush)
            {
                const float ghostWidth = ClampDimension(width * 0.34f, 220.f, 520.f);
                const float ghostHeight = ClampDimension(height * 0.17f, 110.f, 220.f);
                const float ghostCenterX = (width / 2.f) + pulseOffset;
                const float ghostCenterY = centerY - ClampDimension(height * 0.17f, 72.f, 170.f);
                const auto ghostRect = CenteredRect(ghostCenterX, ghostCenterY, ghostWidth, ghostHeight);
                const float cornerRadius = ClampDimension(ghostHeight * 0.16f, 14.f, 28.f);
                const auto roundedGhostRect = D2D1::RoundedRect(ghostRect, cornerRadius, cornerRadius);
                m_renderTarget->FillRoundedRectangle(roundedGhostRect, ghostFillBrush);
                m_renderTarget->DrawRoundedRectangle(roundedGhostRect, ghostBorderBrush, ClampDimension(width * 0.002f, 2.f, 4.f));

                const int innerLines = 3;
                const float lineGap = ghostHeight * 0.20f;
                const float lineStart = ghostCenterY - lineGap;
                for (int lineIndex = 0; lineIndex < innerLines; lineIndex++)
                {
                    const float lineY = lineStart + lineGap * lineIndex;
                    const float lineInset = ghostWidth * (0.18f + lineIndex * 0.04f);
                    m_renderTarget->DrawLine(
                        D2D1::Point2F(ghostRect.left + lineInset, lineY),
                        D2D1::Point2F(ghostRect.right - lineInset, lineY),
                        ghostBorderBrush,
                        ClampDimension(width * 0.0014f, 1.5f, 3.f));
                }
            }

            if (ghostFillBrush)
            {
                ghostFillBrush->Release();
            }

            if (ghostBorderBrush)
            {
                ghostBorderBrush->Release();
            }
        }

        ID2D1SolidColorBrush* glowBrush = nullptr;
        ID2D1SolidColorBrush* numberBrush = nullptr;
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(accentColor.r, accentColor.g, accentColor.b, (0.11f + rotationPulse * 0.11f) * animationAlpha), &glowBrush);
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.f, 1.f, 1.f, 0.95f * animationAlpha), &numberBrush);

        if (glowBrush)
        {
            m_renderTarget->FillEllipse(D2D1::Ellipse(D2D1::Point2F(centerX, centerY), radius + 22.f, radius + 22.f), glowBrush);
            glowBrush->Release();
        }

        IDWriteTextFormat* numberFormat = nullptr;
        writeFactory->CreateTextFormat(NonLocalizable::SegoeUiFont, nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, isRotating ? radius * 0.72f : radius * 1.15f, L"en-US", &numberFormat);
        if (numberFormat && numberBrush)
        {
            const auto numberStr = std::to_wstring(*m_monitorNumber);
            numberFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            numberFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            const auto numberRect = isRotating ?
                D2D1::RectF(centerX - (radius * 0.45f), centerY - (radius * 0.45f), centerX + (radius * 0.45f), centerY + (radius * 0.45f)) :
                D2D1::RectF(centerX - radius, centerY - radius, centerX + radius, centerY + radius);
            m_renderTarget->DrawTextW(numberStr.c_str(), static_cast<UINT32>(numberStr.size()), numberFormat, numberRect, numberBrush);
        }

        if (isRotating)
        {
            ID2D1SolidColorBrush* symbolBrush = nullptr;
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(accentColor.r, accentColor.g, accentColor.b, (0.84f + rotationPulse * 0.14f) * animationAlpha), &symbolBrush);

            if (symbolBrush)
            {
                const float strokeWidth = ClampDimension(radius * 0.045f, 4.5f, 8.5f);
                const float arm = radius * 0.76f;
                const float gap = radius * 0.30f;
                if (m_rotationDirection == RotationDirection::Left)
                {
                    DrawArrowLine(m_renderTarget, symbolBrush, D2D1::Point2F(centerX + gap, centerY - gap), D2D1::Point2F(centerX - arm, centerY - arm), strokeWidth);
                    DrawArrowLine(m_renderTarget, symbolBrush, D2D1::Point2F(centerX - gap, centerY + gap), D2D1::Point2F(centerX + arm, centerY + arm), strokeWidth);
                }
                else
                {
                    DrawArrowLine(m_renderTarget, symbolBrush, D2D1::Point2F(centerX - gap, centerY - gap), D2D1::Point2F(centerX + arm, centerY - arm), strokeWidth);
                    DrawArrowLine(m_renderTarget, symbolBrush, D2D1::Point2F(centerX + gap, centerY + gap), D2D1::Point2F(centerX - arm, centerY + arm), strokeWidth);
                }

                symbolBrush->Release();
            }
        }

        if (numberBrush)
        {
            numberBrush->Release();
        }

        if (numberFormat)
        {
            numberFormat->Release();
        }
    }

    if (m_layoutNameLabel.has_value())
    {
        float labelAlpha = GetLayoutNameLabelAlpha();
        if (labelAlpha <= 0.f)
        {
            m_layoutNameLabel.reset();
        }
        else if (writeFactory)
        {
            if (!isEnabledAnimations)
            {
                // No fading, but the label still disappears once its lifetime is over
                labelAlpha = 1.f;
            }

            // The label is part of the overlay, don't let it outshine a fading scene
            labelAlpha *= animationAlpha;

            IDWriteTextFormat* nameFormat = nullptr;
            writeFactory->CreateTextFormat(NonLocalizable::SegoeUiFont, nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, m_layoutNameLabel->fontSize, L"en-US", &nameFormat);

            IDWriteTextLayout* nameLayout = nullptr;
            const float clientWidth = static_cast<float>(m_clientRect.right - m_clientRect.left);
            const float clientHeight = static_cast<float>(m_clientRect.bottom - m_clientRect.top);
            const float marginX = clientWidth * LayoutNameMarginXRatio;
            const float marginY = clientHeight * LayoutNameMarginYRatio;
            // Keep the chip inside the work area: a long name is trimmed with an ellipsis instead of spilling off-screen
            const float maxTextWidth = std::max<float>(1.f, clientWidth - 2 * marginX - 2 * m_layoutNameLabel->paddingX);
            if (nameFormat)
            {
                nameFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

                IDWriteInlineObject* trimmingSign = nullptr;
                if (SUCCEEDED(writeFactory->CreateEllipsisTrimmingSign(nameFormat, &trimmingSign)))
                {
                    const DWRITE_TRIMMING trimming{ .granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER };
                    nameFormat->SetTrimming(&trimming, trimmingSign);
                    trimmingSign->Release();
                }

                writeFactory->CreateTextLayout(m_layoutNameLabel->text.c_str(), static_cast<UINT32>(m_layoutNameLabel->text.size()), nameFormat, maxTextWidth, clientHeight, &nameLayout);
            }

            if (nameLayout)
            {
                DWRITE_TEXT_METRICS metrics{};
                nameLayout->GetMetrics(&metrics);

                const float chipWidth = std::min<float>(metrics.width, maxTextWidth) + 2 * m_layoutNameLabel->paddingX;
                const float chipHeight = metrics.height + 2 * m_layoutNameLabel->paddingY;

                float left = (clientWidth - chipWidth) / 2.f;
                float top = marginY;
                switch (m_layoutNameLabel->placement)
                {
                case LayoutNameLabelPlacement::TopLeft:
                    left = marginX;
                    break;
                case LayoutNameLabelPlacement::TopRight:
                    left = clientWidth - marginX - chipWidth;
                    break;
                case LayoutNameLabelPlacement::Center:
                    top = (clientHeight - chipHeight) / 2.f;
                    break;
                case LayoutNameLabelPlacement::BottomLeft:
                    left = marginX;
                    top = clientHeight - marginY - chipHeight;
                    break;
                case LayoutNameLabelPlacement::BottomCenter:
                    top = clientHeight - marginY - chipHeight;
                    break;
                case LayoutNameLabelPlacement::BottomRight:
                    left = clientWidth - marginX - chipWidth;
                    top = clientHeight - marginY - chipHeight;
                    break;
                case LayoutNameLabelPlacement::TopCenter:
                default:
                    break;
                }

                D2D1_ROUNDED_RECT chip{
                    .rect = D2D1::RectF(left, top, left + chipWidth, top + chipHeight),
                    .radiusX = LayoutNameCornerRadius,
                    .radiusY = LayoutNameCornerRadius,
                };

                auto backgroundColor = m_layoutNameLabel->backgroundColor;
                auto labelTextColor = m_layoutNameLabel->textColor;
                backgroundColor.a *= labelAlpha;
                labelTextColor.a *= labelAlpha;

                ID2D1SolidColorBrush* backgroundBrush = nullptr;
                ID2D1SolidColorBrush* nameBrush = nullptr;
                m_renderTarget->CreateSolidColorBrush(backgroundColor, &backgroundBrush);
                m_renderTarget->CreateSolidColorBrush(labelTextColor, &nameBrush);

                if (backgroundBrush)
                {
                    m_renderTarget->FillRoundedRectangle(chip, backgroundBrush);
                    backgroundBrush->Release();
                }

                if (nameBrush)
                {
                    m_renderTarget->DrawTextLayout(D2D1::Point2F(left + m_layoutNameLabel->paddingX, top + m_layoutNameLabel->paddingY), nameLayout, nameBrush);
                    nameBrush->Release();
                }

                nameLayout->Release();
            }

            if (nameFormat)
            {
                nameFormat->Release();
            }
        }
    }

    // The lock must be released here, as EndDraw() will wait for vertical sync
    lock.unlock();

    m_renderTarget->EndDraw();
    return RenderResult::Ok;
}

void ZonesOverlay::RenderLoop()
{
    while (!m_abortThread)
    {
        {
            // Wait here while rendering is disabled
            std::unique_lock lock(m_mutex);
            m_cv.wait(lock, [this]() { return (bool)m_shouldRender; });
        }

        auto result = Render();

        if (result == RenderResult::AnimationEnded || result == RenderResult::Failed)
        {
            Hide();
        }
    }
}

void ZonesOverlay::Hide()
{
    bool shouldHideWindow = true;
    {
        std::unique_lock lock(m_mutex);
        m_animation.reset();
        m_layoutNameLabel.reset();
        m_edgeGuide.reset();
        shouldHideWindow = m_shouldRender;
        m_shouldRender = false;
    }

    if (shouldHideWindow)
    {
        ShowWindow(m_window, SW_HIDE);
    }
}

void ZonesOverlay::Show()
{
    bool shouldShowWindow = true;
    {
        std::unique_lock lock(m_mutex);
        shouldShowWindow = !m_shouldRender;
        m_shouldRender = true;

        if (!m_animation)
        {
            m_animation.emplace(AnimationInfo{ .tStart = std::chrono::steady_clock().now(), .autoHide = false });
        }
        else if (m_animation->autoHide)
        {
            // Do not change the starting time of the animation, just reset autoHide
            m_animation->autoHide = false;
        }
    }

    if (shouldShowWindow)
    {
        ShowWindow(m_window, SW_SHOWNA);
    }

    m_cv.notify_all();
}

void ZonesOverlay::SetEdgeGuide(std::optional<EdgeGuide> guide)
{
    std::unique_lock lock(m_mutex);
    m_edgeGuide = guide;
}

void ZonesOverlay::Flash()
{
    bool shouldShowWindow = true;
    {
        std::unique_lock lock(m_mutex);
        shouldShowWindow = !m_shouldRender;
        m_shouldRender = true;

        m_animation.emplace(AnimationInfo{ .tStart = std::chrono::steady_clock().now(), .autoHide = true });
    }

    if (shouldShowWindow)
    {
        ShowWindow(m_window, SW_SHOWNA);
    }

    m_cv.notify_all();
}

void ZonesOverlay::DrawActiveZoneSet(const ZonesMap& zones,
                                     const ZoneIndexSet& highlightZones,
                                     const Colors::ZoneColors& colors,
                                     const bool showZoneText,
                                     const bool styled)
{
    std::unique_lock lock(m_mutex);

    m_sceneRects = {};
    m_drawBackdrop = false;
    m_rotationDirection = RotationDirection::None;
    m_animateRotation = false;
    m_monitorNumber.reset();
    m_rotationPulseStart.reset();

    auto borderColor = ConvertColor(colors.borderColor);
    auto inactiveColor = ConvertColor(colors.primaryColor);
    auto highlightColor = ConvertColor(colors.highlightColor);
    auto numberColor = ConvertColor(colors.numberColor);

    inactiveColor.a = colors.highlightOpacity / 100.f;
    highlightColor.a = colors.highlightOpacity / 100.f;

    std::vector<bool> isHighlighted(zones.size() + 1, false);
    for (ZoneIndex x : highlightZones)
    {
        isHighlighted[x] = true;
    }

    // First draw the inactive zones
    for (const auto& [zoneId, zone] : zones)
    {
        if (!isHighlighted[zoneId])
        {
            DrawableRect drawableRect{
                .rect = ConvertRect(zone.GetZoneRect()),
                .borderColor = borderColor,
                .fillColor = inactiveColor,
                .textColor = numberColor,
                .id = zone.Id(),
                .showText = showZoneText,
                .styled = styled
            };

            m_sceneRects.push_back(drawableRect);
        }
    }

    // Draw the active zones on top of the inactive zones
    for (const auto& [zoneId, zone] : zones)
    {
        if (isHighlighted[zoneId])
        {
            DrawableRect drawableRect{
                .rect = ConvertRect(zone.GetZoneRect()),
                .borderColor = borderColor,
                .fillColor = highlightColor,
                .textColor = numberColor,
                .id = zone.Id(),
                .showText = showZoneText,
                .styled = styled
            };

            m_sceneRects.push_back(drawableRect);
        }
    }
}

void ZonesOverlay::DrawMonitorRotationPreview(const std::vector<RECT>& windowRects, size_t monitorNumber, std::optional<bool> reverse, bool animateRotation)
{
    std::unique_lock lock(m_mutex);

    m_sceneRects = {};
    m_drawBackdrop = true;
    m_edgeGuide.reset();
    m_rotationDirection = reverse.has_value() ? (reverse.value() ? RotationDirection::Left : RotationDirection::Right) : RotationDirection::Both;
    m_animateRotation = animateRotation && reverse.has_value();
    m_monitorNumber = monitorNumber;
    if (m_animateRotation)
    {
        m_rotationPulseStart = std::chrono::steady_clock().now();
    }
    else
    {
        m_rotationPulseStart.reset();
    }

    const auto borderColor = D2D1::ColorF(1.f, 1.f, 1.f, 0.08f);
    const auto fillColor = D2D1::ColorF(0.05f, 0.07f, 0.09f, 0.50f);
    const auto textColor = D2D1::ColorF(1.f, 1.f, 1.f, 0.85f);

    for (const auto& rect : windowRects)
    {
        const auto drawableRect = ShrinkRect(ConvertRect(rect), 0.82f);
        if (drawableRect.right - drawableRect.left < 18.f || drawableRect.bottom - drawableRect.top < 18.f)
        {
            continue;
        }

        m_sceneRects.push_back(DrawableRect{
            .rect = drawableRect,
            .borderColor = borderColor,
            .fillColor = fillColor,
            .textColor = textColor,
            .id = 0,
            .showText = false,
        });
    }
}

void ZonesOverlay::ShowLayoutName(const std::wstring& text, const LayoutNameLabelOptions& options)
{
    if (text.empty())
    {
        return;
    }

    std::unique_lock lock(m_mutex);

    auto backgroundColor = ConvertColor(options.backgroundColor);
    backgroundColor.a = 0.9f;

    m_layoutNameLabel.emplace(LayoutNameLabel{
        .text = text,
        .textColor = ConvertColor(options.textColor),
        .backgroundColor = backgroundColor,
        .fontSize = static_cast<float>(std::max<int>(options.fontSize, 8)),
        .paddingX = static_cast<float>(2 * std::max<int>(options.padding, 0)),
        .paddingY = static_cast<float>(std::max<int>(options.padding, 0)),
        .durationMillis = std::max<int>(options.durationMillis, 300),
        .placement = options.placement,
        .tStart = std::chrono::steady_clock().now(),
    });
}

ZonesOverlay::~ZonesOverlay()
{
    // Constructor early-returns (e.g. CreateHwndRenderTarget failing during a
    // display-driver TDR) leave m_renderThread default-constructed; calling
    // join() on a non-joinable thread terminates the process.
    if (m_renderThread.joinable())
    {
        {
            std::unique_lock lock(m_mutex);
            m_abortThread = true;
            m_shouldRender = true;
        }
        m_cv.notify_all();
        m_renderThread.join();
    }

    if (m_renderTarget)
    {
        m_renderTarget->Release();
        m_renderTarget = nullptr;
    }
}
