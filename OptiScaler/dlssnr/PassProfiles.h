#pragma once
#include <Config.h>
#include "DlssNr_ModelParameters.h"
#include <algorithm>
#include <cmath>

namespace DlssNr::Profiles
{
inline unsigned int PassPreset(const Config& cfg, unsigned int pass)
{
    const unsigned int base = std::min(cfg.DlssNrPreset.value_or_default(), 3u);

    if (pass == 1 && cfg.DlssNrPass2Preset.has_value())
        return std::min(cfg.DlssNrPass2Preset.value(), 3u);

    if (pass == 2 && cfg.DlssNrPass3Preset.has_value())
        return std::min(cfg.DlssNrPass3Preset.value(), 3u);

    return base;
}

inline unsigned int PassStyle(const Config& cfg, unsigned int pass)
{
    const unsigned int base = std::min(cfg.DlssNrStyle.value_or_default(), 2u);

    if (pass == 1 && cfg.DlssNrPass2Style.has_value())
        return std::min(cfg.DlssNrPass2Style.value(), 2u);

    if (pass == 2 && cfg.DlssNrPass3Style.has_value())
        return std::min(cfg.DlssNrPass3Style.value(), 2u);

    if (pass >= 3 && pass < 30 && cfg.DlssNrExtraPasses[pass - 3].style.has_value())
        return std::min(cfg.DlssNrExtraPasses[pass - 3].style.value(), 2u);

    return base;
}

inline ModelSettings PassSettings(const Config& cfg, unsigned int pass)
{
    ModelSettings result { PassPreset(cfg, pass), PassStyle(cfg, pass), cfg.DlssNrIntensity.value_or_default(),
                          cfg.DlssNrLocalStructure.value_or_default(),
                          pass == 0 ? cfg.DlssNrLocalTone.value_or_default() : 0.0f,
                          cfg.DlssNrSkinStructure.value_or_default(),
                          cfg.DlssNrAutoMask.value_or_default() };
    if (pass == 1)
    {
        result.intensity = cfg.DlssNrPass2Intensity.value_or(result.intensity);
        result.localStructure = cfg.DlssNrPass2LocalStructure.value_or(result.localStructure);
        result.localTone = cfg.DlssNrPass2LocalTone.value_or(result.localTone);
        result.skinStructure = cfg.DlssNrPass2SkinStructure.value_or(result.skinStructure);
        result.autoMask = cfg.DlssNrPass2AutoMask.value_or(result.autoMask);
    }
    if (pass == 2)
    {
        result.intensity = cfg.DlssNrPass3Intensity.value_or(result.intensity);
        result.localStructure = cfg.DlssNrPass3LocalStructure.value_or(result.localStructure);
        result.localTone = cfg.DlssNrPass3LocalTone.value_or(result.localTone);
        result.skinStructure = cfg.DlssNrPass3SkinStructure.value_or(result.skinStructure);
        result.autoMask = cfg.DlssNrPass3AutoMask.value_or(result.autoMask);
    }
    if (pass >= 3 && pass < 30)
    {
        const auto& extra = cfg.DlssNrExtraPasses[pass - 3];
        result.intensity = extra.intensity.value_or(result.intensity);
        result.localStructure = extra.structure.value_or(result.localStructure);
        result.localTone = extra.tone.value_or(result.localTone);
        result.skinStructure = extra.skin.value_or(result.skinStructure);
        result.autoMask = extra.autoMask.value_or(result.autoMask);
    }
    const auto bounded = [](float value, float fallback, float minimum) {
        return std::isfinite(value) ? std::clamp(value, minimum, 2.0f) : fallback;
    };
    result.intensity = bounded(result.intensity, 1.0f, 0.0f);
    result.localStructure = bounded(result.localStructure, 1.0f, 0.0f);
    result.localTone = bounded(result.localTone, pass == 0 ? 1.0f : 0.0f, 0.0f);
    result.skinStructure = bounded(result.skinStructure, -1.0f, -1.0f);
    return result;
}
}
