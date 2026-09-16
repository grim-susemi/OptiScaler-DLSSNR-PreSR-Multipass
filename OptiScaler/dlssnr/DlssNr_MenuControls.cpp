#include "pch.h"
#include "DlssNr_MenuSections.h"
#include <Config.h>
#include <menu/menu_common.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>

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

// Model tuning rebuilds the feature; commit slider changes only on release.
template <typename Option>
static void DeferredSlider(const char* label, Option* opt, float mn, float mx, float def, bool inheritReset = false)
{
    static std::unordered_map<ImGuiID, float> pending;
    const ImGuiID id = ImGui::GetID(label);

    auto it = pending.find(id);
    float value = it != pending.end() ? it->second : (opt->value_or(def));

    if (ImGui::SliderFloat(label, &value, mn, mx, "%.2f"))
        pending[id] = value;

    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        auto committed = pending.find(id);

        if (committed != pending.end())
        {
            *opt = std::clamp(committed->second, mn, mx);
            pending.erase(committed);
        }
    }

    ImGui::SameLine();

    const std::string resetId = std::string("Reset##") + label;
    if (ImGui::SmallButton(resetId.c_str()))
    {
        if (inheritReset)
            *opt = std::optional<float> {};
        else
            *opt = def;
        pending.erase(id);
    }

    if (std::strcmp(label, "Intensity") == 0)
        HelpMarker("Enhancement strength. 1 = default.");
    else if (std::strcmp(label, "Local structure") == 0)
        HelpMarker("Fine detail and local contrast. 1 = default.");
    else if (std::strcmp(label, "Local tone") == 0)
        HelpMarker("Broad lighting changes. Later passes default to 0.");
    else if (std::strcmp(label, "Skin structure") == 0)
        HelpMarker("Skin detail. -1 follows Local structure.");
}

// An absent later-pass setting inherits pass 1. The first combo item represents that absence; the
// remaining items map directly to the model's zero-based profile values.
static void InheritedProfileCombo(const char* label, CustomOptional<uint32_t, NoDefault>* opt, const char* const* names,
                                  int nameCount)
{
    int selected = 0;

    if (opt->has_value())
        selected = std::clamp((int) opt->value(), 0, nameCount - 2) + 1;

    if (!ImGui::Combo(label, &selected, names, nameCount))
        return;

    if (selected == 0)
        *opt = std::optional<uint32_t> {};
    else
        *opt = (uint32_t) (selected - 1);

}

void RenderModel(Config* config)
{
    bool unlockPasses = config->DlssNrUnlockPasses.value_or_default();
    const int menuPassLimit = unlockPasses ? 10 : 2;
    static int passes = 1;
    static bool editingPasses = false;
    if (!editingPasses)
        passes = (int) std::clamp(config->DlssNrPasses.value_or_default(), 1u, (unsigned) menuPassLimit);
    ImGui::SliderInt("Model passes", &passes, 1, menuPassLimit, "%d", ImGuiSliderFlags_AlwaysClamp);
    editingPasses = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit())
        config->DlssNrPasses = (uint32_t) std::clamp(passes, 1, menuPassLimit);
    if (ImGui::Checkbox("Unlock up to 10 passes", &unlockPasses))
    {
        config->DlssNrUnlockPasses = unlockPasses;
        config->DlssNrPasses = std::clamp(config->DlssNrPasses.value_or_default(), 1u, unlockPasses ? 10u : 2u);
    }

    static unsigned selectedPass = 0;
    const auto selectedLabel = std::format("Pass {}", selectedPass + 1);
    if (ImGui::BeginCombo("Edit pass", selectedLabel.c_str()))
    {
        for (unsigned pass = 0; pass <= std::size(config->DlssNrPassOverrides); ++pass)
        {
            const auto label = std::format("Pass {}", pass + 1);
            if (ImGui::Selectable(label.c_str(), selectedPass == pass))
                selectedPass = pass;
            if (selectedPass == pass)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (selectedPass >= config->DlssNrPasses.value_or_default())
        ImGui::TextDisabled("Inactive pass. Settings apply when this pass is enabled.");

    // Distinct widget IDs keep uncommitted slider edits with their selected pass.
    ImGui::PushID((int) selectedPass);
    const bool inherited = selectedPass != 0;
    const auto tuning = [&](auto& intensity, auto& structure, auto& tone, auto& skin, auto& autoMask)
    {
        DeferredSlider("Intensity", &intensity, 0.0f, 2.0f,
                       inherited ? config->DlssNrIntensity.value_or_default() : 1.0f, inherited);
        DeferredSlider("Local structure", &structure, 0.0f, 2.0f,
                       inherited ? config->DlssNrLocalStructure.value_or_default() : 1.0f, inherited);
        DeferredSlider("Local tone", &tone, 0.0f, 2.0f, inherited ? 0.0f : 1.0f, inherited);
        DeferredSlider("Skin structure", &skin, -1.0f, 2.0f,
                       inherited ? config->DlssNrSkinStructure.value_or_default() : -1.0f, inherited);
        bool mask = autoMask.value_or(inherited ? config->DlssNrAutoMask.value_or_default() : true);
        if (ImGui::Checkbox("Auto skin mask", &mask))
            autoMask = mask;
        if (inherited)
        {
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset##mask"))
                autoMask = std::optional<bool> {};
        }
        HelpMarker("Model-based skin selection.");
    };
    static const char* styles[] = { "Standard", "Natural", "Cinematic" };
    static const char* inheritedStyles[] = { "Auto", "Standard", "Natural", "Cinematic" };
    if (selectedPass == 0)
    {
        int style = (int) std::min(config->DlssNrStyle.value_or_default(), 2u);
        if (ImGui::Combo("Style", &style, styles, IM_ARRAYSIZE(styles)))
            config->DlssNrStyle = (uint32_t) style;
        tuning(config->DlssNrIntensity, config->DlssNrLocalStructure, config->DlssNrLocalTone,
               config->DlssNrSkinStructure, config->DlssNrAutoMask);
    }
    else
    {
        auto& pass = config->DlssNrPassOverrides[selectedPass - 1];
        InheritedProfileCombo("Style", &pass.style, inheritedStyles, IM_ARRAYSIZE(inheritedStyles));
        tuning(pass.intensity, pass.structure, pass.tone, pass.skin, pass.autoMask);
    }
    ImGui::PopID();
}

void RenderBlend(Config* config)
{
    if (config->DlssNrFinishedPicture.value_or_default() &&
        (config->DlssNrRunBeforeSr.value_or_default() || config->DlssNrDeferredDlss.value_or_default()))
    {
        const auto feature = State::Instance().currentFeature;
        ImGui::BeginDisabled(State::Instance().swapchainApi == API::Vulkan ||
                             (feature && feature->GetUpscalerType() == Upscaler::DLSSD));
        bool hdrTransfer = config->DlssNrHdrTransfer.value_or_default();
        if (ImGui::Checkbox("Match HDR brightness response (experimental)", &hdrTransfer))
            config->DlssNrHdrTransfer = hdrTransfer;
        ImGui::EndDisabled();
        HelpMarker("Match early NR brightness changes to the finished HDR image. Adds GPU work; unreliable fits fall back.");
    }
    float transfer = config->DlssNrTransferStrength.value_or_default();
    if (ImGui::SliderFloat("Detail strength", &transfer, 0.0f, 2.0f, "%.2f"))
        config->DlssNrTransferStrength = transfer;

    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##detail"))
        config->DlssNrTransferStrength = 1.0f;

    HelpMarker("0 = no detail change. 1 = normal.");

    float colour = config->DlssNrColourStrength.value_or_default();
    if (ImGui::SliderFloat("Colour strength", &colour, 0.0f, 4.0f, "%.2f"))
        config->DlssNrColourStrength = colour;

    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##colour"))
        config->DlssNrColourStrength = 1.0f;

    HelpMarker("0 = game colours. 1 = model colours. Above 1 boosts saturation.");

    if (ImGui::TreeNode("Skin and environment (final edit)"))
    {
        bool filter = config->DlssNrSkinProtection.value_or_default();
        if (ImGui::Checkbox("Separate skin / environment controls", &filter))
            config->DlssNrSkinProtection = filter;
        ImGui::BeginDisabled(!filter);
        const auto slider = [](const char* label, auto& option)
        {
            float v = option.value_or_default();
            if (ImGui::SliderFloat(label, &v, 0.0f, 1.0f, "%.2f"))
                option = v;
            HelpMarker("0 = unchanged. 1 = full effect.");
        };
        slider("Skin detail / lighting", config->DlssNrSkinDetail);
        slider("Skin colour", config->DlssNrSkinColour);
        slider("Environment detail / lighting", config->DlssNrEnvironmentDetail);
        slider("Environment colour", config->DlssNrEnvironmentColour);
        bool preview = config->DlssNrShowSkinMask.value_or_default();
        if (ImGui::Checkbox("Preview colour-based mask", &preview))
            config->DlssNrShowSkinMask = preview;
        ImGui::EndDisabled();
        ImGui::TreePop();
    }

    float maxRatio = config->DlssNrMaxRatio.value_or_default();
    if (ImGui::SliderFloat("Highlight guard", &maxRatio, 1.0f, 8.0f, "%.1fx"))
        config->DlssNrMaxRatio = maxRatio;

    ImGui::SameLine();
    if (ImGui::SmallButton("Reset##guard"))
        config->DlssNrMaxRatio = 2.0f;

    HelpMarker("Limit pixel brightening and darkening.");
}

void RenderInspect(Config* config)
{
    bool held = config->DlssNrHoldFrame.value_or_default();
    if (ImGui::Checkbox("Hold frame", &held))
        config->DlssNrHoldFrame = held;

    HelpMarker("Freeze a frame for NR tuning. Later game effects may update; temporal behaviour is not representative.");

    static const char* compareNames[] = { "Off", "Side by side", "Wipe" };
    int compare = (int) config->DlssNrCompare.value_or_default();
    if (ImGui::Combo("Compare", &compare, compareNames, IM_ARRAYSIZE(compareNames)))
        config->DlssNrCompare = (uint32_t) compare;

    HelpMarker("Compare the original and NR output.");

    if (compare != 0)
    {
        bool swap = config->DlssNrCompareSwap.value_or_default();
        if (ImGui::Checkbox("Swap sides", &swap))
            config->DlssNrCompareSwap = swap;


        bool tags = config->DlssNrCompareTags.value_or_default();
        if (ImGui::Checkbox("Label the sides", &tags))
            config->DlssNrCompareTags = tags;



        if (tags)
        {
            float tagScale = config->DlssNrTagScale.value_or_default();
            if (ImGui::SliderFloat("Label size", &tagScale, 0.5f, 5.0f, "%.1fx"))
                config->DlssNrTagScale = std::clamp(tagScale, 0.5f, 5.0f);
        }
    }

    if (compare == 1)
    {
        float zoom = config->DlssNrCompareZoom.value_or_default();
        if (ImGui::SliderFloat("Zoom", &zoom, 1.0f, 2.0f, "%.2f"))
            config->DlssNrCompareZoom = std::clamp(zoom, 1.0f, 2.0f);

        HelpMarker("1 = fit. 2 = crop and enlarge.");
    }

    if (compare == 2)
    {
        float split = config->DlssNrCompareSplit.value_or_default();
        if (ImGui::SliderFloat("Split", &split, 0.0f, 1.0f, "%.2f"))
            config->DlssNrCompareSplit = std::clamp(split, 0.0f, 1.0f);

        HelpMarker("Move the comparison boundary.");
    }

    static const char* debugNames[] = { "Off", "Proxy (what the model sees)", "Model output (raw)",
                                        "Difference (amplified)" };
    int debugView = (int) config->DlssNrDebugView.value_or_default();
    if (ImGui::Combo("Debug view", &debugView, debugNames, IM_ARRAYSIZE(debugNames)))
        config->DlssNrDebugView = (uint32_t) debugView;

    HelpMarker("Difference is amplified 20x. Grey means unchanged.");
}
} // namespace DlssNr::MenuSections
