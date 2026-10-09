#include <amp/activity_schedule.hpp>
#include <cassert>

namespace {
using amp::ActivitySchedule;
using nlohmann::json;
json song(const char* title = "First", int start = 1000) {
    return {{"details", title}, {"state", "Artist"}, {"timestamps", {{"start", start}, {"end", start + 200}}}};
}
void send(ActivitySchedule& schedule, const json& activity, double now) {
    assert(schedule.due(activity, now));
    schedule.attempt(activity, now);
    schedule.acknowledge(activity, now);
}
void first_send_and_spacing() {
    ActivitySchedule schedule;
    assert(!schedule.may_be_active());
    send(schedule, song(), 100);
    assert(schedule.may_be_active());
    assert(!schedule.due(song("Second"), 100.99));
    assert(schedule.next(song("Second"), 100.2) == 101);
    send(schedule, song("Second"), 101);
    assert(schedule.next(song("Second"), 110) == 131);
    // No timer-driven delay after a long idle period.
    send(schedule, song("Third"), 200);
}
void burst_and_reserved_clear() {
    ActivitySchedule schedule;
    send(schedule, song("One"), 0);
    send(schedule, song("Two"), 1);
    send(schedule, song("Three"), 2);
    send(schedule, song("Four"), 3);
    assert(!schedule.due(song("Five"), 4));
    assert(schedule.next(song("Five"), 4) == 20.05);
    assert(!schedule.due(song("Five"), 20.049));
    // The fifth slot clears immediately, despite the previous update's spacing.
    assert(schedule.can_clear(3.01) && schedule.due(nullptr, 3.01));
    send(schedule, nullptr, 3.01);
    assert(!schedule.may_be_active());
    assert(!schedule.can_clear(3.02));
    assert(schedule.next(song("Five"), 4) == 20.05);
    send(schedule, song("Five"), 20.05);
    assert(schedule.next(song("Six"), 20.06) == 21.05);
    // A sixth total request cannot bypass the budget by clearing.
    assert(!schedule.can_clear(20.06));
    assert(schedule.next(nullptr, 20.06) == 21.05);
}
void failures_and_reconnect() {
    ActivitySchedule schedule;
    schedule.attempt(song(), 0); // No acknowledgement: request may still have arrived.
    assert(schedule.may_be_active());
    schedule.disconnected();
    assert(schedule.next(song(), .1) == 1);
    schedule.attempt(song(), 1); schedule.disconnected();
    schedule.attempt(song(), 2); schedule.disconnected();
    schedule.attempt(song(), 3); schedule.disconnected();
    assert(schedule.next(song(), 4) == 20.05);
    assert(schedule.may_be_active());
    schedule.attempt(nullptr, 3.1); // A failed clear does not prove inactivity.
    schedule.disconnected();
    assert(schedule.may_be_active() && !schedule.can_clear(4));
    assert(schedule.next(nullptr, 4) == 20.05);
    send(schedule, nullptr, 20.05);
    assert(!schedule.may_be_active());
    schedule.disconnected(); // Configuration/reconnect retains preceding clear's spacing.
    assert(schedule.next(song(), 20.1) == 21.05);
}
void stable_progress_and_coalescing() {
    ActivitySchedule schedule;
    auto first = song();
    send(schedule, first, 0);
    for (int second = 1; second < 30; ++second) {
        auto observed = first;
        observed["timestamps"]["start"] = 1001; // Normal media timestamp jitter.
        assert(!schedule.due(observed, second));
        assert(schedule.next(observed, second) == 30);
    }
    assert(schedule.due(first, 30));
    send(schedule, first, 30);
    assert(schedule.next(song("Skipped"), 30.2) == 31);
    assert(schedule.next(first, 30.3) == 60); // A transient change that reverted needs no send.
    assert(schedule.next(song("Latest"), 30.8) == 31);
    auto latest = song("Latest");
    latest["assets"] = {{"large_image", "https://example.com/cover.webp"}};
    send(schedule, latest, 31); // Only the latest song and artwork are acknowledged.
    assert(schedule.next(latest, 31.1) == 61);
    assert(schedule.next(first, 31.1) == 32);
    // A seek is material, unlike clock/progress jitter.
    auto seek = latest; seek["timestamps"]["start"] = 990;
    assert(schedule.next(seek, 31.1) == 32);
}
void heartbeat_and_total_clear_limit() {
    ActivitySchedule schedule;
    send(schedule, song(), 0);
    send(schedule, song(), 30);
    send(schedule, song("Two"), 31);
    send(schedule, song("Three"), 32);
    send(schedule, song("Four"), 33);
    assert(schedule.next(song("Five"), 34) == 50.05);
    assert(schedule.next(song("Four"), 34) == 63);
    send(schedule, song("Four"), 63);
    assert(schedule.next(song("Four"), 63.1) == 93); // No expired-deadline spin.

    ActivitySchedule clears;
    for (int request = 0; request < 5; ++request) {
        assert(clears.can_clear(request * .1));
        clears.attempt(nullptr, request * .1);
    }
    assert(!clears.can_clear(.5));
    assert(clears.next(nullptr, .5) == 20.05);
    bool rejected{};
    try { clears.attempt(nullptr, .5); } catch (const std::logic_error&) { rejected = true; }
    assert(rejected && !clears.can_clear(.5));
    assert(clears.can_clear(20.05));
}
}
void test_activity_schedule() {
    first_send_and_spacing();
    burst_and_reserved_clear();
    failures_and_reconnect();
    stable_progress_and_coalescing();
    heartbeat_and_total_clear_limit();
}
