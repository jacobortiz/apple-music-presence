#pragma once
#include <amp/presence.hpp>
#include <algorithm>
#include <deque>
#include <limits>
#include <stdexcept>

namespace amp {
// Count attempts, including failed RPC calls, because Discord may have received
// a request before its acknowledgement was lost. Keep one slot for prompt clears.
class ActivitySchedule {
public:
    double next(const nlohmann::json& activity, double now) const {
        double deadline = !acknowledged_ || materially_changed(sent_, activity)
            ? now : std::max(now, acknowledged_at_ + 30.0);
        if (!activity.is_null()) deadline = std::max(deadline, last_attempt_ + 1.0);
        return capacity_at(activity.is_null(), deadline);
    }
    bool due(const nlohmann::json& activity, double now) const {
        return next(activity, now) <= now;
    }
    void attempt(const nlohmann::json& activity, double now) {
        const bool clear = activity.is_null();
        if (capacity_at(clear, now) > now || (!clear && now < last_attempt_ + 1.0))
            throw std::logic_error("Discord activity update budget is exhausted");
        while (!attempts_.empty() && attempts_.front().at + window <= now) attempts_.pop_front();
        attempts_.push_back({now, !clear});
        last_attempt_ = now;
        if (!clear) may_be_active_ = true;
    }
    void acknowledge(const nlohmann::json& activity, double now) {
        sent_ = activity;
        acknowledged_ = true;
        acknowledged_at_ = now;
        may_be_active_ = !activity.is_null();
    }
    void disconnected() noexcept { acknowledged_ = false; }
    bool can_clear(double now) const { return capacity_at(true, now) <= now; }
    bool may_be_active() const noexcept { return may_be_active_; }
private:
    double capacity_at(bool clear, double deadline) const {
        for (;;) {
            unsigned total{}, updates{};
            double first = std::numeric_limits<double>::infinity(), first_update = first;
            for (const auto& attempt : attempts_) {
                const double expires = attempt.at + window;
                if (expires <= deadline) continue;
                ++total; first = std::min(first, expires);
                if (attempt.update) { ++updates; first_update = std::min(first_update, expires); }
            }
            double available = deadline;
            if (total >= 5) available = std::max(available, first);
            if (!clear && updates >= 4) available = std::max(available, first_update);
            if (available == deadline) return deadline;
            deadline = available;
        }
    }
    struct Attempt { double at; bool update; };
    static constexpr double window = 20.05;
    std::deque<Attempt> attempts_; // Capacity is at most five accepted attempts.
    nlohmann::json sent_ = nullptr;
    double last_attempt_ = -std::numeric_limits<double>::infinity(), acknowledged_at_{};
    bool acknowledged_{}, may_be_active_{};
};
}
