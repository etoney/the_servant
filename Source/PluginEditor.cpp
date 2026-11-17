#include "PluginEditor.h"

#ifndef JucePlugin_VersionString
 #define JucePlugin_VersionString "dev"
#endif

//==============================================================================
namespace pal
{
const juce::Colour ironDark  { 0xff1d1815 };
const juce::Colour iron      { 0xff3a322c };
const juce::Colour rustDeep  { 0xff4a1d0c };
const juce::Colour rust      { 0xff9a4518 };
const juce::Colour rustLight { 0xffc0702a };
const juce::Colour paint     { 0xffd6c7a6 };   // dirty cream stencil paint
const juce::Colour paintDim  { 0xffa8997c };
const juce::Colour signal    { 0xffd2452a };   // rusty red for active things
const juce::Colour signalHot { 0xffff6a3d };
const juce::Colour hazard    { 0xffc99a22 };   // worn safety yellow
const juce::Colour screen    { 0xff0c0a08 };
const juce::Colour leather   { 0xff4a1a16 };   // worn oxblood leather
const juce::Colour leatherDark { 0xff1c0a08 };
}

//==============================================================================
namespace tex
{
inline float hash (int x, int y, int seed)
{
    uint32_t h = (uint32_t) x * 374761393u + (uint32_t) y * 668265263u + (uint32_t) seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (float) (h & 0xffffffu) / 16777215.0f;
}
inline float smooth (float t) { return t * t * (3.0f - 2.0f * t); }
inline float smoothstep (float a, float b, float x) { return smooth (juce::jlimit (0.0f, 1.0f, (x - a) / (b - a))); }
inline float vnoise (float x, float y, int seed)
{
    const int xi = (int) std::floor (x), yi = (int) std::floor (y);
    const float fx = x - (float) xi, fy = y - (float) yi;
    const float a = hash (xi, yi, seed), b = hash (xi + 1, yi, seed), c = hash (xi, yi + 1, seed), d = hash (xi + 1, yi + 1, seed);
    const float u = smooth (fx), v = smooth (fy);
    return a + (b - a) * u + (c - a) * v + (a - b - c + d) * u * v;
}
inline float fbm (float x, float y, int seed, int octaves)
{
    float s = 0, amp = 0.5f, f = 1, norm = 0;
    for (int i = 0; i < octaves; ++i) { s += amp * vnoise (x * f, y * f, seed + i * 31); norm += amp; f *= 2.03f; amp *= 0.5f; }
    return s / norm;
}
struct RGB { float r, g, b; };
inline RGB rgb (juce::Colour c) { return { c.getFloatRed(), c.getFloatGreen(), c.getFloatBlue() }; }
inline RGB mix (RGB a, RGB b, float t) { return { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t }; }
inline RGB mul (RGB a, float k) { return { a.r * k, a.g * k, a.b * k }; }

inline RGB rustAt (float x, float y, float rustBias, int seed)
{
    const float n      = fbm (x / 170.0f, y / 170.0f, seed, 4);
    const float detail = fbm (x / 36.0f, y / 36.0f, seed + 7, 3);
    const float fine   = vnoise (x / 7.0f, y / 7.0f, seed + 13);
    const float mask   = smoothstep (0.45f, 0.62f, n + (detail - 0.5f) * 0.35f + rustBias);
    RGB base = mix (rgb (pal::ironDark), rgb (pal::iron), detail);
    RGB r    = mix (rgb (pal::rustDeep), rgb (pal::rust), fbm (x / 20.0f, y / 20.0f, seed + 3, 2));
    if (fine > 0.72f) r = mix (r, rgb (pal::rustLight), (fine - 0.72f) * 2.5f);
    return mix (base, r, mask);
}

// Paint-chip a rendered image: knock out chunks of alpha with noise, plus grain.
inline void distress (juce::Image& img, float scale, float wear, int seed)
{
    juce::Image::BitmapData bd (img, juce::Image::BitmapData::readWrite);
    for (int y = 0; y < bd.height; ++y)
        for (int x = 0; x < bd.width; ++x)
        {
            auto c = bd.getPixelColour (x, y);
            if (c.getAlpha() == 0) continue;
            const float n = fbm ((float) x / (7.0f * scale), (float) y / (7.0f * scale), seed, 3);
            const float chip = n < wear ? 0.08f : 1.0f;
            const float grain = 0.7f + 0.3f * hash (x, y, seed + 99);
            bd.setPixelColour (x, y, c.withMultipliedAlpha (chip * grain));
        }
}

inline juce::Font stencil (float size) { return juce::Font (juce::FontOptions ("Impact", size, juce::Font::plain)); }
inline juce::Font plainBold (float size) { return juce::Font (juce::FontOptions (size, juce::Font::bold)); }

inline juce::Image distressedText (const juce::String& text, int w, int h, juce::Font font, juce::Colour col,
                                   juce::Justification just, float scale, float wear, int seed, float kerning = 0.0f)
{
    juce::Image img (juce::Image::ARGB, juce::roundToInt ((float) w * scale), juce::roundToInt ((float) h * scale), true);
    {
        juce::Graphics g (img);
        g.addTransform (juce::AffineTransform::scale (scale));
        g.setColour (col);
        g.setFont (font.withExtraKerningFactor (kerning));
        g.drawFittedText (text, 0, 0, w, h, just, 1, 0.55f);   // squeeze rather than clip if the font runs wide
    }
    distress (img, scale, wear, seed);
    return img;
}
} // namespace tex

//==============================================================================
// The Castle Danger crest: a castle silhouette under a blood moon, on a riveted iron medallion.
static void drawCastleCrest (juce::Graphics& g, juce::Rectangle<float> r)
{
    const auto c = r.getCentre();
    const float R = std::min (r.getWidth(), r.getHeight()) * 0.5f;

    // iron ring
    g.setColour (juce::Colours::black.withAlpha (0.6f)); g.fillEllipse (r.translated (1.5f, 2.5f));
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff6a5a4c), c.x - R * 0.6f, c.y - R * 0.8f, juce::Colour (0xff1a1410), c.x + R, c.y + R, true));
    g.fillEllipse (r);
    g.setColour (pal::rust.withAlpha (0.55f)); g.drawEllipse (r.reduced (1.0f), 1.6f);
    for (int i = 0; i < 8; ++i)
    {
        const float a = (float) i / 8.0f * juce::MathConstants<float>::twoPi + 0.39f;
        const float rx = c.x + std::cos (a) * R * 0.88f, ry = c.y + std::sin (a) * R * 0.88f;
        g.setColour (juce::Colour (0xff8a7a68)); g.fillEllipse (rx - 1.8f, ry - 1.8f, 3.6f, 3.6f);
        g.setColour (juce::Colours::black.withAlpha (0.5f)); g.drawEllipse (rx - 1.8f, ry - 1.8f, 3.6f, 3.6f, 0.6f);
    }

    // night sky + blood moon
    const auto inner = r.reduced (R * 0.2f);
    const float ir = inner.getWidth() * 0.5f;
    juce::Graphics::ScopedSaveState ss (g);
    juce::Path clip; clip.addEllipse (inner);
    g.reduceClipRegion (clip);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff2a0c08), c.x, inner.getY(), juce::Colour (0xff080505), c.x, inner.getBottom(), false));
    g.fillEllipse (inner);
    const juce::Rectangle<float> moon (c.x - ir * 0.55f, inner.getY() + ir * 0.12f, ir * 1.1f, ir * 1.1f);
    juce::DropShadow (pal::signalHot.withAlpha (0.55f), (int) (ir * 0.45f), {}).drawForPath (g, [&] { juce::Path p; p.addEllipse (moon); return p; }());
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xffff7a4a), moon.getX() + moon.getWidth() * 0.35f, moon.getY() + moon.getHeight() * 0.3f,
                                             juce::Colour (0xff8a1408), moon.getRight(), moon.getBottom(), true));
    g.fillEllipse (moon);
    g.setColour (juce::Colour (0x40400000));
    g.fillEllipse (moon.getX() + moon.getWidth() * 0.55f, moon.getY() + moon.getHeight() * 0.25f, moon.getWidth() * 0.16f, moon.getWidth() * 0.16f);
    g.fillEllipse (moon.getX() + moon.getWidth() * 0.25f, moon.getY() + moon.getHeight() * 0.55f, moon.getWidth() * 0.12f, moon.getWidth() * 0.12f);

    // castle silhouette
    const float x0 = inner.getX(), y0 = inner.getY(), w = inner.getWidth(), h = inner.getHeight();
    auto X = [&] (float f) { return x0 + f * w; };
    auto Y = [&] (float f) { return y0 + f * h; };
    juce::Path castle;
    auto tower = [&] (float l, float rgt, float top, int merlons)
    {
        castle.addRectangle (X (l), Y (top), X (rgt) - X (l), Y (1.05f) - Y (top));
        const float mw = (rgt - l) / (float) (merlons * 2 - 1);
        for (int i = 0; i < merlons; ++i)
            castle.addRectangle (X (l + (float) i * 2.0f * mw), Y (top - 0.06f), X (l + mw) - X (l), Y (0.06f) - Y (0.0f) + 0.5f);
    };
    tower (0.08f, 0.30f, 0.50f, 3);           // left tower
    tower (0.70f, 0.92f, 0.50f, 3);           // right tower
    tower (0.38f, 0.62f, 0.36f, 3);           // keep
    tower (0.30f, 0.70f, 0.64f, 5);           // curtain wall
    castle.addTriangle (X (0.08f), Y (0.44f), X (0.19f), Y (0.27f), X (0.30f), Y (0.44f));   // spires
    castle.addTriangle (X (0.70f), Y (0.44f), X (0.81f), Y (0.27f), X (0.92f), Y (0.44f));
    castle.setUsingNonZeroWinding (true);
    g.setColour (juce::Colour (0xff070505)); g.fillPath (castle);
    // gate + lit windows
    juce::Path gate;
    gate.addRectangle (X (0.44f), Y (0.84f), X (0.56f) - X (0.44f), Y (0.2f) - Y (0.0f));
    gate.addEllipse (X (0.44f), Y (0.79f), X (0.56f) - X (0.44f), Y (0.1f) - Y (0.0f));
    g.setColour (juce::Colour (0xff2a0c06)); g.fillPath (gate);
    g.setColour (pal::signalHot.withAlpha (0.9f));
    for (auto p : { juce::Point<float> (0.19f, 0.62f), { 0.81f, 0.62f }, { 0.5f, 0.5f } })
        g.fillRect (X (p.x) - w * 0.018f, Y (p.y), w * 0.036f, h * 0.07f);
}

//==============================================================================
ServantLookAndFeel::ServantLookAndFeel()
{
    setColour (juce::Slider::textBoxTextColourId, pal::paintDim);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxHighlightColourId, pal::rust.withAlpha (0.6f));
    setColour (juce::Label::textColourId, pal::paintDim);
    setColour (juce::ComboBox::textColourId, pal::paint);
    setColour (juce::ComboBox::arrowColourId, pal::signal);
    setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff1a1411));
    setColour (juce::PopupMenu::textColourId, pal::paint);
    setColour (juce::PopupMenu::headerTextColourId, pal::rustLight);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, pal::rust);
    setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
    setColour (juce::TextButton::textColourOffId, pal::paint);
    setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff0f0b09));
    setColour (juce::TextEditor::textColourId, pal::paint);
    setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff5a3420));
    setColour (juce::TextEditor::focusedOutlineColourId, pal::rustLight);
    setColour (juce::TextEditor::highlightColourId, pal::rust.withAlpha (0.6f));
    setColour (juce::CaretComponent::caretColourId, pal::signalHot);

    rustDisc = juce::Image (juce::Image::ARGB, 128, 128, true);
    juce::Image::BitmapData bd (rustDisc, juce::Image::BitmapData::writeOnly);
    for (int y = 0; y < 128; ++y)
        for (int x = 0; x < 128; ++x)
        {
            const float n = tex::fbm ((float) x / 22.0f, (float) y / 22.0f, 501, 4);
            const float a = tex::smoothstep (0.5f, 0.7f, n);
            const auto c = tex::rustAt ((float) x * 2.0f, (float) y * 2.0f, 0.3f, 777);
            bd.setPixelColour (x, y, juce::Colour::fromFloatRGBA (c.r, c.g, c.b, a * 0.85f));
        }
}

juce::Font ServantLookAndFeel::getComboBoxFont (juce::ComboBox&) { return tex::plainBold (13.0f); }
juce::Font ServantLookAndFeel::getTextButtonFont (juce::TextButton&, int h) { return tex::stencil (juce::jmin (15.0f, (float) h * 0.62f)).withExtraKerningFactor (0.08f); }

void ServantLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool hi, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (juce::Colours::black.withAlpha (0.55f)); g.fillRoundedRectangle (r.translated (1.0f, 1.5f), 3.0f);
    g.setGradientFill (juce::ColourGradient (down ? pal::rustDeep : juce::Colour (0xff3a2a20), 0, r.getY(),
                                             down ? juce::Colour (0xff2a120a) : juce::Colour (0xff17110e), 0, r.getBottom(), false));
    g.fillRoundedRectangle (r, 3.0f);
    g.setColour (hi ? pal::rustLight : juce::Colour (0xff6a3a20)); g.drawRoundedRectangle (r, 3.0f, 1.2f);
}

void ServantLookAndFeel::drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box)
{
    auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f);
    g.setColour (juce::Colour (0xff0f0b09)); g.fillRoundedRectangle (r, 3.0f);
    g.setColour (box.isMouseOver (true) ? pal::rustLight : juce::Colour (0xff6a3a20)); g.drawRoundedRectangle (r, 3.0f, 1.2f);
    juce::Path arrow; const float ax = (float) w - 16.0f, ay = (float) h * 0.5f;
    arrow.addTriangle (ax - 5, ay - 3, ax + 5, ay - 3, ax, ay + 4);
    g.setColour (pal::signal); g.fillPath (arrow);
}

void ServantLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                        float startAngle, float endAngle, juce::Slider& s)
{
    auto b = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (3.0f);
    const float r = std::min (b.getWidth(), b.getHeight()) * 0.5f;
    const auto c = b.getCentre();
    const float angle = startAngle + pos * (endAngle - startAngle);
    const float thick = juce::jmax (2.5f, r * 0.11f);
    const float alpha = s.isEnabled() ? 1.0f : 0.3f;

    // tick marks stamped round the dial
    g.setColour (pal::paintDim.withAlpha (0.45f * alpha));
    for (int i = 0; i <= 10; ++i)
    {
        const float a = startAngle + (float) i / 10.0f * (endAngle - startAngle) - juce::MathConstants<float>::halfPi;
        const float r1 = r + 1.0f, r0 = r - (i % 5 == 0 ? 5.0f : 3.0f);
        g.drawLine (c.x + std::cos (a) * r0, c.y + std::sin (a) * r0, c.x + std::cos (a) * r1, c.y + std::sin (a) * r1, i % 5 == 0 ? 1.4f : 0.9f);
    }

    const float ar = r - thick - 4.0f;
    juce::Path track; track.addCentredArc (c.x, c.y, ar, ar, 0, startAngle, endAngle, true);
    g.setColour (juce::Colour (0xff120e0c).withAlpha (alpha)); g.strokePath (track, juce::PathStrokeType (thick + 2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (juce::Colour (0xff3a2418).withAlpha (alpha)); g.strokePath (track, juce::PathStrokeType (thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    float from = startAngle;   // bipolar knobs (TRANSIENTS) sweep out from the middle
    if (s.getProperties().contains ("bip")) from = startAngle + (float) s.valueToProportionOfLength (0.0) * (endAngle - startAngle);
    juce::Path val; val.addCentredArc (c.x, c.y, ar, ar, 0, std::min (from, angle), std::max (from, angle), true);
    const bool isAmount = s.getName() == "amount";
    const bool gold = (bool) s.getProperties().getWithDefault ("gold", false);
    if (isAmount)
    {
        // past 100 % it pushes further than the profile asks
        const float dz = startAngle + (100.0f / 150.0f) * (endAngle - startAngle);
        juce::Path zone; zone.addCentredArc (c.x, c.y, r - 1.5f, r - 1.5f, 0, dz, endAngle, true);
        g.setColour (pal::signalHot.withAlpha (0.55f * alpha)); g.strokePath (zone, juce::PathStrokeType (2.0f));
    }
    const bool bipolar = (bool) s.getProperties().getWithDefault ("bipolar", false);
    if (bipolar)
    {
        const float mid = startAngle + 0.5f * (endAngle - startAngle);
        val.clear(); val.addCentredArc (c.x, c.y, ar, ar, 0, std::min (mid, angle), std::max (mid, angle), true);
    }
    const auto arcCol = (gold ? pal::hazard
                              : (isAmount && pos > 100.0f / 150.0f ? pal::signalHot
                                                                   : (s.isMouseOverOrDragging() ? pal::signalHot : pal::signal))).withAlpha (alpha);
    g.setColour (arcCol.withAlpha (0.25f * alpha));
    g.strokePath (val, juce::PathStrokeType (thick + 4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (arcCol);
    g.strokePath (val, juce::PathStrokeType (thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const float br = ar - thick * 1.4f;
    if (br < 4.0f) return;
    const juce::Rectangle<float> body (c.x - br, c.y - br, br * 2, br * 2);
    g.setColour (juce::Colours::black.withAlpha (0.55f)); g.fillEllipse (body.translated (1.5f, 2.5f));
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff6a5a4c).withAlpha (alpha), c.x - br * 0.5f, c.y - br * 0.7f,
                                             juce::Colour (0xff17120f), c.x + br * 0.6f, c.y + br, true));
    g.fillEllipse (body);
    {
        juce::Graphics::ScopedSaveState ss (g);
        juce::Path clip; clip.addEllipse (body);
        g.reduceClipRegion (clip);
        const float off = (float) (((juce::uint64) (juce::pointer_sized_int) &s >> 4) % 64);
        g.setOpacity (0.9f * alpha);
        g.drawImage (rustDisc, body.expanded (br * 0.4f).translated (-off * 0.3f, off * 0.2f), juce::RectanglePlacement::stretchToFit);
    }
    g.setColour (juce::Colour (0xff0f0b09).withAlpha (0.8f));
    for (int i = 0; i < 32; ++i)
    {
        const float a = (float) i / 32.0f * juce::MathConstants<float>::twoPi;
        g.drawLine (c.x + std::cos (a) * br * 0.87f, c.y + std::sin (a) * br * 0.87f,
                    c.x + std::cos (a) * br, c.y + std::sin (a) * br, 1.0f);
    }
    g.setColour (juce::Colour (0xff0b0807)); g.drawEllipse (body, 1.2f);

    juce::Path p; p.addRoundedRectangle (-1.8f, -br * 0.92f, 3.6f, br * 0.6f, 1.2f);
    g.setColour (juce::Colours::black.withAlpha (0.5f));
    g.fillPath (p, juce::AffineTransform::rotation (angle).translated (c.x + 0.8f, c.y + 1.0f));
    g.setColour (pal::paint.withAlpha (alpha));
    g.fillPath (p, juce::AffineTransform::rotation (angle).translated (c));
}

// toggles: an indicator lamp + stencilled label on a rusty plate
void ServantLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool hi, bool)
{
    auto r = b.getLocalBounds().toFloat().reduced (1.0f);
    const bool on = b.getToggleState();
    const bool warn = b.getName() == "compare" || b.getName() == "lock";
    const auto lit = warn ? pal::hazard : pal::signalHot;
    g.setColour (juce::Colours::black.withAlpha (0.55f)); g.fillRoundedRectangle (r.translated (1.0f, 1.5f), 3.0f);
    g.setGradientFill (juce::ColourGradient (on ? juce::Colour (0xff3a1a10) : juce::Colour (0xff2a221d), 0, r.getY(),
                                             juce::Colour (0xff120d0b), 0, r.getBottom(), false));
    g.fillRoundedRectangle (r, 3.0f);
    g.setColour (on ? lit.withAlpha (0.8f) : (hi ? pal::rustLight : juce::Colour (0xff5a3420))); g.drawRoundedRectangle (r, 3.0f, 1.2f);

    const float d = r.getHeight() * 0.42f;
    auto led = juce::Rectangle<float> (d, d).withCentre ({ r.getX() + r.getHeight() * 0.55f, r.getCentreY() });
    if (on)
    {
        juce::Path lp; lp.addEllipse (led);
        juce::DropShadow (lit.withAlpha (0.9f), 10, {}).drawForPath (g, lp);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xfffff0c0), led.getCentreX(), led.getCentreY(), lit, led.getX(), led.getY(), true));
    }
    else g.setColour (juce::Colour (0xff2a1c16));
    g.fillEllipse (led);
    g.setColour (juce::Colours::black.withAlpha (0.7f)); g.drawEllipse (led, 1.0f);

    g.setColour (on ? pal::paint : pal::paintDim);
    g.setFont (r.getWidth() < 120.0f ? tex::stencil (12.5f).withExtraKerningFactor (0.04f) : tex::stencil (15.0f).withExtraKerningFactor (0.1f));
    g.drawFittedText (b.getButtonText(), r.withTrimmedLeft (r.getHeight() * 1.0f).withTrimmedRight (6).toNearestInt(), juce::Justification::centredLeft, 1, 0.6f);
}

//==============================================================================
void BellIcon::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (6.0f);
    const auto c = r.getCentre();
    const float w = r.getWidth();
    if (glow > 0.01f)
    {
        juce::Path halo; halo.addEllipse (r.reduced (w * 0.1f));
        juce::DropShadow (pal::signalHot.withAlpha (0.8f * glow), (int) (w * 0.35f), {}).drawForPath (g, halo);
    }
    const juce::Colour brass (0xffb8893a), brassDark (0xff3a2610);
    // base plate
    const juce::Rectangle<float> base (r.getX() + w * 0.04f, r.getBottom() - w * 0.24f, w * 0.92f, w * 0.2f);
    g.setColour (juce::Colours::black.withAlpha (0.6f)); g.fillEllipse (base.translated (1.5f, 3.0f));
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff4a3a2c), base.getX(), base.getY(), juce::Colour (0xff120c08), base.getRight(), base.getBottom(), false));
    g.fillEllipse (base);
    g.setColour (pal::rust.withAlpha (0.6f)); g.drawEllipse (base, 1.2f);
    // dome
    juce::Path dome;
    const float dy = base.getCentreY() - w * 0.02f;
    dome.startNewSubPath (r.getX() + w * 0.12f, dy);
    dome.cubicTo (r.getX() + w * 0.12f, r.getY() + w * 0.22f, r.getRight() - w * 0.12f, r.getY() + w * 0.22f, r.getRight() - w * 0.12f, dy);
    dome.closeSubPath();
    g.setGradientFill (juce::ColourGradient (brass.brighter (0.4f), c.x - w * 0.2f, r.getY() + w * 0.25f, brassDark, c.x + w * 0.35f, dy, true));
    g.fillPath (dome);
    // tarnish + rust blooms
    {
        juce::Graphics::ScopedSaveState ss (g);
        g.reduceClipRegion (dome);
        for (int i = 0; i < 40; ++i)
        {
            const float x = r.getX() + tex::hash (i, 3, 9) * w, y = r.getY() + w * 0.25f + tex::hash (i, 7, 9) * w * 0.45f;
            const float s = 2.0f + tex::hash (i, 11, 9) * 7.0f;
            g.setColour ((i % 3 == 0 ? pal::rust : juce::Colour (0xff2a3a20)).withAlpha (0.25f + 0.3f * tex::hash (i, 5, 9)));
            g.fillEllipse (x, y, s, s * 0.8f);
        }
        g.setColour (juce::Colours::white.withAlpha (0.35f));
        g.fillEllipse (c.x - w * 0.22f, r.getY() + w * 0.36f, w * 0.12f, w * 0.06f);
    }
    g.setColour (juce::Colours::black.withAlpha (0.7f)); g.strokePath (dome, juce::PathStrokeType (1.2f));
    // plunger
    const juce::Rectangle<float> stem (c.x - w * 0.035f, r.getY() + w * 0.12f, w * 0.07f, w * 0.14f);
    g.setColour (juce::Colour (0xff6a5a4c)); g.fillRect (stem);
    const juce::Rectangle<float> knob (c.x - w * 0.08f, r.getY() + w * 0.04f, w * 0.16f, w * 0.12f);
    g.setGradientFill (juce::ColourGradient (glow > 0.2f ? pal::signalHot : brass.brighter (0.3f), knob.getX(), knob.getY(), brassDark, knob.getRight(), knob.getBottom(), false));
    g.fillEllipse (knob);
    g.setColour (juce::Colours::black.withAlpha (0.7f)); g.drawEllipse (knob, 1.0f);
}

//==============================================================================
void AnalyzeButton::paintButton (juce::Graphics& g, bool hi, bool down)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (juce::Colours::black.withAlpha (0.6f)); g.fillRoundedRectangle (r.translated (1.5f, 2.5f), 5.0f);
    const auto top = listening ? pal::rust.interpolatedWith (pal::signalHot, pulse * 0.6f) : (down ? pal::rustDeep : juce::Colour (0xff6a2a12));
    const auto bot = listening ? pal::rustDeep : juce::Colour (0xff2a100a);
    g.setGradientFill (juce::ColourGradient (top, 0, r.getY(), bot, 0, r.getBottom(), false));
    g.fillRoundedRectangle (r, 5.0f);
    if (listening)
    {
        juce::Path p; p.addRoundedRectangle (r, 5.0f);
        juce::DropShadow (pal::signalHot.withAlpha (0.5f * pulse), 14, {}).drawForPath (g, p);
    }
    g.setColour (hi || listening ? pal::signalHot : juce::Colour (0xff8a4a28)); g.drawRoundedRectangle (r, 5.0f, 1.8f);
    g.setColour (juce::Colours::black.withAlpha (0.5f)); g.drawRoundedRectangle (r.reduced (3.0f), 4.0f, 1.0f);

    auto t = r.reduced (10.0f, 4.0f);
    g.setColour (juce::Colours::black.withAlpha (0.6f));
    g.setFont (tex::stencil (27.0f).withExtraKerningFactor (0.14f));
    g.drawFittedText (getButtonText(), t.removeFromTop (t.getHeight() * 0.64f).translated (1.5f, 2.0f).toNearestInt(), juce::Justification::centredBottom, 1, 0.6f);
    t = r.reduced (10.0f, 4.0f);
    g.setColour (juce::Colour (0xfff0e4c8));
    g.drawFittedText (getButtonText(), t.removeFromTop (t.getHeight() * 0.64f).toNearestInt(), juce::Justification::centredBottom, 1, 0.6f);
    g.setColour (pal::paint.withAlpha (0.8f));
    g.setFont (tex::plainBold (9.5f).withExtraKerningFactor (0.18f));
    g.drawFittedText (sub, t.toNearestInt(), juce::Justification::centred, 1, 0.6f);
}

//==============================================================================
static juce::Font ledFont (float size) { return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), size, juce::Font::bold)); }

void StatusScreen::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (pal::screen); g.fillRoundedRectangle (r, 4.0f);
    g.setColour (juce::Colour (0xff3a2418)); g.drawRoundedRectangle (r, 4.0f, 1.0f);
    auto t = r.reduced (10.0f, 7.0f);
    const float lh = t.getHeight() / (float) juce::jmax (4, lines.size());
    for (int i = 0; i < lines.size(); ++i)
    {
        g.setColour (i < colours.size() ? colours[i] : pal::paintDim);
        g.setFont (ledFont (i == 0 ? 12.5f : 11.0f).withExtraKerningFactor (0.04f));
        g.drawFittedText (lines[i], t.removeFromTop (lh).toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);
    }
}

//==============================================================================
static constexpr float kTopDb = 12.0f, kBotDb = -30.0f, kDisplayRate = 48000.0f;

float CurveDisplay::xFor (float hz) const
{
    const auto p = plot();
    return p.getX() + p.getWidth() * (std::log10 (juce::jlimit (20.0f, 20000.0f, hz)) - std::log10 (20.0f)) / 3.0f;
}
float CurveDisplay::hzFor (float x) const
{
    const auto p = plot();
    return std::pow (10.0f, std::log10 (20.0f) + 3.0f * (x - p.getX()) / p.getWidth());
}
float CurveDisplay::yFor (float db) const
{
    const auto p = plot();
    return p.getY() + p.getHeight() * (kTopDb - db) / (kTopDb - kBotDb);
}
float CurveDisplay::dbFor (float y) const
{
    const auto p = plot();
    return kTopDb - (y - p.getY()) / p.getHeight() * (kTopDb - kBotDb);
}

float CurveDisplay::eqDb (float hz, const svt::Params& p) const
{
    double db = 0;
    if (p.toneOn)
        for (int i = 0; i < svt::kTone; ++i)
            if (std::abs (p.tone[i].g * p.amount) > 0.01f)
            {
                svt::Biquad b; b.peak (kDisplayRate, p.tone[i].f, p.tone[i].q, p.tone[i].g * p.amount);
                db += b.magnitudeDb (hz, kDisplayRate);
            }
    if (p.cleanOn)
    {
        svt::Biquad b;
        if (p.hpf > 20.5f) { b.highpass (kDisplayRate, p.hpf, 0.5412); db += b.magnitudeDb (hz, kDisplayRate); b.highpass (kDisplayRate, p.hpf, 1.3066); db += b.magnitudeDb (hz, kDisplayRate); }
        if (p.lpf < 19900.0f) { b.lowpass (kDisplayRate, p.lpf, 0.7071); db += b.magnitudeDb (hz, kDisplayRate); }
    }
    return (float) db;
}

void CurveDisplay::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    g.setColour (pal::screen); g.fillRoundedRectangle (b, 5.0f);
    g.setColour (juce::Colour (0xff3a2418)); g.drawRoundedRectangle (b, 5.0f, 1.0f);
    const auto p = plot();
    const auto params = proc.currentParams();

    // grid
    g.setFont (ledFont (9.5f));
    for (float hz : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
    {
        const float x = xFor (hz);
        g.setColour (juce::Colour (0xff2a1c16)); g.drawVerticalLine ((int) x, p.getY(), p.getBottom());
        g.setColour (pal::paintDim.withAlpha (0.55f));
        g.drawText (hz >= 1000.0f ? juce::String ((int) (hz / 1000.0f)) + "k" : juce::String ((int) hz), (int) x - 20, (int) p.getBottom() + 1, 40, 12, juce::Justification::centred);
    }
    for (float db : { 12.0f, 0.0f, -12.0f, -24.0f })
    {
        const float y = yFor (db);
        g.setColour (db == 0.0f ? juce::Colour (0xff4a3020) : juce::Colour (0xff2a1c16)); g.drawHorizontalLine ((int) y, p.getX(), p.getRight());
        g.setColour (pal::paintDim.withAlpha (0.55f));
        g.drawText ((db > 0 ? "+" : "") + juce::String ((int) db), 0, (int) y - 6, (int) p.getX() - 4, 12, juce::Justification::centredRight);
    }

    juce::Graphics::ScopedSaveState ss (g);
    g.reduceClipRegion (p.toNearestInt().withTrimmedTop (-12));
    {
        const auto track = proc.getTrackName();
        g.setColour (pal::hazard.withAlpha (0.8f)); g.setFont (ledFont (10.5f));
        g.drawText ("ROLE: " + proc.getRoleName().toUpperCase() + (track.isNotEmpty() ? "  //  " + track.toUpperCase() : juce::String()),
                    juce::Rectangle<float> (p.getX() + 2.0f, p.getY() - 12.0f, p.getWidth() * 0.5f, 11.0f), juce::Justification::centredLeft);
    }
    const auto& a = proc.getAnalysis();

    auto curvePath = [&] (std::function<float (int)> dbAt)
    {
        juce::Path path;
        for (int k = 0; k < svt::kBands; ++k)
        {
            const juce::Point<float> pt (xFor (svt::bandHz (k)), yFor (dbAt (k)));
            if (k == 0) path.startNewSubPath (pt); else path.lineTo (pt);
        }
        return path;
    };

    // EQ curve (what The Servant is doing), around 0 dB
    {
        juce::Path eq; const float y0 = yFor (0.0f);
        const int n = (int) p.getWidth();
        juce::Path fill; fill.startNewSubPath (p.getX(), y0);
        for (int i = 0; i <= n; i += 2)
        {
            const float x = p.getX() + (float) i;
            const float y = yFor (juce::jlimit (kBotDb - 2.0f, kTopDb + 2.0f, eqDb (hzFor (x), params)));
            if (i == 0) eq.startNewSubPath (x, y); else eq.lineTo (x, y);
            fill.lineTo (x, y);
        }
        fill.lineTo (p.getRight(), y0); fill.closeSubPath();
        g.setColour (pal::rustLight.withAlpha (0.12f)); g.fillPath (fill);
        g.setColour (pal::rustLight.withAlpha (0.75f)); g.strokePath (eq, juce::PathStrokeType (1.4f));
    }

    if (a.valid)
    {
        // the track as heard, and as it will come out
        g.setColour (pal::paintDim.withAlpha (0.8f));
        g.strokePath (curvePath ([&a] (int k) { return a.measured[k]; }), juce::PathStrokeType (1.3f));
        float result[svt::kBands];
        for (int k = 0; k < svt::kBands; ++k) result[k] = a.measured[k] + eqDb (svt::bandHz (k), params);
        svt::normalise (result);
        auto rp = curvePath ([&result] (int k) { return result[k]; });
        g.setColour (pal::signalHot.withAlpha (0.3f)); g.strokePath (rp, juce::PathStrokeType (5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (pal::signalHot); g.strokePath (rp, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    else
    {
        g.setColour (pal::paint.withAlpha (0.55f));
        g.setFont (tex::stencil (16.0f).withExtraKerningFactor (0.12f));
        g.drawText (! proc.getLink().masterNearby() ? "WAITING FOR THE MASTER"
                    : (proc.getStage() == ServantProcessor::Stage::listening ? "LISTENING...  THE MASTER DECIDES AT PRESCRIBE" : "PLAY, THEN RESET ON THE MASTER"),
                    p.withHeight (p.getHeight() * 0.28f).toNearestInt(), juce::Justification::centred);
    }

    // resonances: a marker on top, live gain reduction hanging from it
    int nRes = 0; const auto* res = proc.getAppliedRes (nRes);
    for (int i = 0; i < nRes; ++i)
    {
        const float x = xFor (res[i].f);
        const bool muted = proc.getEngine().muted[(size_t) i].load();
        const float gr = proc.getEngine().grDb[(size_t) i].load();
        const auto col = muted ? pal::paintDim.withAlpha (0.4f) : (i == hoverRes ? pal::signalHot : pal::signal);
        if (! muted && gr > 0.05f)
        {
            g.setColour (pal::signalHot.withAlpha (0.55f));
            g.fillRect (x - 2.5f, p.getY(), 5.0f, yFor (-gr) - yFor (0.0f));
        }
        juce::Path tri; tri.addTriangle (x - 6, p.getY() - 11, x + 6, p.getY() - 11, x, p.getY() - 1);
        g.setColour (col); g.fillPath (tri);
        if (muted) { g.setColour (pal::paint.withAlpha (0.7f)); g.drawLine (x - 4, p.getY() - 10, x + 4, p.getY() - 2, 1.2f); g.drawLine (x + 4, p.getY() - 10, x - 4, p.getY() - 2, 1.2f); }
    }

    // tone EQ nodes
    for (int i = 0; i < svt::kTone; ++i)
    {
        const float gn = params.tone[i].g * params.amount;
        const bool active = std::abs (params.tone[i].g) > 0.01f;
        if (! active && i != hoverNode && i != dragNode) continue;
        const juce::Point<float> c (xFor (params.tone[i].f), yFor (gn));
        const bool gold = proc.isUserSet (i);
        const auto col = gold ? pal::hazard : pal::signalHot;
        const float rr = (i == hoverNode || i == dragNode) ? 8.0f : 6.5f;
        g.setColour (juce::Colours::black.withAlpha (0.6f)); g.fillEllipse (c.x - rr + 1, c.y - rr + 1.5f, rr * 2, rr * 2);
        g.setColour (active ? col : col.withAlpha (0.35f)); g.fillEllipse (c.x - rr, c.y - rr, rr * 2, rr * 2);
        g.setColour (juce::Colour (0xff120c08)); g.setFont (ledFont (9.5f));
        g.drawText (juce::String (i + 1), juce::Rectangle<float> (c.x - rr, c.y - rr, rr * 2, rr * 2), juce::Justification::centred);
    }

    // legend
    g.setFont (ledFont (9.5f));
    auto lg = juce::Rectangle<float> (p.getRight() - 224, p.getBottom() - 16, 224, 13);
    g.setColour (pal::screen.withAlpha (0.85f)); g.fillRoundedRectangle (lg.expanded (6.0f, 3.0f), 3.0f);
    auto item = [&] (juce::Colour c, const juce::String& t, float w)
    {
        auto r = lg.removeFromLeft (w);
        g.setColour (c); g.fillRect (r.removeFromLeft (12).withSizeKeepingCentre (12, 3));
        g.setColour (pal::paintDim); g.drawText (t, r.withTrimmedLeft (4), juce::Justification::centredLeft);
    };
    item (pal::paintDim, "TRACK", 60);
    item (pal::signalHot, "RESULT", 66);
    item (pal::rustLight, "EQ", 40);
}

int CurveDisplay::nodeAt (juce::Point<float> pos) const
{
    const auto params = proc.currentParams();
    int best = -1; float bd = 12.0f;
    for (int i = 0; i < svt::kTone; ++i)
    {
        const float d = pos.getDistanceFrom ({ xFor (params.tone[i].f), yFor (params.tone[i].g * params.amount) });
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

int CurveDisplay::resAt (juce::Point<float> pos) const
{
    const auto p = plot();
    if (pos.y > p.getY() + 4.0f || pos.y < p.getY() - 16.0f) return -1;
    int n = 0; const auto* res = proc.getAppliedRes (n);
    for (int i = 0; i < n; ++i) if (std::abs (pos.x - xFor (res[i].f)) < 7.0f) return i;
    return -1;
}

static juce::String hzText (float hz) { return hz >= 1000.0f ? juce::String (hz / 1000.0f, 2) + " kHz" : juce::String (juce::roundToInt (hz)) + " Hz"; }

void CurveDisplay::mouseMove (const juce::MouseEvent& e)
{
    const int r = resAt (e.position), n = r < 0 ? nodeAt (e.position) : -1;
    if (r != hoverRes || n != hoverNode) { hoverRes = r; hoverNode = n; repaint(); }
    if (r >= 0)
    {
        int cnt = 0; const auto* res = proc.getAppliedRes (cnt);
        const bool muted = proc.getEngine().muted[(size_t) r].load();
        setTooltip ("Resonance at " + hzText (res[r].f) + (muted ? ": muted. Click to let The Servant work on it again." : ". Click to mute it (leave it alone)."));
    }
    else if (n >= 0)
    {
        const auto params = proc.currentParams();
        setTooltip ("Tone band " + juce::String (n + 1) + ": " + hzText (params.tone[n].f) + ", " + juce::String (params.tone[n].g, 1) + " dB, Q "
                    + juce::String (params.tone[n].q, 2) + ". Drag to move it, scroll to make it wider or narrower.");
    }
    else setTooltip ("Grey: your track. Red: how it will come out. Orange: what The Servant is doing. "
                     "Drag the numbered dots to override a tone move; click a triangle up top to mute a resonance.");
    setMouseCursor (n >= 0 || r >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
}

void CurveDisplay::setParam (const juce::String& id, float v)
{
    if (auto* rp = proc.apvts.getParameter (id)) rp->setValueNotifyingHost (rp->convertTo0to1 (v));
}

void CurveDisplay::mouseDown (const juce::MouseEvent& e)
{
    const int r = resAt (e.position);
    if (r >= 0)
    {
        auto& m = proc.getEngine().muted[(size_t) r];
        m = ! m.load();
        repaint();
        return;
    }
    dragNode = nodeAt (e.position);
    if (dragNode >= 0)
        for (auto* w : { "f", "g" }) proc.apvts.getParameter (ServantProcessor::toneId (w, dragNode))->beginChangeGesture();
}

void CurveDisplay::mouseDrag (const juce::MouseEvent& e)
{
    if (dragNode < 0) return;
    const float amount = juce::jmax (0.05f, proc.currentParams().amount);
    setParam (ServantProcessor::toneId ("f", dragNode), juce::jlimit (20.0f, 20000.0f, hzFor (e.position.x)));
    setParam (ServantProcessor::toneId ("g", dragNode), juce::jlimit (-12.0f, 12.0f, dbFor (e.position.y) / amount));
    repaint();
}

void CurveDisplay::mouseUp (const juce::MouseEvent&)
{
    if (dragNode >= 0)
        for (auto* w : { "f", "g" }) proc.apvts.getParameter (ServantProcessor::toneId (w, dragNode))->endChangeGesture();
    dragNode = -1;
}

void CurveDisplay::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const int n = nodeAt (e.position);
    if (n < 0) return;
    const auto id = ServantProcessor::toneId ("q", n);
    const float q = proc.apvts.getRawParameterValue (id)->load();
    auto* rp = proc.apvts.getParameter (id);
    rp->beginChangeGesture();
    setParam (id, juce::jlimit (0.3f, 6.0f, q * std::pow (1.15f, w.deltaY > 0 ? 1.0f : -1.0f)));
    rp->endChangeGesture();
    repaint();
}

//==============================================================================
ServantEditor::ServantEditor (ServantProcessor& p)
    : AudioProcessorEditor (&p), proc (p), curve (p)
{
    setLookAndFeel (&lnf);

    amountKnob = &makeKnob ("amount", "AMOUNT",
        "How much of the tone EQ and resonance control you hear. 100% = exactly what The Master and The Servant decided. Above 100% pushes further, 0% = only the clean-up filters.", 21.0f);
    amountKnob->slider.setName ("amount");
    hpfKnob = &makeKnob ("hpf", "HIGH-PASS", "Cuts rumble below the track's useful range (24 dB/oct). Decided at PRESCRIBE from where this track's content starts, and its ROLE. Turn it to override (it turns gold).");
    lpfKnob = &makeKnob ("lpf", "LOW-PASS", "Rolls off fizz above the track's useful range (12 dB/oct). Decided at PRESCRIBE for rhythm parts; turn fully right for OFF.");
    depthKnob = &makeKnob ("resDepth", "DEPTH", "How hard the resonances are pushed down when they ring. They're only cut while they ring, never all the time.");
    sharpKnob = &makeKnob ("resSharp", "SHARPNESS", "How narrow the resonance cuts are. Higher = surgical, lower = broader and smoother.");
    speedKnob = &makeKnob ("resSpeed", "SPEED", "How fast the resonance cuts react and let go. Faster for picking and drums, slower for sustained chords.");
    for (int i = 0; i < svt::kTone; ++i)
    {
        toneKnobs[i] = &makeKnob (ServantProcessor::toneId ("g", i), "BAND " + juce::String (i + 1),
                                  "Tone band " + juce::String (i + 1) + " gain. Set by The Master from the whole mix; turn it to override (it turns gold). Move its frequency on the curve above.");
        toneKnobs[i]->slider.getProperties().set ("bipolar", true);
    }

    for (auto* b : { &matchButton, &compareButton, &cleanOn, &resOn, &resLock, &toneOn, &toneLock }) addAndMakeVisible (*b);
    compareButton.setName ("compare");
    resLock.setName ("lock"); toneLock.setName ("lock");
    matchButton.setTooltip ("Keeps the output at the same loudness as the input, so you judge the sound, not the volume. Leave it on.");
    compareButton.setTooltip ("Hear the untouched track (same timing, same level) to check The Servant made it better. Turn it off before you print!");
    cleanOn.setTooltip ("High-pass and low-pass clean-up.");
    resOn.setTooltip ("The resonance suppressor: finds frequencies that ring and ducks only those, only while they ring.");
    resLock.setTooltip ("Keeps the current resonances the next time The Master prescribes.");
    toneOn.setTooltip ("Broad, gentle EQ moves, set by The Master so this track fits the mix and the genre.");
    toneLock.setTooltip ("Keeps this tone EQ and clean-up as they are: The Master won't change them.");
    auto attach = [this] (const char* id, juce::ToggleButton& b)
    { buttonAtts.push_back (std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, id, b)); };
    attach ("match", matchButton); attach ("compare", compareButton); attach ("cleanOn", cleanOn);
    attach ("resOn", resOn); attach ("resLock", resLock); attach ("toneOn", toneOn); attach ("toneLock", toneLock);

    addAndMakeVisible (analyzeButton);
    analyzeButton.setTooltip ("What The Servant is doing. The Master runs it: RESET on The Master starts the listening, PRESCRIBE finishes it.");
    addAndMakeVisible (status);
    status.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (curve);
    addAndMakeVisible (bell);
    bell.setInterceptsMouseClicks (false, false);

    // ---- MIX LINK (Castle Danger Link to The Master)
    compKnob = &makeKnob ("comp", "AMOUNT", "Compressor amount. The threshold follows the track's own level, so it works the same on any track. The Master sets this when the track's peaks stick out of the mix.");
    trimKnob = &makeKnob ("trim", "LEVEL", "The track's level in the mix, after MATCH (so it really changes the balance). The Master sets this to balance the mix; move it any time.");
    deessKnob = &makeKnob ("deess", "AMOUNT", "How hard the de-esser turns down the \"s\" sounds. Only the top end is turned down, and only while an \"s\" is there. The Master sets this when the track is sibilant.");
    deessHzKnob = &makeKnob ("deessHz", "FREQ", "Where the de-esser splits the sound: everything above it is turned down during an \"s\". Set from where this track's sibilance sits.");
    deessOn.setTooltip ("De-esser on/off. The Master turns it on for sibilant tracks (usually the lead vocal).");
    buttonAtts.push_back (std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, "deessOn", deessOn));
    addAndMakeVisible (deessOn);
    deessGr.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::bold)));
    deessGr.setColour (juce::Label::textColourId, pal::paintDim);
    deessGr.setJustificationType (juce::Justification::centred);
    deessGr.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (deessGr);
    transAtkKnob = &makeKnob ("transAtk", "ATTACK", "Transient shaper: more (+) or less (-) of each hit's attack. + makes drums snap and picking cut through; - softens harsh or clicky attacks. The Master sets it by ROLE and how punchy the mix is.");
    transSusKnob = &makeKnob ("transSus", "SUSTAIN", "Transient shaper: longer (+) or shorter (-) tails after each hit. - tightens a boomy kick or roomy drums; + brings out ring and body.");
    transAtkKnob->slider.getProperties().set ("bip", true);
    transSusKnob->slider.getProperties().set ("bip", true);
    transOn.setTooltip ("Transient shaper on/off. The Master turns it on for tracks whose hits need more snap, or less.");
    buttonAtts.push_back (std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, "transOn", transOn));
    addAndMakeVisible (transOn);
    transMove.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::bold)));
    transMove.setColour (juce::Label::textColourId, pal::paintDim);
    transMove.setJustificationType (juce::Justification::centred);
    transMove.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (transMove);
    compOn.setTooltip ("Compressor on/off.");
    compStyleBox.addItemList ({ "AUTO", "PUNCH", "SMOOTH", "TIGHT", "GLUE" }, 1);
    compStyleBox.setTooltip ("How the compressor reacts. AUTO follows the ROLE: PUNCH for kick, snare and drums (lets each hit's crack through, then holds the body), "
                             "SMOOTH for the lead (vocal levelling: catches early, lets go slowly), TIGHT for bass (every note at the same level), GLUE for everything else (gentle).");
    compStyleAtt = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (p.apvts, "compStyle", compStyleBox);
    addAndMakeVisible (compStyleBox);
    compRead.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::bold)));
    compRead.setColour (juce::Label::textColourId, pal::paintDim);
    compRead.setJustificationType (juce::Justification::centred);
    compRead.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (compRead);
    buttonAtts.push_back (std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, "compOn", compOn));
    addAndMakeVisible (compOn);
    for (int idx : svt::roleMenuOrder()) roleBox.addItem (ServantProcessor::roleNames()[idx], idx + 1);   // item id = parameter index + 1
    roleBox.setTooltip ("What this track is in the song. The Master balances the mix by it (the lead vocal and lead guitar stay on top, "
                        "backing vocals sit under the lead, kick and bass own the low end), and The Servant picks its clean-up by it.");
    roleAtt = std::make_unique<juce::ParameterAttachment> (*p.apvts.getParameter ("role"),
                                                           [this] (float v) { roleBox.setSelectedId (juce::roundToInt (v) + 1, juce::dontSendNotification); });
    roleBox.onChange = [this] { if (roleBox.getSelectedId() > 0) roleAtt->setValueAsCompleteGesture ((float) (roleBox.getSelectedId() - 1)); };
    roleAtt->sendInitialUpdate();
    addAndMakeVisible (roleBox);
    linkLabel.setFont (juce::Font (juce::FontOptions (12.5f)));
    linkLabel.setColour (juce::Label::textColourId, pal::paint);
    linkLabel.setJustificationType (juce::Justification::topLeft);
    linkLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (linkLabel);
    linkLabel.setText ("Not linked. The Servant works with The Master: put The Master last on your master bus and use its MY PLUGINS mode.", juce::dontSendNotification);
    linkLabel.setColour (juce::Label::textColourId, pal::paintDim);

    setSize (1060, 840);
    updateStatus();
    startTimerHz (30);
}

ServantEditor::~ServantEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

ServantEditor::Knob& ServantEditor::makeKnob (const juce::String& id, const juce::String& text, const juce::String& tip, float labelSize)
{
    auto k = std::make_unique<Knob>();
    k->label.setText (text, juce::dontSendNotification);
    k->label.setJustificationType (juce::Justification::centred);
    k->label.setFont (tex::stencil (labelSize).withExtraKerningFactor (0.1f));
    k->label.setColour (juce::Label::textColourId, pal::paint);
    k->label.setMinimumHorizontalScale (0.6f);
    k->slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 76, 17);
    k->slider.setColour (juce::Slider::textBoxTextColourId, pal::paint.withAlpha (0.85f));
    k->slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colour (0xff3a2418));
    k->slider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0x90000000));
    k->slider.setMouseDragSensitivity (220);
    k->slider.setTooltip (tip);
    k->att = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.apvts, id, k->slider);
    k->slider.setDoubleClickReturnValue (true, proc.apvts.getParameter (id)->convertFrom0to1 (proc.apvts.getParameter (id)->getDefaultValue()));
    addAndMakeVisible (k->slider);
    addAndMakeVisible (k->label);
    knobs.push_back (std::move (k));
    return *knobs.back();
}

void ServantEditor::placeKnob (Knob& k, juce::Rectangle<int> cell)
{
    k.label.setBounds (cell.removeFromTop (18));
    k.slider.setBounds (cell);
}

void ServantEditor::resized()
{
    plates.clear();
    bell.setBounds (16, 10, 90, 92);
    logoArea  = { 114, 8, 400, 70 };
    badgeArea = { 548, 14, 260, 86 };
    crestArea = { 556, 19, 76, 76 };
    roleArea = { 842, 14, 200, 86 };
    roleBox.setBounds (842, 38, 200, 28);
    stripeArea = { 0, 110, getWidth(), 18 };

    plates.push_back ({ "ORDERS", { 16, 138, 300, 250 }, true });
    analyzeButton.setBounds (28, 164, 276, 62);
    status.setBounds (30, 236, 272, 94);
    matchButton.setBounds (28, 340, 134, 36);
    compareButton.setBounds (170, 340, 134, 36);

    plates.push_back ({ "THE CURVE", { 326, 138, 718, 250 } });
    curve.setBounds (338, 162, 694, 214);

    plates.push_back ({ "HOW HARD", { 16, 398, 170, 290 } });
    placeKnob (*amountKnob, { 24, 440, 154, 196 });

    plates.push_back ({ "CLEAN-UP", { 196, 398, 170, 290 } });
    cleanOn.setBounds (208, 424, 146, 30);
    placeKnob (*hpfKnob, { 216, 462, 130, 108 });
    placeKnob (*lpfKnob, { 216, 574, 130, 108 });

    plates.push_back ({ "RESONANCE", { 376, 398, 290, 290 } });
    resOn.setBounds (388, 424, 128, 30);
    resLock.setBounds (526, 424, 128, 30);
    placeKnob (*depthKnob, { 386, 470, 90, 120 });
    placeKnob (*sharpKnob, { 476, 470, 90, 120 });
    placeKnob (*speedKnob, { 566, 470, 90, 120 });

    plates.push_back ({ "TONE EQ", { 676, 398, 368, 290 } });
    toneOn.setBounds (688, 424, 166, 30);
    toneLock.setBounds (866, 424, 166, 30);
    for (int i = 0; i < svt::kTone; ++i)
        placeKnob (*toneKnobs[i], { 688 + (i % 3) * 116, 462 + (i / 3) * 112, 112, 108 });

    plates.push_back ({ "MIX LINK  //  THE MASTER", { 16, 698, 250, 130 } });
    linkLabel.setBounds (28, 726, 228, 96);
    plates.push_back ({ "DE-ESSER", { 276, 698, 260, 130 } });
    deessOn.setBounds (288, 728, 70, 30);
    deessGr.setBounds (288, 766, 70, 40);
    placeKnob (*deessKnob, { 362, 712, 84, 110 });
    placeKnob (*deessHzKnob, { 446, 712, 84, 110 });
    plates.push_back ({ "COMPRESSOR", { 546, 698, 172, 130 } });
    compOn.setBounds (556, 724, 70, 28);
    compStyleBox.setBounds (556, 758, 70, 22);
    compRead.setBounds (552, 784, 78, 34);
    placeKnob (*compKnob, { 630, 724, 84, 100 });
    plates.push_back ({ "TRANSIENTS", { 728, 698, 214, 130 } });
    transOn.setBounds (738, 728, 62, 30);
    transMove.setBounds (738, 766, 62, 40);
    placeKnob (*transAtkKnob, { 802, 724, 68, 100 });
    placeKnob (*transSusKnob, { 870, 724, 68, 100 });
    plates.push_back ({ "TRIM", { 952, 698, 92, 130 } });
    placeKnob (*trimKnob, { 954, 724, 88, 100 });

    buildBackdrop();
}

void ServantEditor::updateStatus()
{
    const auto stage = proc.getStage();
    const auto& a = proc.getAnalysis();
    const bool linked = proc.getLink().masterNearby();
    juce::StringArray l; juce::Array<juce::Colour> c;
    auto add = [&] (const juce::String& s, juce::Colour col) { l.add (s); c.add (col); };
    auto mmss = [] (float s) { const int t = juce::roundToInt (s); return juce::String (t / 60) + ":" + juce::String (t % 60).paddedLeft ('0', 2); };
    int nRes = 0; proc.getAppliedRes (nRes);

    if (! linked)
    {
        analyzeButton.setButtonText ("WAITING");
        analyzeButton.sub = "FOR THE MASTER ON THE MASTER BUS";
        add ("AT YOUR SERVICE.", pal::signalHot);
        add ("1. SET THIS TRACK'S ROLE (TOP RIGHT)", pal::paint);
        add ("2. PUT THE MASTER LAST ON THE MASTER BUS", pal::paint);
        add ("3. ITS MY PLUGINS MODE RUNS ME", pal::paint);
    }
    else if (stage == ServantProcessor::Stage::listening)
    {
        analyzeButton.setButtonText ("LISTENING");
        analyzeButton.sub = "THE MASTER DECIDES AT PRESCRIBE";
        add ("LISTENING...  " + mmss (proc.getEngine().analyzer.learnedSeconds()), pal::signalHot);
        add ("PLAY THE WHOLE SONG.", pal::paint);
        add (a.valid ? "STILL USING: " + juce::String (nRes) + " RESONANCES, HPF " + juce::String (juce::roundToInt (proc.getCleanup().hpf)) + " HZ"
                     : "SILENCE AND BLEED DON'T COUNT.", pal::paintDim);
        add ("THEN PRESCRIBE ON THE MASTER", pal::hazard);
    }
    else
    {
        analyzeButton.setButtonText ("SERVED");
        analyzeButton.sub = "RESET ON THE MASTER TO LISTEN AGAIN";
        const auto& cl = proc.getCleanup();
        add ("HEARD " + mmss (a.seconds) + " OF THIS TRACK", pal::signalHot);
        add ("CLEAN-UP: " + juce::String (juce::roundToInt (cl.hpf)) + " HZ" + (cl.lpf < 19900.0f ? " TO " + juce::String (cl.lpf / 1000.0f, 1) + " KHZ" : juce::String (" UP")), pal::paint);
        add (nRes == 0 ? "RESONANCES: NONE FOUND" : "RESONANCES: " + juce::String (nRes) + " FOUND", pal::paint);
        add ("TONE, COMP, LEVEL: FROM THE MASTER", pal::hazard);
    }
    analyzeButton.listening = linked && stage == ServantProcessor::Stage::listening;
    if (l != status.lines) { status.lines = l; status.colours = c; status.repaint(); }
}

void ServantEditor::timerCallback()
{
    if (++linkTick >= 15)
    {
        linkTick = 0;
        auto& lk = proc.getLink();
        const auto track = proc.getTrackName();
        juce::String t;
        if (! lk.masterNearby())
            t = "Not linked. The Servant works with The Master: put The Master last on your master bus and use its MY PLUGINS mode.";
        else
        {
            t = "LINKED TO THE MASTER" + (track.isNotEmpty() ? "  //  track: " + track : juce::String());
            const bool recent = lk.getLastNoteTime() > 0 && juce::Time::getMillisecondCounter() - lk.getLastNoteTime() < 60000;
            t << "\n" << (recent ? "Just set by The Master: tone EQ, compressor and trim. Override anything; your changes show gold."
                                  : "RESET on The Master starts the listening; PRESCRIBE decides this track's clean-up and resonances here, "
                                    "and its tone, compression and level from the whole mix. APPLY sets them.");
        }
        linkLabel.setColour (juce::Label::textColourId, lk.masterNearby() ? pal::paint : pal::paintDim);
        linkLabel.setText (t, juce::dontSendNotification);
    }
    const bool listening = proc.getStage() == ServantProcessor::Stage::listening;
    phase += 0.12f;
    const float pulse = listening ? 0.5f + 0.5f * std::sin (phase) : 0.0f;
    if (listening || analyzeButton.pulse > 0.0f) { analyzeButton.pulse = pulse; analyzeButton.repaint(); }
    const float bellTarget = listening ? 0.4f + 0.6f * pulse : 0.0f;
    if (std::abs (bell.glow - bellTarget) > 0.01f) { bell.glow += (bellTarget - bell.glow) * 0.3f; bell.repaint(); }

    updateStatus();
    if (proc.analysisVersion.load() != lastVersion) lastVersion = proc.analysisVersion.load();

    // knobs the user has turned since the analysis go gold; tone knobs are named after their frequency
    const auto params = proc.currentParams();
    for (int i = 0; i < svt::kTone; ++i)
    {
        const auto f = params.tone[i].f;
        const auto t = f >= 1000.0f ? juce::String (f / 1000.0f, f >= 10000.0f ? 1 : 2) + " kHz" : juce::String (juce::roundToInt (f)) + " Hz";
        if (toneKnobs[i]->label.getText() != t) toneKnobs[i]->label.setText (t, juce::dontSendNotification);
        const bool gold = proc.isUserSet (i);
        if ((bool) toneKnobs[i]->slider.getProperties()["gold"] != gold) { toneKnobs[i]->slider.getProperties().set ("gold", gold); toneKnobs[i]->slider.repaint(); }
    }
    for (auto [k, flag] : { std::pair<Knob*, int> { hpfKnob, ServantProcessor::kHpfFlag }, { lpfKnob, ServantProcessor::kLpfFlag } })
    {
        const bool gold = proc.isUserSet (flag);
        if ((bool) k->slider.getProperties()["gold"] != gold) { k->slider.getProperties().set ("gold", gold); k->slider.repaint(); }
    }
    for (auto* k : { hpfKnob, lpfKnob }) k->slider.setEnabled (params.cleanOn);
    for (auto* k : { deessKnob, deessHzKnob }) k->slider.setEnabled (params.deessOn);
    {
        const float gr = proc.getEngine().deGrDb.exchange (0.0f);
        deessGrShown = std::max (gr, deessGrShown * 0.9f);
        const auto t = ! params.deessOn ? juce::String ("OFF") : "-" + juce::String (deessGrShown, 1) + "\ndB";
        if (deessGr.getText() != t) deessGr.setText (t, juce::dontSendNotification);
        deessGr.setColour (juce::Label::textColourId, deessGrShown > 0.5f ? pal::signalHot : pal::paintDim);
        const bool gold = proc.isUserSet (ServantProcessor::kDeessHzFlag);
        if ((bool) deessHzKnob->slider.getProperties()["gold"] != gold) { deessHzKnob->slider.getProperties().set ("gold", gold); deessHzKnob->slider.repaint(); }
    }
    for (auto* k : { transAtkKnob, transSusKnob }) k->slider.setEnabled (params.transOn);
    {
        const float gr = proc.getEngine().compGrDb.exchange (0.0f);
        compGrShown = std::max (gr, compGrShown * 0.9f);
        const juce::String style = svt::compStyle (params.compStyle).name;
        const auto t = ! params.compOn ? juce::String ("OFF") : style + "\n-" + juce::String (compGrShown, 1) + " dB";
        if (compRead.getText() != t) compRead.setText (t, juce::dontSendNotification);
        compRead.setColour (juce::Label::textColourId, compGrShown > 0.5f ? pal::signalHot : pal::paintDim);
    }
    {
        const float mv = proc.getEngine().tsDb.exchange (0.0f);
        transShown = std::abs (mv) > std::abs (transShown) * 0.9f ? mv : transShown * 0.9f;
        const auto t = ! params.transOn ? juce::String ("OFF") : (transShown >= 0 ? "+" : "") + juce::String (transShown, 1) + "\ndB";
        if (transMove.getText() != t) transMove.setText (t, juce::dontSendNotification);
        transMove.setColour (juce::Label::textColourId, std::abs (transShown) > 0.5f ? pal::signalHot : pal::paintDim);
    }
    for (auto* k : { depthKnob, sharpKnob, speedKnob }) k->slider.setEnabled (params.resOn);
    for (auto* k : toneKnobs) k->slider.setEnabled (params.toneOn);
    curve.repaint();
}

//==============================================================================
void ServantEditor::buildBackdrop()
{
    const float scale = 2.0f;
    const int W = getWidth(), H = getHeight();
    if (W <= 0 || H <= 0) return;
    const int PW = (int) ((float) W * scale), PH = (int) ((float) H * scale);
    juce::Image base (juce::Image::RGB, W, H, false);

    std::vector<juce::Rectangle<float>> pr;
    for (auto& s : plates) pr.push_back (s.bounds.toFloat());
    pr.push_back (badgeArea.toFloat());
    const auto stripe = stripeArea.toFloat();

    {
        juce::Image::BitmapData bd (base, juce::Image::BitmapData::writeOnly);
        for (int py = 0; py < H; ++py)
        {
            const float y = (float) py;
            for (int px = 0; px < W; ++px)
            {
                const float x = (float) px;
                tex::RGB col;
                int inside = -1; float edge = 1e9f; bool shadow = false;
                for (int i = 0; i < (int) pr.size(); ++i)
                {
                    const auto& p = pr[(size_t) i];
                    if (p.contains (x, y))
                    {
                        inside = i;
                        edge = std::min ({ x - p.getX(), p.getRight() - x, y - p.getY(), p.getBottom() - y });
                        break;
                    }
                    if (p.translated (3.0f, 4.0f).contains (x, y)) shadow = true;
                }
                if (stripe.contains (x, y))
                {
                    const float grainN = tex::fbm (x / 3.0f, y / 3.0f, 3030, 3);
                    const float crease = tex::smoothstep (0.55f, 0.62f, tex::fbm (x / 18.0f, y / 2.0f, 4040, 2));
                    col = tex::mix (tex::rgb (pal::leatherDark), tex::rgb (pal::leather), grainN);
                    col = tex::mul (col, 1.0f - crease * 0.35f);
                    const float wearSpot = tex::smoothstep (0.6f, 0.75f, tex::fbm (x / 40.0f, y / 10.0f, 5050, 3));
                    col = tex::mix (col, tex::rgb (juce::Colour (0xff6a3a2a)), wearSpot * 0.4f);
                    const float t = (y - stripe.getY()) / stripe.getHeight();
                    col = tex::mul (col, 0.75f + 0.35f * std::sin (t * juce::MathConstants<float>::pi));
                }
                else if (inside >= 0)
                {
                    const float d = tex::fbm (x / 30.0f, y / 30.0f, 211, 3);
                    tex::RGB steel = tex::mix (tex::rgb (juce::Colour (0xff1e1a17)), tex::rgb (juce::Colour (0xff332c27)), d);
                    const float u = std::fmod (x + y + 2000.0f, 16.0f) / 16.0f, v = std::fmod (x - y + 2000.0f, 16.0f) / 16.0f;
                    const float tread = (std::abs (u - 0.5f) < 0.08f && std::abs (v - 0.25f) < 0.18f) || (std::abs (v - 0.5f) < 0.08f && std::abs (u - 0.75f) < 0.18f) ? 1.0f : 0.0f;
                    steel = tex::mix (steel, tex::rgb (juce::Colour (0xff4a4038)), tread * 0.55f);
                    const float edgeRust = (1.0f - juce::jlimit (0.0f, 1.0f, edge / 14.0f)) * tex::smoothstep (0.35f, 0.6f, tex::vnoise (x / 6.0f, y / 6.0f, 17));
                    const float bloom = tex::smoothstep (0.6f, 0.75f, tex::fbm (x / 55.0f, y / 55.0f, 404, 3)) * 0.7f;
                    col = tex::mix (steel, tex::rustAt (x, y, 0.15f, 1337), juce::jlimit (0.0f, 1.0f, edgeRust + bloom));
                    col = tex::mul (col, 0.88f);
                }
                else
                {
                    col = tex::rustAt (x, y, 0.1f, 42);
                    if (shadow) col = tex::mul (col, 0.45f);
                }
                const float streak = tex::fbm (x / 9.0f, y / 240.0f, 909, 3);
                const float drip = tex::smoothstep (0.58f, 0.78f, streak) * (0.25f + 0.5f * y / (float) H);
                col = tex::mul (col, 1.0f - drip * 0.6f);
                const float runoff = tex::smoothstep (0.66f, 0.82f, tex::fbm (x / 5.0f, y / 120.0f, 31337, 2));
                col = tex::mix (col, tex::rgb (pal::rustDeep), runoff * 0.35f);
                const float oil = tex::smoothstep (0.7f, 0.8f, tex::fbm (x / 70.0f, y / 50.0f, 555, 3));
                col = tex::mul (col, 1.0f - oil * 0.45f);
                if (tex::hash (px, py, 5) > 0.988f) col = tex::mul (col, 0.45f);
                const float grime = 0.72f + 0.28f * tex::fbm (x / 260.0f, y / 260.0f, 77, 2);
                const float dx = (x / (float) W - 0.5f) * 2.0f, dy = (y / (float) H - 0.5f) * 2.0f;
                const float vig = 1.0f - 0.45f * std::min (1.0f, (dx * dx + dy * dy) * 0.6f);
                col = tex::mul (col, grime * vig);
                bd.setPixelColour (px, py, juce::Colour::fromFloatRGBA (juce::jlimit (0.0f, 1.0f, col.r), juce::jlimit (0.0f, 1.0f, col.g),
                                                                        juce::jlimit (0.0f, 1.0f, col.b), 1.0f));
            }
        }
    }

    backdrop = juce::Image (juce::Image::RGB, PW, PH, false);
    juce::Graphics g (backdrop);
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
    g.drawImage (base, 0, 0, PW, PH, 0, 0, W, H);
    g.addTransform (juce::AffineTransform::scale (scale));
    juce::Random rnd (1991);

    auto rivet = [&] (float cx, float cy, float rad)
    {
        g.setColour (juce::Colours::black.withAlpha (0.6f)); g.fillEllipse (cx - rad + 1, cy - rad + 1.5f, rad * 2, rad * 2);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff8a7a68), cx - rad * 0.4f, cy - rad * 0.5f, juce::Colour (0xff2a201a), cx + rad, cy + rad, true));
        g.fillEllipse (cx - rad, cy - rad, rad * 2, rad * 2);
        g.setColour (pal::rust.withAlpha (0.55f)); g.drawEllipse (cx - rad, cy - rad, rad * 2, rad * 2, 1.2f);
        g.setColour (pal::rustDeep.withAlpha (0.35f)); g.fillRect (cx - 0.8f, cy + rad, 1.6f, 3.0f + rnd.nextFloat() * 9.0f);
    };
    auto plateFrame = [&] (juce::Rectangle<float> p, bool hot)
    {
        g.setColour (juce::Colours::white.withAlpha (0.07f)); g.drawLine (p.getX() + 2, p.getY() + 1, p.getRight() - 2, p.getY() + 1, 1.0f);
        g.setColour (juce::Colours::black.withAlpha (0.55f)); g.drawRect (p, 1.0f);
        if (hot) { g.setColour (pal::signal.withAlpha (0.45f)); g.drawRect (p.reduced (2.0f), 1.5f); }
        rivet (p.getX() + 6, p.getY() + 6, 3.0f); rivet (p.getRight() - 6, p.getY() + 6, 3.0f);
        rivet (p.getX() + 6, p.getBottom() - 6, 3.0f); rivet (p.getRight() - 6, p.getBottom() - 6, 3.0f);
    };

    for (int i = 0; i < 300; ++i)
    {
        const float x = rnd.nextFloat() * (float) W, y = rnd.nextFloat() * (float) H;
        const float len = 6.0f + rnd.nextFloat() * 70.0f, ang = rnd.nextFloat() * juce::MathConstants<float>::pi;
        g.setColour ((rnd.nextBool() ? juce::Colour (0xffd0c0a8) : juce::Colours::black).withAlpha (0.04f + rnd.nextFloat() * 0.1f));
        g.drawLine (x, y, x + std::cos (ang) * len, y + std::sin (ang) * len, 0.5f + rnd.nextFloat() * 0.6f);
    }

    int seed = 10;
    for (auto& s : plates)
    {
        const auto p = s.bounds.toFloat();
        plateFrame (p, s.hot);
        auto title = tex::distressedText (s.title, s.bounds.getWidth() - 40, 18, tex::stencil (15.0f), s.hot ? pal::signalHot : pal::paint,
                                          juce::Justification::centredLeft, scale, 0.2f, seed++, 0.12f);
        g.setOpacity (1.0f);
        g.drawImage (title, juce::Rectangle<float> (p.getX() + 14, p.getY() + 3, (float) s.bounds.getWidth() - 40, 18.0f));
    }
    // screen wells
    for (auto rr : { curve.getBounds(), status.getBounds() })
    {
        g.setColour (juce::Colours::black.withAlpha (0.7f)); g.fillRoundedRectangle (rr.toFloat().expanded (4.0f), 6.0f);
    }
    {   // stamped hint under AMOUNT
        auto h1 = tex::distressedText ("100% = AS DECIDED.", 150, 14, tex::stencil (12.5f), pal::signalHot.withAlpha (0.9f), juce::Justification::centred, scale, 0.18f, 61, 0.14f);
        auto h2 = tex::distressedText ("TURN IT DOWN TO TASTE.", 150, 14, tex::plainBold (9.0f), pal::paint.withAlpha (0.75f), juce::Justification::centred, scale, 0.18f, 62, 0.14f);
        g.setOpacity (1.0f);
        g.drawImage (h1, juce::Rectangle<float> (26.0f, 644.0f, 150.0f, 14.0f));
        g.drawImage (h2, juce::Rectangle<float> (26.0f, 660.0f, 150.0f, 14.0f));
        auto h3 = tex::distressedText ("ONLY CUTS WHILE IT RINGS.", 260, 14, tex::plainBold (9.5f), pal::paint.withAlpha (0.7f), juce::Justification::centred, scale, 0.18f, 63, 0.16f);
        g.drawImage (h3, juce::Rectangle<float> (391.0f, 612.0f, 260.0f, 14.0f));
        auto h4 = tex::distressedText ("CLICK A RED TRIANGLE ON THE CURVE", 260, 14, tex::plainBold (9.0f), pal::paintDim.withAlpha (0.8f), juce::Justification::centred, scale, 0.18f, 64, 0.1f);
        auto h5 = tex::distressedText ("TO MUTE ONE.", 260, 14, tex::plainBold (9.0f), pal::paintDim.withAlpha (0.8f), juce::Justification::centred, scale, 0.18f, 65, 0.1f);
        g.drawImage (h4, juce::Rectangle<float> (391.0f, 636.0f, 260.0f, 14.0f));
        g.drawImage (h5, juce::Rectangle<float> (391.0f, 652.0f, 260.0f, 14.0f));
    }

    // leather strap: stitching + chrome studs
    {
        g.setColour (juce::Colours::black.withAlpha (0.75f));
        g.drawLine (0, stripe.getY(), (float) W, stripe.getY(), 1.5f);
        g.drawLine (0, stripe.getBottom(), (float) W, stripe.getBottom(), 1.5f);
        juce::Path stitch;
        for (float yy : { stripe.getY() + 3.0f, stripe.getBottom() - 3.0f }) { stitch.startNewSubPath (0, yy); stitch.lineTo ((float) W, yy); }
        juce::Path dashed; const float dl[] = { 5.0f, 3.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (dashed, stitch, dl, 2);
        g.setColour (pal::paint.withAlpha (0.45f)); g.fillPath (dashed);
        for (float x = 22.0f; x < (float) W; x += 34.0f)
        {
            const float cy = stripe.getCentreY(), sr = 4.2f;
            g.setColour (juce::Colours::black.withAlpha (0.6f)); g.fillEllipse (x - sr + 1.0f, cy - sr + 1.5f, sr * 2, sr * 2);
            g.setGradientFill (juce::ColourGradient (juce::Colour (0xffe8e0d4), x - sr * 0.4f, cy - sr * 0.5f, juce::Colour (0xff3a3430), x + sr, cy + sr, true));
            g.fillEllipse (x - sr, cy - sr, sr * 2, sr * 2);
            if (rnd.nextFloat() < 0.5f) { g.setColour (pal::rust.withAlpha (0.5f)); g.drawEllipse (x - sr, cy - sr, sr * 2, sr * 2, 1.0f); }
            g.setColour (juce::Colours::white.withAlpha (0.7f)); g.fillEllipse (x - sr * 0.45f, cy - sr * 0.55f, sr * 0.6f, sr * 0.45f);
        }
    }

    // CASTLE DANGER MUSIC COMPANY badge with the castle crest
    const auto badge = badgeArea.toFloat();
    plateFrame (badge, false);
    g.setColour (pal::paint.withAlpha (0.22f)); g.drawRect (badge.reduced (5.0f), 1.0f);
    drawCastleCrest (g, crestArea.toFloat());
    const float tx = (float) crestArea.getRight() + 8.0f, tw = badge.getRight() - tx - 10.0f;
    auto l1 = tex::distressedText ("CASTLE DANGER", (int) tw, 28, tex::stencil (25.0f), juce::Colour (0xffeadfc4), juce::Justification::centredLeft, scale, 0.24f, 4242, 0.08f);
    auto l2 = tex::distressedText ("MUSIC COMPANY", (int) tw, 20, tex::stencil (17.0f), pal::rustLight, juce::Justification::centredLeft, scale, 0.24f, 4343, 0.2f);
    auto l3 = tex::distressedText ("WE MAKE MUSIC", (int) tw, 14, tex::plainBold (9.0f), pal::paintDim, juce::Justification::centredLeft, scale, 0.15f, 4444, 0.3f);
    g.setColour (juce::Colours::black);
    g.setOpacity (0.7f); g.drawImage (l1, juce::Rectangle<float> (tx + 1.5f, badge.getY() + 13.0f, tw, 28.0f));
    g.setOpacity (1.0f); g.drawImage (l1, juce::Rectangle<float> (tx, badge.getY() + 11.0f, tw, 28.0f));
    g.setOpacity (0.7f); g.drawImage (l2, juce::Rectangle<float> (tx + 1.5f, badge.getY() + 40.0f, tw, 20.0f));
    g.setOpacity (1.0f); g.drawImage (l2, juce::Rectangle<float> (tx, badge.getY() + 38.0f, tw, 20.0f));
    g.drawImage (l3, juce::Rectangle<float> (tx + 1.0f, badge.getY() + 60.0f, tw, 14.0f));

    auto sub = tex::distressedText ("TRACK ASSISTANT TO THE MASTER  //  V" + juce::String (JucePlugin_VersionString),
                                    400, 14, tex::plainBold (10.5f), pal::paintDim, juce::Justification::centredLeft, scale, 0.15f, 99, 0.14f);
    g.setOpacity (1.0f);
    g.drawImage (sub, juce::Rectangle<float> (116.0f, 86.0f, 400.0f, 14.0f));

    auto pl = tex::distressedText ("ROLE", 120, 16, tex::stencil (14.0f), pal::paint, juce::Justification::centredLeft, scale, 0.2f, 7, 0.15f);
    g.drawImage (pl, juce::Rectangle<float> ((float) roleArea.getX() + 2, (float) roleArea.getY() + 2, 120.0f, 16.0f));
    auto rh = tex::distressedText ("WHAT THIS TRACK DOES IN THE SONG", 200, 12, tex::plainBold (8.5f), pal::paintDim, juce::Justification::centredLeft, scale, 0.15f, 8, 0.12f);
    g.drawImage (rh, juce::Rectangle<float> ((float) roleArea.getX() + 2, (float) roleArea.getY() + 58, 200.0f, 12.0f));

    // the logo: THE SERVANT in rusty red, over a tarnished serving-tray rule
    {
        const int lw = logoArea.getWidth(), lh = logoArea.getHeight() + 10;
        logoImage = juce::Image (juce::Image::ARGB, (int) ((float) lw * scale), (int) ((float) lh * scale), true);
        juce::Graphics lg (logoImage);
        lg.addTransform (juce::AffineTransform::scale (scale));
        {
            const juce::Rectangle<float> tray (4.0f, 62.0f, (float) lw - 40.0f, 7.0f);
            lg.setColour (juce::Colours::black.withAlpha (0.6f)); lg.fillRoundedRectangle (tray.translated (1.5f, 2.0f), 3.5f);
            lg.setGradientFill (juce::ColourGradient (juce::Colour (0xffb8a888), 0, tray.getY(), juce::Colour (0xff3a3028), 0, tray.getBottom(), false));
            lg.fillRoundedRectangle (tray, 3.5f);
            juce::Random tr (5);
            for (int i = 0; i < 60; ++i)
            {
                lg.setColour ((i % 2 ? pal::rust : juce::Colour (0xff1a1410)).withAlpha (0.4f + 0.3f * tr.nextFloat()));
                lg.fillEllipse (tray.getX() + tr.nextFloat() * tray.getWidth(), tray.getY() + tr.nextFloat() * 4.0f, 2.0f + tr.nextFloat() * 6.0f, 2.0f + tr.nextFloat() * 2.5f);
            }
            lg.setColour (juce::Colours::black.withAlpha (0.7f)); lg.drawRoundedRectangle (tray, 3.5f, 1.0f);
        }
        auto f = tex::stencil (60.0f).withExtraKerningFactor (0.05f);
        {
            juce::GlyphArrangement all; all.addLineOfText (f, "THE SERVANT", 0, 0);
            const float wAll = all.getBoundingBox (0, -1, true).getWidth();
            if (wAll > (float) lw - 6.0f) f = f.withHeight (60.0f * ((float) lw - 6.0f) / wAll);
        }
        juce::Image word (juce::Image::ARGB, (int) ((float) lw * scale), (int) ((float) lh * scale), true);
        {
            juce::Graphics wg (word);
            wg.addTransform (juce::AffineTransform::scale (scale));
            wg.setFont (f);
            wg.setColour (pal::signal);
            wg.drawText ("THE SERVANT", 0, 0, lw, 58, juce::Justification::centredLeft, false);
        }
        tex::distress (word, scale, 0.3f, 666);
        lg.drawImage (word, juce::Rectangle<float> (0.0f, 0.0f, (float) lw, (float) lh));
    }
}

void ServantEditor::paint (juce::Graphics& g)
{
    if (backdrop.isValid()) g.drawImage (backdrop, getLocalBounds().toFloat());
    else g.fillAll (pal::ironDark);
    if (logoImage.isValid())
    {
        const auto la = logoArea.toFloat().withHeight ((float) logoArea.getHeight() + 10.0f);
        g.setColour (juce::Colours::black);
        g.setOpacity (0.65f); g.drawImage (logoImage, la.translated (2.5f, 3.5f));
        g.setOpacity (1.0f); g.drawImage (logoImage, la);
    }
}
