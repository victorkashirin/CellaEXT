#pragma once

#include "plugin.hpp"

struct ScrewGrey : app::ThemedSvgScrew {
    ScrewGrey()
    {
        setSvg(Svg::load(asset::plugin(
                   pluginInstance, "res/components/ScrewGrey.svg")),
            Svg::load(asset::plugin(
                pluginInstance, "res/components/ScrewDark.svg")));
    }
};

struct VCVButtonHuge : app::SvgSwitch {
    VCVButtonHuge()
    {
        momentary = true;
        addFrame(Svg::load(asset::plugin(
            pluginInstance, "res/components/VCVButtonHuge_0.svg")));
        addFrame(Svg::load(asset::plugin(
            pluginInstance, "res/components/VCVButtonHuge_1.svg")));
    }
};

struct VCVButtonHugeToggle : app::SvgSwitch {
    VCVButtonHugeToggle()
    {
        momentary = false;
        addFrame(Svg::load(asset::plugin(
            pluginInstance, "res/components/VCVButtonHuge_0.svg")));
        addFrame(Svg::load(asset::plugin(
            pluginInstance, "res/components/VCVButtonHuge_1.svg")));
    }
};
