#pragma once
#include "people.hpp"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
namespace autoframe {
inline bool choose_subject(const std::vector<Track>& tracks, const std::string& mode,
                           int locked_id, int previous_id, Track &out) {
    std::vector<Track> candidates;
    for(const auto &t:tracks) if(t.visible) candidates.push_back(t);
    if(candidates.empty()) return false;
    if(mode=="locked") {
        for(const auto &t:candidates) if(t.id==locked_id) {out=t; return true;}
        return false;
    }
    if(mode=="auto") {
        for(const auto &t:candidates) if(t.id==previous_id) {out=t; return true;}
    }
    if(mode=="left")
        out=*std::min_element(candidates.begin(),candidates.end(),[](auto &a,auto &b){return a.box.cx()<b.box.cx();});
    else if(mode=="right")
        out=*std::max_element(candidates.begin(),candidates.end(),[](auto &a,auto &b){return a.box.cx()<b.box.cx();});
    else if(mode=="center")
        out=*std::min_element(candidates.begin(),candidates.end(),[](auto &a,auto &b){return std::abs(a.box.cx()-.5f)<std::abs(b.box.cx()-.5f);});
    else
        out=*std::max_element(candidates.begin(),candidates.end(),[](auto &a,auto &b){return a.box.area()<b.box.area();});
    return true;
}
} // namespace autoframe