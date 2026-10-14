#pragma once
// OBS-independent, normalized source-space framing math.
#include <algorithm>
#include <cmath>

namespace autoframe {
inline float clamp(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }
struct Box {
    float x = 0, y = 0, w = 0, h = 0; // top-left and size, normalized 0..1
    float score = 0;
    bool valid() const { return w > 0 && h > 0; }
    float cx() const { return x + w * 0.5f; }
    float cy() const { return y + h * 0.5f; }
    float area() const { return w * h; }
};
struct Options {
    float subject_x = 0.50f;         // desired horizontal placement within output
    float subject_height = 0.70f;    // body height as fraction of output
    float headroom = 0.10f;          // fraction of output above body
    float max_zoom = 3.5f;
    float pan_smooth_seconds = 0.75f;
    float zoom_smooth_seconds = 1.20f;
    float dead_zone = 0.025f;       // source-frame fraction, scaled by zoom
    float max_pan_speed = 0.55f;    // normalized source frame distance / second
    float max_zoom_speed = 2.0f;    // zoom units / second
    float lead_seconds = 0.15f;    // look-ahead for walking motion
    bool return_to_wide = true;
};
struct Framing { float x = .5f, y = .5f, zoom = 1.f; };
struct Crop { float left, top, right, bottom; };
inline Crop crop(const Framing &f) {
    const float size = 1.f / std::max(1.f, f.zoom);
    const float x = clamp(f.x - size * 0.5f, 0.f, 1.f - size);
    const float y = clamp(f.y - size * 0.5f, 0.f, 1.f - size);
    return {x, y, x + size, y + size};
}
inline Framing target_for(const Box &b, const Options &o, float vx=0, float vy=0) {
    if (!b.valid()) return {};
    Framing f;
    f.zoom = clamp(o.subject_height / b.h, 1.f, std::max(1.f, o.max_zoom));
    const float crop_size = 1.f / f.zoom;
    const float body_output_height = b.h * f.zoom;
    // Centers body under the requested headroom. Works with zoom clamping.
    const float subject_y = o.headroom + body_output_height * .5f; // may exceed 1 for intentional waist-up crop
    f.x = b.cx() + vx * o.lead_seconds + (.5f - o.subject_x) * crop_size;
    f.y = b.cy() + vy * o.lead_seconds + (.5f - subject_y) * crop_size;
    f.x = clamp(f.x, crop_size * .5f, 1.f - crop_size * .5f);
    f.y = clamp(f.y, crop_size * .5f, 1.f - crop_size * .5f);
    return f;
}
inline float capped_approach(float now, float target, float alpha, float max_step) {
    return now + clamp((target - now) * alpha, -max_step, max_step);
}
inline Framing advance(Framing current, Framing desired, float seconds,
                       const Options &opt, bool tracking) {
    const float dt = clamp(seconds, 0.f, .25f);
    const float dead_zone = clamp(opt.dead_zone, 0.f, .30f) / std::max(1.f, current.zoom);
    if (tracking) {
        if (std::fabs(desired.x - current.x) < dead_zone) desired.x = current.x;
        if (std::fabs(desired.y - current.y) < dead_zone) desired.y = current.y;
    }
    const float pan_a = 1.f - std::exp(-dt / std::max(.02f, opt.pan_smooth_seconds));
    const float zoom_a = 1.f - std::exp(-dt / std::max(.02f, opt.zoom_smooth_seconds));
    current.x = capped_approach(current.x, desired.x, pan_a, std::max(0.01f, opt.max_pan_speed) * dt);
    current.y = capped_approach(current.y, desired.y, pan_a, std::max(0.01f, opt.max_pan_speed) * dt);
    current.zoom = capped_approach(current.zoom, desired.zoom, zoom_a,
                                  std::max(.1f, opt.max_zoom_speed) * dt);
    current.zoom = clamp(current.zoom, 1.f, std::max(1.f, opt.max_zoom));
    const float half = .5f / current.zoom;
    current.x = clamp(current.x, half, 1.f - half);
    current.y = clamp(current.y, half, 1.f - half);
    return current;
}
} // namespace autoframe