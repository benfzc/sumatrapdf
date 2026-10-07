/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"

#include "RateLimiter.h"

// must be last due to assert() over-write
#include "base/tests/UtAssert.h"

constexpr i64 kSecMs = 1000;
constexpr i64 kMinMs = 60 * kSecMs;

static bool IsReady(RateLimiter* rl, i64 nowMs) {
    return RateLimiterCheck(rl, nowMs).state == RateState::Ready;
}

static void MinuteWindowTest() {
    RateLimiter rl;
    rl.perMinute = 3;
    rl.perHour = 100;

    i64 t = 1000 * kMinMs;
    for (int i = 0; i < 3; i++) {
        utassert(IsReady(&rl, t + i * kSecMs));
        RateLimiterSent(&rl, t + i * kSecMs);
    }

    // 4th request within the same minute waits for the 1st to age out
    RateCheck c = RateLimiterCheck(&rl, t + 10 * kSecMs);
    utassert(c.state == RateState::Wait);
    utassert(c.waitMs == 50 * kSecMs);

    utassert(IsReady(&rl, t + kMinMs + 1));
}

static void HourWindowTest() {
    RateLimiter rl;
    rl.perMinute = 0; // no minute cap
    rl.perHour = 2;

    i64 t = 1000 * kMinMs;
    RateLimiterSent(&rl, t);
    RateLimiterSent(&rl, t + 5 * kMinMs);

    RateCheck c = RateLimiterCheck(&rl, t + 10 * kMinMs);
    utassert(c.state == RateState::Wait);
    utassert(c.waitMs == 50 * kMinMs);

    // the 1st send leaves the hour window; expired entries get dropped
    utassert(IsReady(&rl, t + 60 * kMinMs));
    utassert(len(rl.sentMs) == 1);
}

static void BackoffTest() {
    RateLimiter rl;
    i64 t = 1000 * kMinMs;

    // 30s, 60s, 120s
    i64 expected[] = {30 * kSecMs, 60 * kSecMs, 120 * kSecMs};
    for (i64 cooldown : expected) {
        RateLimiterBlocked(&rl, t, 0);
        RateCheck c = RateLimiterCheck(&rl, t);
        utassert(c.state == RateState::Wait);
        utassert(c.waitMs == cooldown);
    }

    // success resets the backoff
    RateLimiterSucceeded(&rl);
    RateLimiterBlocked(&rl, t, 0);
    utassert(RateLimiterCheck(&rl, t).waitMs == 30 * kSecMs);
}

static void LongestCooldownTest() {
    RateLimiter rl;
    rl.nBlocked = 3; // 3 push-backs so far: 30s * 2^3
    RateLimiterBlocked(&rl, 0, 0);
    utassert(rl.cooldownUntilMs == 240 * kSecMs);

    // a Retry-After longer than our own cooldown is honored
    RateLimiter rl2;
    RateLimiterBlocked(&rl2, 0, 20 * kMinMs);
    utassert(rl2.cooldownUntilMs == 20 * kMinMs);
}

static void PauseTest() {
    RateLimiter rl;
    i64 t = 1000 * kMinMs;
    for (int i = 0; i < 4; i++) {
        RateLimiterBlocked(&rl, t, 0);
    }
    utassert(RateLimiterCheck(&rl, t + 60 * kMinMs).state == RateState::Paused);

    RateLimiterResume(&rl);
    utassert(IsReady(&rl, t));
}

void RateLimiter_UnitTests() {
    MinuteWindowTest();
    HourWindowTest();
    BackoffTest();
    LongestCooldownTest();
    PauseTest();
}
