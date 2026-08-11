#include "core/render/ui_coverage.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void expect(float actual, float expected, const char *label) {
    if (std::abs(actual - expected) > 1e-6f)
        throw std::runtime_error(std::string(label) + " coverage mismatch");
}
}

int main() {
    try {
        expect(mcvr::ui::accumulateCoverage(0.0f, 0.0f), 0.0f, "world");
        expect(mcvr::ui::accumulateCoverage(0.0f, 0.5f), 0.5f, "half-transparent UI");
        expect(mcvr::ui::accumulateCoverage(0.5f, 0.5f), 0.75f, "overlapping UI");
        expect(mcvr::ui::accumulateCoverage(0.3f, 1.0f), 1.0f, "opaque UI");
        expect(mcvr::ui::accumulateCoverage(0.6f, 0.0f), 0.6f, "transparent draw");
        expect(mcvr::ui::accumulateCoverage(-1.0f, 2.0f), 1.0f, "clamping");
        if (!mcvr::ui::sourceOverColor(VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, false))
            throw std::runtime_error("ordinary blend classification");
        if (mcvr::ui::sourceOverColor(VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_OP_ADD, false))
            throw std::runtime_error("inversion incorrectly accepted as source-over");
        const float alpha = .5f, ui = .3f * alpha;
        for (float generatedBackground : {.1f, .9f}) {
            const float real = ui + (1-alpha)*.2f;
            const float recovered = real - (1-alpha)*.2f;
            expect(recovered + (1-alpha)*generatedBackground,
                ui + (1-alpha)*generatedBackground, "recomposition changes only background");
        }
        VkColorBlendEquationEXT inverse{VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
            VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_OP_ADD};
        mcvr::ui::coverageEquation(inverse, false);
        if (inverse.srcAlphaBlendFactor != VK_BLEND_FACTOR_ZERO || inverse.dstAlphaBlendFactor != VK_BLEND_FACTOR_ONE)
            throw std::runtime_error("affine background effect overwrote fractional UI coverage");
        for (float source : {0.f, .3f, 1.f}) for (float background : {.1f, .9f}) {
            const auto transform = [source](float x) { return source + (1-2*source)*x; };
            const float transformedUi = (1-2*source)*ui + source*alpha;
            expect(transformedUi + (1-alpha)*transform(background),
                transform(ui+(1-alpha)*background), "affine HUD-less recomposition");
        }
        std::cout << "UI coverage source-over contract passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
