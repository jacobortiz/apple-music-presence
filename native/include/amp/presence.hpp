#pragma once
#include <amp/model.hpp>
#include <nlohmann/json.hpp>

namespace amp {
nlohmann::json presence(const Snapshot&, double now, const std::optional<Artwork>& = {});
bool materially_changed(const nlohmann::json& before, const nlohmann::json& after);
}
