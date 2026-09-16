#include "pch.h"

#include "DlssNr.h"
#include "DlssNr_MenuSections.h"
#include <Config.h>
#include <algorithm>
#include <cmath>

namespace DlssNr::MenuSections
{

void RenderInput(Config* config)
{
    // Resolution changes rebuild model resources; commit only after releasing the slider.
    static int pendingScale = -1;

    int scalePercent =
        pendingScale >= 0 ? pendingScale : (int) lroundf(config->DlssNrWorkingScale.value_or_default() * 100.0f);

    if (ImGui::SliderInt("Model resolution", &scalePercent, 25, 200, "%d%%"))
        pendingScale = scalePercent;

    if (ImGui::IsItemDeactivatedAfterEdit() && pendingScale >= 0)
    {
        config->DlssNrWorkingScale = std::clamp(pendingScale, 25, 200) / 100.0f;
        pendingScale = -1;
    }

    HelpMarker("50% halves width and height. 100% uses the full input size.");

    if (scalePercent > 100)
    {
        static const char* dsNames[] = { "FSR1",     "Bicubic", "Catmull-Rom", "Lanczos2",
                                         "Lanczos3", "Kaiser2", "Kaiser3",     "MAGIC" };
        int ds = (int) config->DlssNrScalingDownscaler.value_or_default();
        if (ds < 0 || ds >= IM_ARRAYSIZE(dsNames))
            ds = (int) Scaler::Lanczos3;

        if (ImGui::Combo("Downscaler (NR)", &ds, dsNames, IM_ARRAYSIZE(dsNames)))
            config->DlssNrScalingDownscaler = (Scaler) ds;

        HelpMarker("Downsampling filter for resolutions above 100%.");
    }
    {
        const bool reduced = config->DlssNrWorkingScale.value_or_default() < 0.999f;

        ImGui::BeginDisabled(!reduced);

        static const char* enlargeNames[] = { "Classic", "Matched residual", "Matched residual + DLSS" };
        int enlarge = (int) std::min(config->DlssNrTransfer.value_or_default(), 2u);

        if (ImGui::Combo("Enlargement", &enlarge, enlargeNames, IM_ARRAYSIZE(enlargeNames)))
            config->DlssNrTransfer = (uint32_t) enlarge;

        ImGui::EndDisabled();

        HelpMarker("Below 100%: enlarge the output or the NR difference. DLSS requires post-upscale DX12 processing.");
    }
    static const char* reversibleNames[] = { "Off (soft knee)", "Neutwo proxy + composed", "Neutwo proxy + replace",
                                             "Hybrid proxy + composed", "Hybrid proxy + replace" };
    int reversible = (int) config->DlssNrReversibleMode.value_or_default();
    if (reversible < 0 || reversible > 4)
        reversible = 0;
    if (ImGui::Combo("HDR mapping (experimental)", &reversible, reversibleNames, IM_ARRAYSIZE(reversibleNames)))
        config->DlssNrReversibleMode = (uint32_t) reversible;

    HelpMarker("HDR mapping curve. Replace bypasses strength and highlight controls.");

    float wpScale = config->DlssNrWhitePointScale.value_or_default();
    if (ImGui::SliderFloat("Paper white", &wpScale, 0.25f, 2000.0f, "%.2fx", ImGuiSliderFlags_Logarithmic))
        config->DlssNrWhitePointScale = wpScale;
    HelpMarker("Higher values darken the NR input; lower values brighten it.");
}

} // namespace DlssNr::MenuSections
