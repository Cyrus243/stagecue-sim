#pragma once

#include "Meters.h"

#include <ftxui/component/component_options.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include <string>

/** Look and feel of the TUI: a dark "control room" dashboard with thin grey panels,
    amber-accented uppercase titles, muted status chips and segmented meters. */
namespace sim::style
{
    using namespace ftxui;

    inline const Color kBg = Color::RGB(10, 10, 12);
    inline const Color kPanelBorder = Color::RGB(58, 58, 64);
    inline const Color kText = Color::RGB(214, 214, 218);
    inline const Color kBright = Color::RGB(245, 245, 247);
    inline const Color kMuted = Color::RGB(128, 128, 138);
    inline const Color kUnlit = Color::RGB(42, 42, 48);
    inline const Color kAccent = Color::RGB(232, 142, 40);
    inline const Color kGreen = Color::RGB(84, 214, 128);
    inline const Color kAmber = Color::RGB(232, 196, 72);
    inline const Color kRed = Color::RGB(232, 76, 76);
    inline const Color kBlue = Color::RGB(88, 140, 240);
    inline const Color kDimGreenBg = Color::RGB(28, 64, 40);
    inline const Color kDimRedBg = Color::RGB(72, 24, 24);
    inline const Color kFieldBg = Color::RGB(46, 46, 52);

    constexpr float kMeterRedDb = -6.0f;
    constexpr float kMeterAmberDb = -18.0f;

    inline std::string fixed(float value, int precision)
    {
        std::ostringstream stream;
        stream << std::fixed << std::setprecision(precision) << value;
        return stream.str();
    }

    inline std::string upper(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return value;
    }

    inline std::string formatDb(float db) { return db <= kMeterFloorDb ? "-inf" : fixed(db, 1); }

    inline Element muted(const std::string& value) { return text(value) | color(kMuted); }

    /** One column of breathing room inside a border. */
    inline Element padded(Element content) { return hbox({ text(" "), std::move(content) | flex, text(" ") }); }

    /** Grey-bordered box with an amber bar and an uppercase title, optionally followed by a muted subtitle. */
    inline Element panel(const std::string& title, Element content, const std::string& subtitle = {})
    {
        Elements heading { text("▌") | color(kAccent), text(upper(title)) | bold | color(kBright) };
        if (!subtitle.empty())
            heading.push_back(text("  " + upper(subtitle)) | color(kMuted));

        return padded(vbox({ hbox(std::move(heading)), text(""), std::move(content) })) | borderStyled(LIGHT, kPanelBorder);
    }

    /** Status chip like ONLINE / ACTIVE: muted fill, bright text. */
    inline Element chip(bool ok, const std::string& okText, const std::string& badText)
    {
        return ok ? text(" " + upper(okText) + " ") | bgcolor(kDimGreenBg) | color(kGreen)
                  : text(" " + upper(badText) + " ") | bgcolor(kDimRedBg) | color(kRed);
    }

    /** Red outlined tag like [CLASSIFIED ACCESS]. */
    inline Element tag(const std::string& value) { return text("[" + upper(value) + "]") | color(kRed) | bgcolor(kDimRedBg); }

    /** "Label: [value]" pair, the value on a field-grey background. */
    inline Element labelValue(const std::string& label, const std::string& value)
    {
        return hbox({ muted(label + ": "), text(" " + value + " ") | bgcolor(kFieldBg) | color(kBright) });
    }

    /** Boxed metric: big bold value over a muted caption. */
    inline Element kpiTile(const std::string& value, const std::string& caption, Color valueColor = kBright)
    {
        return vbox({
                   text(value) | bold | color(valueColor) | hcenter,
                   text(caption) | color(kMuted) | hcenter,
               })
             | size(WIDTH, GREATER_THAN, 12) | borderStyled(LIGHT, kPanelBorder) | flex;
    }

    /** Segmented meter: `cells` bars, lit ones green/amber/red by the level they stand for, then the dB value. */
    inline Element segmentedMeter(float db, int cells)
    {
        const float fraction = std::clamp((db - kMeterFloorDb) / -kMeterFloorDb, 0.0f, 1.0f);
        const int lit = static_cast<int>(std::lround(fraction * static_cast<float>(cells)));

        Elements segments;
        for (int i = 0; i < cells; ++i)
        {
            const float cellDb = kMeterFloorDb * (1.0f - static_cast<float>(i + 1) / static_cast<float>(cells));
            const Color litColor = cellDb > kMeterRedDb ? kRed : cellDb > kMeterAmberDb ? kAmber : kGreen;
            segments.push_back(text("▌") | color(i < lit ? litColor : kUnlit));
        }

        const Color valueColor = db > kMeterRedDb ? kRed : db <= kMeterFloorDb ? kMuted : kBright;
        segments.push_back(text(" " + formatDb(db)) | bold | color(valueColor) | size(WIDTH, EQUAL, 6));
        return hbox(std::move(segments));
    }

    enum class Dot { Live, Idle, Warn, Error };

    /** Coloured dot plus label, like the status column of a table. */
    inline Element statusDot(Dot state, const std::string& label)
    {
        const Color dotColor = state == Dot::Live ? kGreen : state == Dot::Warn ? kAmber : state == Dot::Error ? kRed : kMuted;
        return hbox({ text(state == Dot::Idle ? "○ " : "● ") | color(dotColor),
                      text(label) | color(state == Dot::Idle ? kMuted : kText) });
    }

    /** Muted uppercase table header cell of a fixed width (0 = flex). */
    inline Element headerCell(const std::string& label, int width)
    {
        auto cell = text(upper(label)) | color(kMuted);
        return width > 0 ? cell | size(WIDTH, EQUAL, width) : cell | flex;
    }

    // ------------------------------------------------------- component options

    inline MenuOption toggleOption()
    {
        auto option = MenuOption::Toggle();
        option.elements_infix = [] { return text(" "); };
        option.entries_option.transform = [](const EntryState& state) {
            auto label = text(" " + state.label + " ");
            if (state.focused)
                return label | bgcolor(kAccent) | color(kBg) | bold;
            if (state.active)
                return label | bgcolor(kFieldBg) | color(kAccent) | bold;
            return label | color(kMuted);
        };
        return option;
    }

    template <typename T>
    SliderOption<T> sliderOption(T* value, T min, T max, T increment)
    {
        SliderOption<T> option;
        option.value = value;
        option.min = min;
        option.max = max;
        option.increment = increment;
        option.color_active = kAccent;
        option.color_inactive = kUnlit;
        return option;
    }

    /** Filled white button (like APPROVE). */
    inline ButtonOption primaryButton()
    {
        ButtonOption option;
        option.transform = [](const EntryState& state) {
            auto label = text("  " + upper(state.label) + "  ") | bold;
            return state.focused ? label | bgcolor(kAccent) | color(kBg)
                                 : label | bgcolor(kBright) | color(kBg);
        };
        return option;
    }

    /** Outlined button (like CANCEL). */
    inline ButtonOption secondaryButton()
    {
        ButtonOption option;
        option.transform = [](const EntryState& state) {
            auto label = text("[ " + upper(state.label) + " ]");
            return state.focused ? label | bgcolor(kAccent) | color(kBg) | bold : label | color(kText);
        };
        return option;
    }

    inline CheckboxOption checkboxOption()
    {
        auto option = CheckboxOption::Simple();
        option.transform = [](const EntryState& state) {
            auto box = text(state.state ? "[x] " : "[ ] ") | color(state.state ? kAccent : kMuted);
            auto label = text(state.label) | color(state.state ? kText : kMuted);
            auto element = hbox({ box, label });
            return state.focused ? element | inverted : element;
        };
        return option;
    }

    inline InputOption inputOption(const std::string& placeholder)
    {
        InputOption option;
        option.multiline = false;
        option.placeholder = placeholder;
        option.transform = [](InputState state) {
            state.element |= color(kText);
            if (state.is_placeholder)
                state.element |= color(kMuted);
            state.element |= bgcolor(state.focused ? kFieldBg : kUnlit);
            return state.element;
        };
        return option;
    }
}
