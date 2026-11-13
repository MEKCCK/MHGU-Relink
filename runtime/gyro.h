// Relative gyro turns. A new source or a sampling gap starts at the current aim.
#pragma once
#include <math.h>
#include <stdint.h>

typedef struct {
    uint64_t sample_ns;
    float yaw, pitch;
} GyroTurn;

static inline void gyro_sample(GyroTurn *turn, uint64_t sample_ns, float yaw_rate, float pitch_rate) {
    if (!isfinite(yaw_rate) || !isfinite(pitch_rate) || !sample_ns) return;
    uint64_t previous = turn->sample_ns;
    if (sample_ns <= previous) return;
    turn->sample_ns = sample_ns;
    if (!previous || sample_ns - previous > 50000000ull) {
        turn->yaw = turn->pitch = 0;
        return;
    }
    float dt = (float)(sample_ns - previous) * 1e-9f;
    if (fabsf(yaw_rate) >= .03f) turn->yaw += yaw_rate * dt;
    if (fabsf(pitch_rate) >= .03f) turn->pitch += pitch_rate * dt;
}
