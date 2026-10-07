/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

// Client-side request throttling for web services with undocumented quotas
// (e.g. the free Google Translate endpoint).
//
// Two sliding windows (per minute, per hour) cap how often we send. When the
// service pushes back (HTTP 429, captcha page) we cool down with exponential
// backoff; after too many consecutive push-backs we pause until the user asks
// to resume.
//
// Time is passed in by the caller (ms) so the logic is testable with a fake
// clock and has no OS dependency.
//
//   ready ──Sent──► ready ... window full ──► wait(until oldest leaves window)
//     │
//     └─Blocked──► wait(30s) ─Blocked─► wait(60s) ... ─4th Blocked─► paused
//                      │                                              │
//                      └──────Succeeded──► ready ◄──────Resume────────┘

enum class RateState {
    Ready,  // a request may be sent now
    Wait,   // retry after RateCheck.waitMs
    Paused, // too many push-backs; only RateLimiterResume() unpauses
};

struct RateCheck {
    RateState state = RateState::Ready;
    i64 waitMs = 0;
};

struct RateLimiter {
    int perMinute = 20;
    int perHour = 300;

    // send times within the last hour, oldest first
    Vec<i64> sentMs;
    i64 cooldownUntilMs = 0;
    // consecutive push-backs from the service
    int nBlocked = 0;
};

RateCheck RateLimiterCheck(RateLimiter* rl, i64 nowMs);
void RateLimiterSent(RateLimiter* rl, i64 nowMs);
// retryAfterMs: the service's Retry-After hint, 0 if none
void RateLimiterBlocked(RateLimiter* rl, i64 nowMs, i64 retryAfterMs);
void RateLimiterSucceeded(RateLimiter* rl);
void RateLimiterResume(RateLimiter* rl);

#if IS_DEBUG
void RateLimiter_UnitTests();
#endif
