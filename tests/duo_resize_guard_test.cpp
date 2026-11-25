#include "duo_resize_guard.h"

#include <cstdio>
#include <cstdlib>

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::fprintf(stderr, "CHECK failed (%s:%d): %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                                  \
        }                                                                                  \
    } while (false)

namespace {

using gotham::DuoResizeGuard;

void TestReapplyAfterRevertAndResetOnMatch() {
    DuoResizeGuard guard;
    bool hit_limit = false;

    // Initial Duo resize requested at t=0 (even timestamp 0 must honor cooldown).
    guard.NoteResizeRequested(0);
    CHECK(!guard.ShouldReapply(1920, 1080, 3840, 1080, 200, hit_limit));
    CHECK(!hit_limit);
    CHECK(guard.retry_count == 0);

    // Game adopts 3840x1080 -> state resets.
    CHECK(!guard.ShouldReapply(3840, 1080, 3840, 1080, 250, hit_limit));
    CHECK(!hit_limit);
    CHECK(!guard.last_resize_ms.has_value());

    // Seconds later, game reverts to 1920x1080 -> immediate reapply.
    CHECK(guard.ShouldReapply(1920, 1080, 3840, 1080, 25000, hit_limit));
    CHECK(!hit_limit);
    CHECK(guard.retry_count == 1);

    // During cooldown window, do not spam resize requests.
    CHECK(!guard.ShouldReapply(1920, 1080, 3840, 1080, 25999, hit_limit));
    CHECK(!hit_limit);
    CHECK(guard.retry_count == 1);

    // Once game settles back to 3840x1080, retry counter resets for future level loads.
    CHECK(!guard.ShouldReapply(3840, 1080, 3840, 1080, 26200, hit_limit));
    CHECK(guard.retry_count == 0);
}

void TestRetryLimitAndNonMonotonicClock() {
    DuoResizeGuard guard;
    bool hit_limit = false;

    guard.NoteResizeRequested(1000);

    // Non-monotonic timestamp resets cooldown anchor without underflow or spurious retry.
    CHECK(!guard.ShouldReapply(1920, 1080, 3840, 1080, 900, hit_limit));
    CHECK(guard.retry_count == 0);

    uint64_t t = 1900;
    for (int i = 1; i <= DuoResizeGuard::kMaxRetries; ++i) {
        CHECK(guard.ShouldReapply(1920, 1080, 3840, 1080, t, hit_limit));
        CHECK(!hit_limit);
        CHECK(guard.retry_count == i);
        t += DuoResizeGuard::kCooldownMs;
    }

    // Next check after max retries reports hit_limit once, then stays silent.
    CHECK(!guard.ShouldReapply(1920, 1080, 3840, 1080, t, hit_limit));
    CHECK(hit_limit);
    CHECK(!guard.ShouldReapply(1920, 1080, 3840, 1080, t + DuoResizeGuard::kCooldownMs, hit_limit));
    CHECK(!hit_limit);

    // Manual Reset() allows retries again.
    guard.Reset();
    CHECK(guard.ShouldReapply(1920, 1080, 3840, 1080, t + 2000, hit_limit));
    CHECK(guard.retry_count == 1);
}

void TestZeroDimensionsIgnored() {
    DuoResizeGuard guard;
    bool hit_limit = false;
    CHECK(!guard.ShouldReapply(0, 1080, 3840, 1080, 100, hit_limit));
    CHECK(!guard.ShouldReapply(1920, 0, 3840, 1080, 100, hit_limit));
    CHECK(!guard.ShouldReapply(1920, 1080, 0, 1080, 100, hit_limit));
    CHECK(guard.retry_count == 0);
}

}  // namespace

int main() {
    TestReapplyAfterRevertAndResetOnMatch();
    TestRetryLimitAndNonMonotonicClock();
    TestZeroDimensionsIgnored();
    std::printf("All duo_resize_guard_tests passed.\n");
    return 0;
}
