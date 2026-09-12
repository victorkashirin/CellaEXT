#include "Expression.hpp"

#include <algorithm>
#include <cmath>

namespace cella::sfz {

float normalizedVoltage(float volts) noexcept
{
    if (!std::isfinite(volts))
        return 0.0f;
    return std::clamp(volts * 0.1f, 0.0f, 1.0f);
}

float polyVoltageForLane(const PolyVoltage& input, size_t lane,
    float disconnectedValue) noexcept
{
    const size_t channels = std::min<size_t>(input.channels, input.values.size());
    if (channels == 0)
        return disconnectedValue;
    const float value = channels == 1 ? input.values[0]
                                      : (lane < channels ? input.values[lane]
                                                         : disconnectedValue);
    return std::isfinite(value) ? value : disconnectedValue;
}

int SelectorQuantizer::process(size_t lane, float voltage,
    int switchCount) noexcept
{
    if (lane >= selections_.size() || switchCount <= 1) {
        if (lane < selections_.size())
            selections_[lane] = 0;
        return 0;
    }
    switchCount = std::min(switchCount, 128);
    voltage = std::isfinite(voltage) ? std::clamp(voltage, 0.0f, 10.0f) : 0.0f;
    int current = std::clamp<int>(selections_[lane], 0, switchCount - 1);
    const float step = 10.0f / static_cast<float>(switchCount - 1);
    const float hysteresis = step * HysteresisFraction;
    while (current + 1 < switchCount) {
        const float boundary = (static_cast<float>(current) + 0.5f) * step;
        if (voltage <= boundary + hysteresis)
            break;
        ++current;
    }
    while (current > 0) {
        const float boundary = (static_cast<float>(current) - 0.5f) * step;
        if (voltage >= boundary - hysteresis)
            break;
        --current;
    }
    selections_[lane] = static_cast<int16_t>(current);
    return current;
}

void SelectorQuantizer::reset(int selection) noexcept
{
    selections_.fill(static_cast<int16_t>(std::clamp(selection, 0, 127)));
}

const NamedController* findNamedController(
    const InstrumentMetadata& metadata, int cc) noexcept
{
    for (const NamedController& controller : metadata.namedControllers) {
        if (controller.number == cc)
            return &controller;
    }
    return nullptr;
}

} // namespace cella::sfz
