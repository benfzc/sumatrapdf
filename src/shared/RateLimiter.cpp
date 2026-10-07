/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#include "base/Base.h"

#include "RateLimiter.h"

constexpr i64 kMinuteMs = 60 * 1000;
constexpr i64 kHourMs = 60 * kMinuteMs;

// cooldown after the first push-back; doubles on each consecutive one
constexpr i64 kFirstCooldownMs = 30 * 1000;
// push-backs in a row tolerated before pausing (the 4th one pauses), so the
// longest automatic cooldown is 30s * 2^3 = 4 min
constexpr int kMaxBlockedInRow = 3;

// forget sends that left the hour window
static void DropExpired(RateLimiter* rl, i64 nowMs) {
    int n = 0;
    while (n < len(rl->sentMs) && rl->sentMs[n] <= nowMs - kHourMs) {
        n++;
    }
    if (n > 0) {
        VecRemoveAtN(rl->sentMs, 0, n);
    }
}

static RateCheck WaitFor(i64 waitMs) {
    RateCheck res;
    res.state = RateState::Wait;
    res.waitMs = waitMs;
    return res;
}

RateCheck RateLimiterCheck(RateLimiter* rl, i64 nowMs) {
    if (rl->nBlocked > kMaxBlockedInRow) {
        RateCheck res;
        res.state = RateState::Paused;
        return res;
    }

    if (nowMs < rl->cooldownUntilMs) {
        return WaitFor(rl->cooldownUntilMs - nowMs);
    }

    DropExpired(rl, nowMs);
    int nSent = len(rl->sentMs);

    // hour window full: wait until its oldest send drops out
    if (rl->perHour > 0 && nSent >= rl->perHour) {
        i64 oldest = rl->sentMs[nSent - rl->perHour];
        return WaitFor(oldest + kHourMs - nowMs);
    }

    // minute window full: e.g. perMinute=20 -> look at the 20th most recent send
    if (rl->perMinute > 0 && nSent >= rl->perMinute) {
        i64 nth = rl->sentMs[nSent - rl->perMinute];
        if (nth > nowMs - kMinuteMs) {
            return WaitFor(nth + kMinuteMs - nowMs);
        }
    }

    return RateCheck{};
}

void RateLimiterSent(RateLimiter* rl, i64 nowMs) {
    VecAppend(rl->sentMs, nowMs);
}

void RateLimiterBlocked(RateLimiter* rl, i64 nowMs, i64 retryAfterMs) {
    // 30s, 60s, 120s, 240s
    i64 cooldown = kFirstCooldownMs;
    for (int i = 0; i < rl->nBlocked && i < kMaxBlockedInRow; i++) {
        cooldown *= 2;
    }
    if (retryAfterMs > cooldown) {
        cooldown = retryAfterMs;
    }

    rl->nBlocked++;
    rl->cooldownUntilMs = nowMs + cooldown;
}

void RateLimiterSucceeded(RateLimiter* rl) {
    rl->nBlocked = 0;
}

void RateLimiterResume(RateLimiter* rl) {
    rl->nBlocked = 0;
    rl->cooldownUntilMs = 0;
}
