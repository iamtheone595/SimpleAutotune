// Simple Autotune - real-time pitch correction plugin (AU / VST3)
//
// Pipeline:
//   1. Detect the pitch of the incoming (mono-summed) signal with the YIN algorithm
//      (run on a ~22 kHz decimated copy to keep CPU low).
//   2. Snap the detected note to the nearest enabled note (the 12 note toggles = your scale).
//   3. Smooth the correction over "Retune Speed" milliseconds.
//   4. Shift the audio with a single-head delay-line shifter that re-centres itself by
//      jumping a whole number of pitch periods (phase-aligned crossfade, no comb filtering).
//
// Presets store the "style" (retune speed + strength). Key / scale stay yours.
// Best on a dry, solo, monophonic vocal. Not meant for chords or full mixes.

#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>

//==============================================================================
// Look
//==============================================================================
namespace Palette
{
    const juce::Colour bg         (0xff1d1b19);
    const juce::Colour header     (0xff161513);
    const juce::Colour panel      (0xff262421);
    const juce::Colour edgeDark   (0xff0e0d0c);
    const juce::Colour edgeLite   (0xff38352f);
    const juce::Colour text       (0xffe8e2d4);
    const juce::Colour textDim    (0xff958f82);
    const juce::Colour textFaint  (0xff5a574f);
    const juce::Colour accent     (0xffd9962b);
    const juce::Colour keyOn      (0xffeee8da);
    const juce::Colour keyOff     (0xff3f3d38);
    const juce::Colour keyLabel   (0xff3b3832);
    const juce::Colour blackOn    (0xff1a1917);
    const juce::Colour blackOff   (0xff2a2825);
    const juce::Colour well       (0xff141312);
}

static juce::Font uiFont (float height, bool bold = false)
{
    return juce::Font (juce::FontOptions ("Helvetica Neue", height, bold ? juce::Font::bold : juce::Font::plain));
}

static void drawPanel (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& title)
{
    auto f = r.toFloat();
    g.setColour (Palette::panel);
    g.fillRoundedRectangle (f, 5.0f);
    g.setColour (Palette::edgeDark);
    g.drawRoundedRectangle (f.reduced (0.5f), 5.0f, 1.0f);
    g.setColour (Palette::edgeLite.withAlpha (0.7f));
    g.drawLine (f.getX() + 6.0f, f.getY() + 1.5f, f.getRight() - 6.0f, f.getY() + 1.5f, 1.0f);
    g.setColour (Palette::textDim);
    g.setFont (uiFont (11.0f, true));
    g.drawText (title, r.getX() + 12, r.getY() + 7, r.getWidth() - 24, 14, juce::Justification::centredLeft);
}

//==============================================================================
// Presets (style only: retune speed + strength)
//==============================================================================
struct Preset
{
    juce::String name;
    float speed;     // ms, 0..200
    float strength;  // 0..1
};

static const std::vector<Preset>& factoryPresets()
{
    static const std::vector<Preset> p {
        { "Default",       30.0f, 1.00f },
        { "Gentle nudge", 160.0f, 0.35f },
        { "Subtle polish",110.0f, 0.50f },
        { "Natural",       80.0f, 0.75f },
        { "Modern pop",    25.0f, 0.95f },
        { "Tight",         12.0f, 1.00f },
        { "Hard tune",      0.0f, 1.00f },
    };
    return p;
}

static juce::File presetDir()
{
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
   #if JUCE_MAC
    base = base.getChildFile ("Application Support");
   #endif
    auto dir = base.getChildFile ("SimpleAutotune").getChildFile ("Presets");
    dir.createDirectory();
    return dir;
}

//==============================================================================
// Processor
//==============================================================================
class AutotuneProcessor : public juce::AudioProcessor
{
public:
    AutotuneProcessor()
        : AudioProcessor (BusesProperties()
                              .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                              .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
          apvts (*this, nullptr, "PARAMS", createLayout())
    {
        for (int i = 0; i < 12; ++i)
            noteParams[(size_t) i] = apvts.getRawParameterValue ("note" + juce::String (i));
        speedParam    = apvts.getRawParameterValue ("speed");
        strengthParam = apvts.getRawParameterValue ("strength");
    }

    // shared with the editor
    juce::AudioProcessorValueTreeState apvts;
    std::atomic<float> meterMidi { 0.0f };       // detected pitch as a MIDI note number, 0 = none
    std::atomic<float> meterCorrSemis { 0.0f };  // correction currently applied, in semitones

    void applyPreset (float speed, float strength)
    {
        setParam ("speed", speed);
        setParam ("strength", strength);
    }

    void rememberProgram (int index) { currentProgram = index; }

    //==============================================================================
    const juce::String getName() const override { return "Simple Autotune"; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    // Host-visible presets (AU preset menu etc.)
    int getNumPrograms() override { return (int) factoryPresets().size(); }
    int getCurrentProgram() override { return currentProgram; }
    void setCurrentProgram (int index) override
    {
        // Some hosts call setCurrentProgram(0) right after restoring a saved session.
        // Ignoring a request for the program we're already "on" stops that from
        // overwriting the knob settings that were just loaded.
        if (index == currentProgram || index < 0 || index >= getNumPrograms())
            return;
        currentProgram = index;
        const auto& p = factoryPresets()[(size_t) index];
        applyPreset (p.speed, p.strength);
    }
    const juce::String getProgramName (int index) override
    {
        if (index >= 0 && index < getNumPrograms())
            return factoryPresets()[(size_t) index].name;
        return {};
    }
    void changeProgramName (int, const juce::String&) override {}

    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override; // defined below the editor

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        const auto out = layouts.getMainOutputChannelSet();
        return (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo())
               && out == layouts.getMainInputChannelSet();
    }

    void getStateInformation (juce::MemoryBlock& dest) override
    {
        if (auto xml = apvts.copyState().createXml())
            copyXmlToBinary (*xml, dest);
    }

    void setStateInformation (const void* data, int size) override
    {
        if (auto xml = getXmlFromBinary (data, size))
            if (xml->hasTagName (apvts.state.getType()))
                apvts.replaceState (juce::ValueTree::fromXml (*xml));
    }

    //==============================================================================
    void prepareToPlay (double sampleRate, int) override
    {
        sr = sampleRate;

        // Pitch detector works on a decimated copy (~22 kHz) to save CPU.
        decim   = std::max (1, (int) std::lround (sr / 22000.0));
        srA     = sr / (double) decim;
        tauMin  = std::max (2, (int) (srA / 1000.0)); // highest pitch ~1000 Hz
        tauMax  = (int) (srA / 70.0);                 // lowest pitch  ~70 Hz
        yinW    = (int) (srA * 0.025);
        anaSize = yinW + tauMax + 1;
        hop     = std::max (8, (int) (srA * 0.0116)); // pitch re-estimated ~86x/sec

        ring.assign  ((size_t) anaSize, 0.0f);
        frame.assign ((size_t) anaSize, 0.0f);
        diff.assign  ((size_t) tauMax + 1, 0.0f);
        cmnd.assign  ((size_t) tauMax + 1, 0.0f);

        int size = 1;
        while (size < (int) (sr * 0.1))
            size <<= 1;
        delayMask = size - 1;
        for (auto& d : delay)
            d.assign ((size_t) size, 0.0f);

        dMid     = (float) (sr * 0.020);  // centre delay = reported latency
        halfWin  = (float) (sr * 0.010);  // re-centre when the delay drifts this far
        xfLen    = std::max (32, (int) (sr * 0.004));

        reset();
        setLatencySamples ((int) dMid);
    }

    // Called by hosts when playback starts/stops: wipe anything left over so old audio
    // can't leak into the start of the next pass.
    void reset() override
    {
        std::fill (ring.begin(), ring.end(), 0.0f);
        std::fill (frame.begin(), frame.end(), 0.0f);
        for (auto& d : delay)
            std::fill (d.begin(), d.end(), 0.0f);

        ringPos = 0;
        hopCount = 0;
        decAcc = 0.0f;
        decCount = 0;
        writePos = 0;

        delayA = dMid;
        delayB = dMid;
        xf = 1.0f;
        period = (float) (sr / 200.0);

        currentCorr = 0.0f;
        targetCorr  = 0.0f;
        histCount = 0;
        histPos = 0;
        unvoicedCount = 0;
        meterMidi.store (0.0f);
        meterCorrSemis.store (0.0f);
    }

    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals noDenormals;

        const int numCh = std::min (buffer.getNumChannels(), 2);
        const int n = buffer.getNumSamples();
        if (numCh == 0 || ring.empty())
            return;

        updateAllowed();
        const float speedMs  = speedParam->load();
        const float strength = strengthParam->load();
        const float alpha = speedMs < 1.0f ? 1.0f
                                           : 1.0f - std::exp (-1.0f / (speedMs * 0.001f * (float) sr));

        float* ptr[2] = { nullptr, nullptr };
        for (int c = 0; c < numCh; ++c)
            ptr[c] = buffer.getWritePointer (c);

        for (int i = 0; i < n; ++i)
        {
            float in[2] = { 0.0f, 0.0f };
            float mono = 0.0f;
            for (int c = 0; c < numCh; ++c)
            {
                in[c] = ptr[c][i];
                mono += in[c];
            }
            mono /= (float) numCh;

            // feed the (decimated) pitch detector
            decAcc += mono;
            if (++decCount >= decim)
            {
                ring[(size_t) ringPos] = decAcc / (float) decim;
                ringPos = (ringPos + 1) % anaSize;
                decAcc = 0.0f;
                decCount = 0;

                if (++hopCount >= hop)
                {
                    hopCount = 0;
                    analyse (strength);
                }
            }

            // smooth the correction, convert to a delay drift (samples per sample)
            currentCorr += alpha * (targetCorr - currentCorr);
            const float drift = 1.0f - std::pow (2.0f, currentCorr / 12.0f);
            delayA += drift;
            delayB += drift;

            // re-centre by jumping a whole number of pitch periods, with a short crossfade
            if (xf >= 1.0f && (delayA < dMid - halfWin || delayA > dMid + halfWin))
            {
                float jumps = std::round ((dMid - delayA) / period);
                if (jumps == 0.0f)
                    jumps = (delayA < dMid) ? 1.0f : -1.0f;
                delayB = delayA;           // old head keeps reading where it was
                delayA += jumps * period;  // new head lands a whole number of periods away
                xf = 0.0f;
            }
            const float maxDelay = (float) delayMask - 4.0f;
            delayA = juce::jlimit (2.0f, maxDelay, delayA);
            delayB = juce::jlimit (2.0f, maxDelay, delayB);

            float gNew = 1.0f, gOld = 0.0f;
            if (xf < 1.0f)
            {
                xf = std::min (1.0f, xf + 1.0f / (float) xfLen);
                gNew = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi * xf);
                gOld = 1.0f - gNew;
            }

            for (int c = 0; c < numCh; ++c)
            {
                auto& b = delay[(size_t) c];
                b[(size_t) writePos] = in[c];
                float o = gNew * readDelay (b, delayA);
                if (gOld > 0.0f)
                    o += gOld * readDelay (b, delayB);
                ptr[c][i] = o;
            }
            writePos = (writePos + 1) & delayMask;
        }

        meterCorrSemis.store (currentCorr, std::memory_order_relaxed);
    }

private:
    //==============================================================================
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
    {
        using namespace juce;
        static const char* noteNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        static const bool majorOn[12]    = { true, false, true, false, true, true, false, true, false, true, false, true };

        AudioProcessorValueTreeState::ParameterLayout layout;
        for (int i = 0; i < 12; ++i)
            layout.add (std::make_unique<AudioParameterBool> (
                ParameterID { "note" + String (i), 1 }, noteNames[i], majorOn[i]));
        layout.add (std::make_unique<AudioParameterFloat> (
            ParameterID { "speed", 1 }, "Retune Speed (ms)",
            NormalisableRange<float> (0.0f, 200.0f, 1.0f), 30.0f));
        layout.add (std::make_unique<AudioParameterFloat> (
            ParameterID { "strength", 1 }, "Strength",
            NormalisableRange<float> (0.0f, 1.0f, 0.01f), 1.0f));
        return layout;
    }

    void setParam (const char* id, float value)
    {
        if (auto* p = apvts.getParameter (id))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->convertTo0to1 (value));
            p->endChangeGesture();
        }
    }

    void updateAllowed()
    {
        for (int i = 0; i < 12; ++i)
            allowed[i] = noteParams[(size_t) i]->load() > 0.5f;
    }

    float snap (float midi) const
    {
        float best = midi, bestDist = 1.0e9f;
        const int c = (int) std::round (midi);
        for (int m = c - 7; m <= c + 7; ++m)
        {
            const int pc = ((m % 12) + 12) % 12;
            if (! allowed[pc]) continue;
            const float d = std::abs ((float) m - midi);
            if (d < bestDist) { bestDist = d; best = (float) m; }
        }
        return best; // no notes enabled = no correction
    }

    float medianMidi (float m)
    {
        hist[(size_t) histPos] = m;
        histPos = (histPos + 1) % 3;
        histCount = std::min (histCount + 1, 3);
        if (histCount < 3) return m;
        auto a = hist;
        std::sort (a.begin(), a.end());
        return a[1];
    }

    void analyse (float strength)
    {
        for (int k = 0; k < anaSize; ++k)
            frame[(size_t) k] = ring[(size_t) ((ringPos + k) % anaSize)];

        const float f0 = yin();
        if (f0 > 0.0f)
        {
            unvoicedCount = 0;
            period = std::max (2.0f, (float) (sr / f0));

            float midi = 69.0f + 12.0f * std::log2 (f0 / 440.0f);
            midi = medianMidi (midi);
            meterMidi.store (midi, std::memory_order_relaxed);
            targetCorr = (snap (midi) - midi) * strength;
        }
        else if (++unvoicedCount >= 3) // only let go after ~35 ms of no clear pitch
        {
            histCount = 0;
            targetCorr = 0.0f;
            meterMidi.store (0.0f, std::memory_order_relaxed);
        }
    }

    // YIN pitch detector. Returns frequency in Hz, or 0 if unvoiced/silent.
    float yin()
    {
        double energy = 0.0;
        for (int j = anaSize - yinW; j < anaSize; ++j)
            energy += (double) frame[(size_t) j] * frame[(size_t) j];
        if (std::sqrt (energy / yinW) < 0.003) // silence gate (~ -50 dB)
            return 0.0f;

        for (int tau = 1; tau <= tauMax; ++tau)
        {
            double s = 0.0;
            for (int j = 0; j < yinW; ++j)
            {
                const double d = (double) frame[(size_t) j] - (double) frame[(size_t) (j + tau)];
                s += d * d;
            }
            diff[(size_t) tau] = (float) s;
        }

        float running = 0.0f;
        cmnd[0] = 1.0f;
        for (int tau = 1; tau <= tauMax; ++tau)
        {
            running += diff[(size_t) tau];
            cmnd[(size_t) tau] = diff[(size_t) tau] * (float) tau / std::max (running, 1.0e-9f);
        }

        int tauEst = -1;
        for (int tau = tauMin; tau < tauMax; ++tau)
        {
            if (cmnd[(size_t) tau] < 0.15f)
            {
                while (tau + 1 < tauMax && cmnd[(size_t) (tau + 1)] < cmnd[(size_t) tau])
                    ++tau;
                tauEst = tau;
                break;
            }
        }
        if (tauEst < 0)
            return 0.0f;

        float better = (float) tauEst;
        if (tauEst > 1)
        {
            const float s0 = cmnd[(size_t) (tauEst - 1)];
            const float s1 = cmnd[(size_t) tauEst];
            const float s2 = cmnd[(size_t) (tauEst + 1)];
            const float denom = 2.0f * (s0 - 2.0f * s1 + s2);
            if (std::abs (denom) > 1.0e-9f)
                better += (s0 - s2) / denom;
        }
        return (float) (srA / better);
    }

    float readDelay (const std::vector<float>& b, float d) const
    {
        const float pos = (float) writePos - d;
        const float fl = std::floor (pos);
        const int i0 = ((int) fl) & delayMask;
        const int i1 = (i0 + 1) & delayMask;
        const float frac = pos - fl;
        return b[(size_t) i0] + frac * (b[(size_t) i1] - b[(size_t) i0]);
    }

    //==============================================================================
    std::array<std::atomic<float>*, 12> noteParams {};
    std::atomic<float>* speedParam = nullptr;
    std::atomic<float>* strengthParam = nullptr;
    int currentProgram = 0;

    double sr = 44100.0, srA = 22050.0;

    // pitch detection
    int decim = 1, decCount = 0;
    float decAcc = 0.0f;
    int tauMin = 0, tauMax = 0, yinW = 0, anaSize = 1, hop = 256, hopCount = 0, ringPos = 0;
    std::vector<float> ring, frame, diff, cmnd;
    std::array<float, 3> hist { 0.0f, 0.0f, 0.0f };
    int histCount = 0, histPos = 0, unvoicedCount = 0;
    bool allowed[12] = {};

    // correction smoothing
    float currentCorr = 0.0f, targetCorr = 0.0f;

    // pitch shifter
    int delayMask = 0, writePos = 0, xfLen = 176;
    std::array<std::vector<float>, 2> delay;
    float delayA = 0.0f, delayB = 0.0f, xf = 1.0f, dMid = 0.0f, halfWin = 0.0f, period = 200.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutotuneProcessor)
};

//==============================================================================
// UI components
//==============================================================================
class DeskLookAndFeel : public juce::LookAndFeel_V4
{
public:
    DeskLookAndFeel()
    {
        setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff1c1b19));
        setColour (juce::PopupMenu::textColourId, Palette::text);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, Palette::accent.darker (0.55f));
        setColour (juce::PopupMenu::highlightedTextColourId, Palette::text);
        setColour (juce::ComboBox::textColourId, Palette::text);
    }

    juce::Font getComboBoxFont (juce::ComboBox&) override { return uiFont (12.0f); }
    juce::Font getPopupMenuFont() override { return uiFont (12.0f); }
    juce::Font getTextButtonFont (juce::TextButton&, int) override { return uiFont (11.0f, true); }

    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds (8, 1, box.getWidth() - 24, box.getHeight() - 2);
        label.setFont (getComboBoxFont (box));
    }

    void drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox&) override
    {
        const auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) w, (float) h);
        g.setColour (Palette::well);
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (Palette::edgeLite);
        g.drawRoundedRectangle (r.reduced (0.5f), 3.0f, 1.0f);

        const float cx = (float) w - 11.0f, cy = (float) h * 0.5f;
        juce::Path p;
        p.addTriangle (cx - 3.5f, cy - 1.5f, cx + 3.5f, cy - 1.5f, cx, cy + 2.5f);
        g.setColour (Palette::textDim);
        g.fillPath (p);
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&,
                               bool highlighted, bool down) override
    {
        const auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (down ? Palette::well.darker (0.3f) : (highlighted ? juce::Colour (0xff1b1a18) : Palette::well));
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (Palette::edgeLite);
        g.drawRoundedRectangle (r, 3.0f, 1.0f);
    }

    void drawButtonText (juce::Graphics& g, juce::TextButton& b, bool highlighted, bool) override
    {
        g.setColour (highlighted ? Palette::text : Palette::textDim);
        g.setFont (getTextButtonFont (b, b.getHeight()));
        g.drawText (b.getButtonText(), b.getLocalBounds(), juce::Justification::centred);
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                           float a0, float a1, juce::Slider& s) override
    {
        auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h);
        auto textArea = bounds.removeFromBottom (18.0f);
        auto area = bounds.reduced (6.0f);
        const float dia = std::min (area.getWidth(), area.getHeight());
        const auto c = area.getCentre();
        const float rOuter = dia * 0.5f;
        const float rInner = rOuter - 4.0f;
        const float rKnob  = rOuter - 9.0f;

        auto pt = [&] (float angle, float r)
        {
            return juce::Point<float> (c.x + r * std::sin (angle), c.y - r * std::cos (angle));
        };

        // scale ticks, lit up to the current value
        const int nTicks = 11;
        for (int i = 0; i < nTicks; ++i)
        {
            const float t = (float) i / (float) (nTicks - 1);
            const float a = a0 + t * (a1 - a0);
            g.setColour (t <= pos + 0.001f ? Palette::accent : Palette::textFaint);
            g.drawLine (juce::Line<float> (pt (a, rInner), pt (a, rOuter)), 1.5f);
        }

        // knob: soft shadow, matte body, slightly lighter cap
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillEllipse (c.x - rKnob - 1.0f, c.y - rKnob + 2.0f, rKnob * 2.0f + 2.0f, rKnob * 2.0f + 2.0f);
        g.setColour (Palette::edgeDark);
        g.fillEllipse (c.x - rKnob - 1.2f, c.y - rKnob - 1.2f, rKnob * 2.0f + 2.4f, rKnob * 2.0f + 2.4f);
        g.setColour (juce::Colour (0xff2e2c29));
        g.fillEllipse (c.x - rKnob, c.y - rKnob, rKnob * 2.0f, rKnob * 2.0f);
        const float rCap = rKnob * 0.74f;
        g.setColour (juce::Colour (0xff383631));
        g.fillEllipse (c.x - rCap, c.y - rCap, rCap * 2.0f, rCap * 2.0f);
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.drawEllipse (c.x - rCap + 0.5f, c.y - rCap + 0.5f, rCap * 2.0f - 1.0f, rCap * 2.0f - 1.0f, 1.0f);

        // pointer with rounded ends
        const float ang = a0 + pos * (a1 - a0);
        juce::Path ptr;
        ptr.startNewSubPath (pt (ang, rKnob * 0.30f));
        ptr.lineTo (pt (ang, rKnob * 0.90f));
        g.setColour (Palette::text);
        g.strokePath (ptr, juce::PathStrokeType (2.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // value readout
        const double v = s.getValue();
        const juce::String txt = (s.getName() == "speed")
                                     ? juce::String ((int) std::round (v)) + " ms"
                                     : juce::String ((int) std::round (v * 100.0)) + " %";
        g.setColour (Palette::text);
        g.setFont (uiFont (12.0f));
        g.drawText (txt, textArea.toNearestInt(), juce::Justification::centred);
    }
};

//------------------------------------------------------------------------------
class PianoKeyboard : public juce::Component
{
public:
    explicit PianoKeyboard (juce::AudioProcessorValueTreeState& s) : apvts (s)
    {
        for (int i = 0; i < 12; ++i)
            notes[(size_t) i] = apvts.getRawParameterValue ("note" + juce::String (i));
    }

    int detectedPc = -1;

    void paint (juce::Graphics& g) override
    {
        const float w = (float) getWidth() / 7.0f;

        for (int k = 0; k < 7; ++k)
        {
            const auto key = whiteRect (k);
            const bool on = isOn (whitePc[k]);
            auto fill = on ? Palette::keyOn : Palette::keyOff;
            if (whitePc[k] == detectedPc)
                fill = fill.interpolatedWith (Palette::accent, 0.5f);
            g.setColour (fill);
            g.fillPath (keyShape (key, 3.5f));

            g.setColour (on ? Palette::keyLabel : Palette::textFaint);
            g.setFont (uiFont (10.5f, true));
            g.drawText (whiteName[k], key.withTrimmedTop (key.getHeight() - 26.0f).withHeight (13.0f).toNearestInt(),
                        juce::Justification::centred);
            if (on)
            {
                g.setColour (Palette::accent);
                g.fillEllipse (key.getCentreX() - 2.5f, key.getBottom() - 9.0f, 5.0f, 5.0f);
            }
        }

        for (int i = 0; i < 5; ++i)
        {
            const auto key = blackRect (i, w);
            const bool on = isOn (blackPc[i]);
            auto fill = on ? Palette::blackOn : Palette::blackOff;
            if (blackPc[i] == detectedPc)
                fill = fill.interpolatedWith (Palette::accent, 0.55f);
            g.setColour (fill);
            g.fillPath (keyShape (key, 2.5f));
            if (on)
            {
                g.setColour (Palette::accent);
                g.fillEllipse (key.getCentreX() - 2.0f, key.getBottom() - 8.0f, 4.0f, 4.0f);
            }
        }
    }

    // click a key to toggle it; drag across keys to switch several on/off in one stroke
    void mouseDown (const juce::MouseEvent& e) override
    {
        const int pc = keyAt (e.position);
        paintValue = notes[(size_t) pc]->load() > 0.5f ? 0.0f : 1.0f;
        lastPc = -1;
        setNote (pc, paintValue);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        setNote (keyAt (e.position), paintValue);
    }

private:
    void setNote (int pc, float v)
    {
        if (pc < 0 || pc == lastPc)
            return;
        lastPc = pc;
        if ((notes[(size_t) pc]->load() > 0.5f) == (v > 0.5f))
            return;
        if (auto* p = apvts.getParameter ("note" + juce::String (pc)))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (v);
            p->endChangeGesture();
        }
    }

    static juce::Path keyShape (juce::Rectangle<float> r, float radius)
    {
        juce::Path p;
        p.addRoundedRectangle (r.getX(), r.getY(), r.getWidth(), r.getHeight(), radius, radius,
                               false, false, true, true);
        return p;
    }

    bool isOn (int pc) const { return notes[(size_t) pc]->load() > 0.5f; }

    juce::Rectangle<float> whiteRect (int k) const
    {
        const float w = (float) getWidth() / 7.0f;
        return juce::Rectangle<float> ((float) k * w, 0.0f, w, (float) getHeight()).reduced (0.8f);
    }

    juce::Rectangle<float> blackRect (int i, float w) const
    {
        const float bw = w * 0.60f, bh = (float) getHeight() * 0.62f;
        return juce::Rectangle<float> (blackPos[i] * w - bw * 0.5f, 0.0f, bw, bh);
    }

    int keyAt (juce::Point<float> p) const
    {
        const float w = (float) getWidth() / 7.0f;
        for (int i = 0; i < 5; ++i)
            if (blackRect (i, w).contains (p))
                return blackPc[i];
        return whitePc[juce::jlimit (0, 6, (int) (p.x / w))];
    }

    static constexpr int whitePc[7]     = { 0, 2, 4, 5, 7, 9, 11 };
    static constexpr int blackPc[5]     = { 1, 3, 6, 8, 10 };
    static constexpr float blackPos[5]  = { 1.0f, 2.0f, 4.0f, 5.0f, 6.0f };
    const char* whiteName[7]            = { "C", "D", "E", "F", "G", "A", "B" };

    juce::AudioProcessorValueTreeState& apvts;
    std::array<std::atomic<float>*, 12> notes {};
    float paintValue = 1.0f;
    int lastPc = -1;
};

//------------------------------------------------------------------------------
class PitchMeter : public juce::Component
{
public:
    void update (float midi, float corrSemis)
    {
        voiced = midi > 0.0f;
        if (voiced)
        {
            const float rounded = std::round (midi);
            const float dev = (midi - rounded) * 100.0f;
            smoothDev = wasVoiced ? smoothDev + 0.35f * (dev - smoothDev) : dev;
            note = (int) rounded;
        }
        wasVoiced = voiced;
        smoothCorr += 0.25f * (corrSemis * 100.0f - smoothCorr);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        // ---- note readout
        g.setColour (voiced ? Palette::text : Palette::textFaint);
        g.setFont (uiFont (30.0f, true));
        g.drawText (voiced ? noteName (note) : juce::String ("--"), 0, 0, 80, 40, juce::Justification::centred);
        g.setFont (uiFont (10.0f));
        g.setColour (Palette::textDim);
        g.drawText (voiced ? juce::String::formatted ("%+d ct", (int) std::round (smoothDev)) : juce::String(),
                    0, 40, 80, 14, juce::Justification::centred);

        // ---- cents scale
        const float cx0 = 92.0f, cx1 = (float) getWidth() - 4.0f;
        auto xAt = [&] (float cents) { return cx0 + (cents + 50.0f) / 100.0f * (cx1 - cx0); };
        const float base = 30.0f;

        g.setColour (Palette::accent.withAlpha (0.14f));
        g.fillRect (xAt (-5.0f), 12.0f, xAt (5.0f) - xAt (-5.0f), base - 12.0f);

        g.setColour (Palette::textFaint);
        g.drawLine (cx0, base, cx1, base, 1.0f);
        g.setColour (Palette::textDim);
        for (int c = -50; c <= 50; c += 10)
        {
            const float len = (c == 0) ? 12.0f : (std::abs (c) == 50 ? 9.0f : 5.0f);
            g.drawLine (xAt ((float) c), base - len, xAt ((float) c), base, 1.0f);
        }
        g.setFont (uiFont (9.0f));
        g.drawText ("-50", (int) xAt (-50.0f) - 15, 34, 30, 10, juce::Justification::centred);
        g.drawText ("0",   (int) xAt (0.0f) - 15,   34, 30, 10, juce::Justification::centred);
        g.drawText ("+50", (int) xAt (50.0f) - 15,  34, 30, 10, juce::Justification::centred);

        if (voiced)
        {
            const float x = xAt (juce::jlimit (-50.0f, 50.0f, smoothDev));
            g.setColour (Palette::accent);
            g.drawLine (x, 7.0f, x, base + 3.0f, 2.0f);
            juce::Path tri;
            tri.addTriangle (x - 3.5f, 0.5f, x + 3.5f, 0.5f, x, 7.0f);
            g.fillPath (tri);
        }

        // ---- correction bar
        g.setColour (Palette::textDim);
        g.setFont (uiFont (9.0f, true));
        g.drawText ("Correction", (int) cx0, 51, 100, 11, juce::Justification::centredLeft);
        g.setColour (Palette::text);
        g.setFont (uiFont (10.0f));
        g.drawText (juce::String::formatted ("%+d ct", (int) std::round (smoothCorr)),
                    (int) cx1 - 60, 51, 60, 11, juce::Justification::centredRight);

        const float ty = 64.0f, th = 6.0f, half = (cx1 - cx0) * 0.5f, mid = cx0 + half;
        g.setColour (Palette::well);
        g.fillRoundedRectangle (cx0, ty, cx1 - cx0, th, 2.0f);
        const float amt = juce::jlimit (-1.0f, 1.0f, smoothCorr / 100.0f);
        g.setColour (Palette::accent);
        if (amt >= 0.0f)
            g.fillRect (mid, ty + 1.0f, amt * half, th - 2.0f);
        else
            g.fillRect (mid + amt * half, ty + 1.0f, -amt * half, th - 2.0f);
        g.setColour (Palette::textDim);
        g.drawLine (mid, ty - 2.0f, mid, ty + th + 2.0f, 1.0f);
    }

private:
    static juce::String noteName (int midi)
    {
        static const char* n[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return juce::String (n[((midi % 12) + 12) % 12]) + juce::String (midi / 12 - 1);
    }

    bool voiced = false, wasVoiced = false;
    float smoothDev = 0.0f, smoothCorr = 0.0f;
    int note = 60;
};

//------------------------------------------------------------------------------
class AutotuneEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit AutotuneEditor (AutotuneProcessor& p)
        : juce::AudioProcessorEditor (p), proc (p), keyboard (p.apvts)
    {
        for (int i = 0; i < 12; ++i)
            noteP[(size_t) i] = proc.apvts.getRawParameterValue ("note" + juce::String (i));
        speedP    = proc.apvts.getRawParameterValue ("speed");
        strengthP = proc.apvts.getRawParameterValue ("strength");

        addAndMakeVisible (keyboard);
        addAndMakeVisible (meter);

        speedSlider.setName ("speed");
        strengthSlider.setName ("strength");
        for (auto* s : { &speedSlider, &strengthSlider })
        {
            s->setSliderStyle (juce::Slider::RotaryVerticalDrag);
            s->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            s->setRotaryParameters (juce::MathConstants<float>::pi * 1.25f,
                                    juce::MathConstants<float>::pi * 2.75f, true);
            s->setLookAndFeel (&laf);
            addAndMakeVisible (*s);
        }
        speedAtt    = std::make_unique<SliderAtt> (proc.apvts, "speed", speedSlider);
        strengthAtt = std::make_unique<SliderAtt> (proc.apvts, "strength", strengthSlider);
        speedSlider.setDoubleClickReturnValue (true, 30.0);
        strengthSlider.setDoubleClickReturnValue (true, 1.0);

        // key / scale
        keyBox.addItemList (juce::StringArray { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" }, 1);
        scaleBox.addItem ("Major", 1);
        scaleBox.addItem ("Minor", 2);
        scaleBox.addItem ("Chromatic", 3);
        scaleBox.addItem ("Custom", 4);
        keyBox.setSelectedId (1, juce::dontSendNotification);
        scaleBox.setSelectedId (1, juce::dontSendNotification);
        auto applyKeyScale = [this]
        {
            const int sc = scaleBox.getSelectedId();
            if (sc >= 1 && sc <= 3)
                applyScale (keyBox.getSelectedId() - 1, sc);
        };
        keyBox.onChange = applyKeyScale;
        scaleBox.onChange = applyKeyScale;

        // presets
        presetBox.onChange = [this]
        {
            const int id = presetBox.getSelectedId();
            const auto& fac = factoryPresets();
            if (id >= 1 && id <= (int) fac.size())
            {
                const auto& pr = fac[(size_t) (id - 1)];
                proc.applyPreset (pr.speed, pr.strength);
                proc.rememberProgram (id - 1);
            }
            else if (id >= 101 && id - 101 < (int) userPresets.size())
            {
                const auto& pr = userPresets[(size_t) (id - 101)];
                proc.applyPreset (pr.speed, pr.strength);
            }
        };
        saveBtn.onClick = [this] { promptSavePreset(); };

        for (auto* b : { &keyBox, &scaleBox, &presetBox })
        {
            b->setLookAndFeel (&laf);
            addAndMakeVisible (*b);
        }
        saveBtn.setLookAndFeel (&laf);
        addAndMakeVisible (saveBtn);

        lastMask = readMask();
        syncCombos (lastMask);
        refreshPresetList();

        setResizable (false, false);
        setSize (540, 272);
        startTimerHz (30);
    }

    ~AutotuneEditor() override
    {
        stopTimer();
        speedSlider.setLookAndFeel (nullptr);
        strengthSlider.setLookAndFeel (nullptr);
        keyBox.setLookAndFeel (nullptr);
        scaleBox.setLookAndFeel (nullptr);
        presetBox.setLookAndFeel (nullptr);
        saveBtn.setLookAndFeel (nullptr);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Palette::bg);

        g.setColour (Palette::header);
        g.fillRect (0, 0, getWidth(), 40);
        g.setColour (Palette::edgeLite);
        g.fillRect (0, 39, getWidth(), 1);
        g.setColour (Palette::edgeDark);
        g.fillRect (0, 40, getWidth(), 1);

        g.setColour (Palette::text);
        g.setFont (uiFont (14.0f, true));
        g.drawText ("Simple Autotune", 16, 0, 220, 40, juce::Justification::centredLeft);

        const double rate = proc.getSampleRate();
        const int latMs = rate > 0.0 ? (int) std::round (proc.getLatencySamples() * 1000.0 / rate) : 0;
        g.setColour (Palette::textDim);
        g.setFont (uiFont (10.0f));
        g.drawText ("Latency " + juce::String (latMs) + " ms", getWidth() - 136, 0, 120, 40,
                    juce::Justification::centredRight);

        drawPanel (g, panelScale, "Scale");
        drawPanel (g, panelPitch, "Input pitch");
        drawPanel (g, panelCtl,   "Tuning");

        g.setColour (Palette::textDim);
        g.setFont (uiFont (10.0f));
        g.drawText ("Retune speed", speedSlider.getX(),    74, speedSlider.getWidth(),    12, juce::Justification::centred);
        g.drawText ("Strength",     strengthSlider.getX(), 74, strengthSlider.getWidth(), 12, juce::Justification::centred);
        g.drawText ("Preset", presetBox.getX() + 2, 207, 80, 12, juce::Justification::centredLeft);
    }

    void resized() override
    {
        panelScale = { 12, 50, 300, 92 };
        panelPitch = { 12, 150, 300, 110 };
        panelCtl   = { 322, 50, 206, 210 };

        scaleBox.setBounds (panelScale.getRight() - 10 - 88, panelScale.getY() + 5, 88, 20);
        keyBox.setBounds (scaleBox.getX() - 6 - 48, panelScale.getY() + 5, 48, 20);

        keyboard.setBounds (panelScale.reduced (10).withTrimmedTop (22));
        meter.setBounds (panelPitch.reduced (10).withTrimmedTop (18));

        speedSlider.setBounds (panelCtl.getX() + 10, 88, 93, 112);
        strengthSlider.setBounds (panelCtl.getX() + 10 + 93, 88, 93, 112);

        presetBox.setBounds (panelCtl.getX() + 10, 224, 136, 22);
        saveBtn.setBounds (presetBox.getRight() + 6, 224, 44, 22);
    }

    // called from the save dialog
    void savePreset (const juce::String& rawName)
    {
        const auto name = juce::File::createLegalFileName (rawName.trim());
        if (name.isEmpty())
            return;

        juce::XmlElement xml ("PRESET");
        xml.setAttribute ("speed", (double) speedP->load());
        xml.setAttribute ("strength", (double) strengthP->load());
        xml.writeTo (presetDir().getChildFile (name + ".xml"));
        refreshPresetList();
    }

private:
    using SliderAtt = juce::AudioProcessorValueTreeState::SliderAttachment;

    //--------------------------------------------------------------------------
    void promptSavePreset()
    {
        auto* w = new juce::AlertWindow ("Save preset", "Name for this preset:",
                                         juce::MessageBoxIconType::NoIcon);
        w->addTextEditor ("name", "My preset " + juce::String ((int) userPresets.size() + 1));
        w->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
        w->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

        juce::Component::SafePointer<AutotuneEditor> safe (this);
        w->enterModalState (true,
                            juce::ModalCallbackFunction::create ([safe, w] (int result)
                            {
                                if (result == 1 && safe != nullptr)
                                    safe->savePreset (w->getTextEditorContents ("name"));
                            }),
                            true);
    }

    void refreshPresetList()
    {
        userPresets.clear();
        for (const auto& f : presetDir().findChildFiles (juce::File::findFiles, false, "*.xml"))
            if (auto xml = juce::parseXML (f))
                if (xml->hasTagName ("PRESET"))
                    userPresets.push_back ({ f.getFileNameWithoutExtension(),
                                             (float) xml->getDoubleAttribute ("speed", 30.0),
                                             (float) xml->getDoubleAttribute ("strength", 1.0) });
        std::sort (userPresets.begin(), userPresets.end(),
                   [] (const Preset& a, const Preset& b) { return a.name.compareIgnoreCase (b.name) < 0; });

        presetBox.clear (juce::dontSendNotification);
        int id = 1;
        for (const auto& pr : factoryPresets())
            presetBox.addItem (pr.name, id++);
        if (! userPresets.empty())
        {
            presetBox.addSeparator();
            presetBox.addSectionHeading ("My presets");
            int uid = 101;
            for (const auto& pr : userPresets)
                presetBox.addItem (pr.name, uid++);
        }
        syncPreset();
    }

    static bool matches (const Preset& p, float speed, float strength)
    {
        return std::abs (p.speed - speed) < 0.5f && std::abs (p.strength - strength) < 0.006f;
    }

    // keep the preset menu honest: show the matching preset, or "Custom" once a knob moves
    void syncPreset()
    {
        const float sp = speedP->load(), st = strengthP->load();
        int found = 0;

        int id = 1;
        for (const auto& pr : factoryPresets())
        {
            if (matches (pr, sp, st)) { found = id; break; }
            ++id;
        }
        if (found == 0)
        {
            id = 101;
            for (const auto& pr : userPresets)
            {
                if (matches (pr, sp, st)) { found = id; break; }
                ++id;
            }
        }

        if (found != 0)
        {
            presetBox.setSelectedId (found, juce::dontSendNotification);
        }
        else
        {
            presetBox.setSelectedId (0, juce::dontSendNotification);
            presetBox.setText ("Custom", juce::dontSendNotification);
        }
        lastSpeed = sp;
        lastStrength = st;
    }

    //--------------------------------------------------------------------------
    int readMask() const
    {
        int m = 0;
        for (int i = 0; i < 12; ++i)
            if (noteP[(size_t) i]->load() > 0.5f)
                m |= 1 << i;
        return m;
    }

    static int maskFor (int key, int scaleId)
    {
        static const int major[7] = { 0, 2, 4, 5, 7, 9, 11 };
        static const int minor[7] = { 0, 2, 3, 5, 7, 8, 10 };
        if (scaleId == 3)
            return 0xFFF;
        const int* s = (scaleId == 1) ? major : minor;
        int m = 0;
        for (int i = 0; i < 7; ++i)
            m |= 1 << ((key + s[i]) % 12);
        return m;
    }

    void applyScale (int key, int scaleId)
    {
        const int m = maskFor (key, scaleId);
        for (int i = 0; i < 12; ++i)
            if (auto* p = proc.apvts.getParameter ("note" + juce::String (i)))
            {
                p->beginChangeGesture();
                p->setValueNotifyingHost ((m & (1 << i)) ? 1.0f : 0.0f);
                p->endChangeGesture();
            }
    }

    // keep the Key / Scale boxes in step with whatever notes are switched on
    void syncCombos (int mask)
    {
        const int curKey = keyBox.getSelectedId() - 1;
        const int curScale = scaleBox.getSelectedId();
        if (curScale >= 1 && curScale <= 3 && curKey >= 0 && maskFor (curKey, curScale) == mask)
            return;

        for (int sc = 1; sc <= 3; ++sc)
            for (int k = 0; k < 12; ++k)
                if (maskFor (k, sc) == mask)
                {
                    keyBox.setSelectedId (k + 1, juce::dontSendNotification);
                    scaleBox.setSelectedId (sc, juce::dontSendNotification);
                    return;
                }
        scaleBox.setSelectedId (4, juce::dontSendNotification);
    }

    void timerCallback() override
    {
        const float m = proc.meterMidi.load (std::memory_order_relaxed);
        meter.update (m, proc.meterCorrSemis.load (std::memory_order_relaxed));
        keyboard.detectedPc = m > 0.0f ? (((int) std::round (m) % 12) + 12) % 12 : -1;
        keyboard.repaint();

        const int mask = readMask();
        if (mask != lastMask)
        {
            lastMask = mask;
            syncCombos (mask);
        }

        if (speedP->load() != lastSpeed || strengthP->load() != lastStrength)
            syncPreset();
    }

    AutotuneProcessor& proc;
    DeskLookAndFeel laf;
    PianoKeyboard keyboard;
    PitchMeter meter;
    juce::Slider speedSlider, strengthSlider;
    juce::ComboBox keyBox, scaleBox, presetBox;
    juce::TextButton saveBtn { "Save" };
    std::unique_ptr<SliderAtt> speedAtt, strengthAtt;
    std::array<std::atomic<float>*, 12> noteP {};
    std::atomic<float>* speedP = nullptr;
    std::atomic<float>* strengthP = nullptr;
    std::vector<Preset> userPresets;
    int lastMask = 0;
    float lastSpeed = -1.0f, lastStrength = -1.0f;
    juce::Rectangle<int> panelScale, panelPitch, panelCtl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutotuneEditor)
};

//==============================================================================
juce::AudioProcessorEditor* AutotuneProcessor::createEditor()
{
    return new AutotuneEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AutotuneProcessor();
}
