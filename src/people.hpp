#pragma once
// Lightweight multi-person tracker. ID assignment is based on predicted center
// and intersection-over-union; no facial recognition or biometric re-ID.
#include "framing.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace autoframe {
struct Track {
    int id = 0;
    Box box;
    float vx = 0, vy = 0; // normalized source frame per second
    float missing_seconds = 0;
    bool visible = true;
};
inline float iou(const Box &a, const Box &b) {
    const float l = std::max(a.x, b.x), t = std::max(a.y, b.y);
    const float r = std::min(a.x + a.w, b.x + b.w);
    const float bot = std::min(a.y + a.h, b.y + b.h);
    const float intersection = std::max(0.f, r-l) * std::max(0.f, bot-t);
    return intersection / std::max(0.0001f, a.area() + b.area() - intersection);
}
class PeopleTracker {
    int next_id_ = 1;
    std::vector<Track> tracks_;
public:
    const std::vector<Track> &tracks() const { return tracks_; }
    void reset() { tracks_.clear(); next_id_ = 1; }
    void update(const std::vector<Box>& detections, float elapsed) {
        const float dt = clamp(elapsed, 0.015f, 0.5f);
        // Only one detection per track, minimizing combined distance and shape cost.
        struct Match {int ti, di; float cost;};
        std::vector<Match> matches;
        for (int t = 0; t < (int)tracks_.size(); ++t) {
            const auto &tr = tracks_[t];
            for (int d = 0; d < (int)detections.size(); ++d) {
                const Box &b = detections[d];
                const float predx = tr.box.cx() + tr.vx * dt;
                const float predy = tr.box.cy() + tr.vy * dt;
                const float dist = std::hypot(predx - b.cx(), predy - b.cy());
                const float overlap = iou(tr.box, b);
                const float gate = .14f + .5f * std::max(tr.box.w, tr.box.h) + tr.missing_seconds * .15f;
                if (dist > gate && overlap < .08f) continue;
                const float scale_penalty = .12f * std::fabs(std::log(std::max(.001f, b.area()) /
                                                              std::max(.001f, tr.box.area())));
                matches.push_back({t, d, .92f * dist + .08f * (1.f - overlap) + scale_penalty});
            }
        }
        std::sort(matches.begin(), matches.end(), [](const Match &a, const Match &b){return a.cost < b.cost;});
        std::vector<bool> assigned_tracks(tracks_.size(), false), assigned_dets(detections.size(), false);
        for (const auto &m : matches) {
            if (assigned_tracks[m.ti] || assigned_dets[m.di]) continue;
            auto &tr = tracks_[m.ti];
            const Box &b = detections[m.di];
            const float nvx = (b.cx() - tr.box.cx()) / dt;
            const float nvy = (b.cy() - tr.box.cy()) / dt;
            tr.vx = .70f * tr.vx + .30f * nvx;
            tr.vy = .70f * tr.vy + .30f * nvy;
            // Mild detector-box smoothing without delaying obvious body motion.
            tr.box.x = .25f * tr.box.x + .75f * b.x;
            tr.box.y = .25f * tr.box.y + .75f * b.y;
            tr.box.w = .25f * tr.box.w + .75f * b.w;
            tr.box.h = .25f * tr.box.h + .75f * b.h;
            tr.box.score = b.score;
            tr.missing_seconds = 0;
            tr.visible = true;
            assigned_tracks[m.ti] = true;
            assigned_dets[m.di] = true;
        }
        for (int t = 0; t < (int)tracks_.size(); ++t) {
            if (!assigned_tracks[t]) {
                tracks_[t].missing_seconds += dt;
                tracks_[t].visible = false;
            }
        }
        for (int d = 0; d < (int)detections.size(); ++d) {
            if (!assigned_dets[d] && detections[d].valid())
                tracks_.push_back({next_id_++, detections[d], 0, 0, 0, true});
        }
        // ID retention up to 3 seconds during brief occlusions.
        tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
                        [](const Track &t){return t.missing_seconds > 3.f;}), tracks_.end());
    }
    std::vector<Track> visible() const {
        std::vector<Track> out;
        for (auto &t : tracks_) if (t.visible) out.push_back(t);
        return out;
    }
};
} // namespace autoframe