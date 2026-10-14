#include "framing.hpp"
#include "people.hpp"
#include "selection.hpp"
#include <cassert>
#include <cmath>
#include <iostream>
using namespace autoframe;
static bool near(float a,float b,float eps=.015f){return std::fabs(a-b)<eps;}
int main() {
    Options o;
    Box subject{.72f,.14f,.10f,.26f,.95f};
    auto desired=target_for(subject,o);
    assert(desired.zoom>1.f && desired.zoom<=o.max_zoom);
    assert(desired.x>=0 && desired.x<=1 && desired.y>=0 && desired.y<=1);
    auto c=crop(desired);
    assert(c.left>=0 && c.top>=0 && c.right<=1 && c.bottom<=1);
    assert(near(c.right-c.left,1.f/desired.zoom));
    auto f=Framing{};
    for(int i=0;i<250;i++) f=advance(f,desired,1.f/30.f,o,true);
    assert(near(f.zoom,desired.zoom));
    assert(near(f.x,desired.x));
    for(int i=0;i<250;i++) f=advance(f,Framing{},1.f/30.f,o,false);
    assert(near(f.zoom,1.f));
    // Every target and intermediate crop stays inside the original image.
    for(float x=0;x<=1.f;x+=.035f) for(float y=0;y<=1.f;y+=.035f) {
        auto target=target_for(Box{x,y,.1f,.15f,.85f},o);
        auto uv=crop(target);
        assert(uv.left>=0 && uv.top>=0 && uv.right<=1.00001f && uv.bottom<=1.00001f);
    }
    // Two people retain IDs when detection order reverses and positions shift.
    PeopleTracker tracker;
    tracker.update({Box{.10f,.10f,.15f,.6f,.9f},Box{.65f,.12f,.15f,.6f,.9f}},.15f);
    assert(tracker.visible().size()==2);
    int left_id=tracker.visible()[0].id, right_id=tracker.visible()[1].id;
    tracker.update({Box{.64f,.12f,.15f,.6f,.9f},Box{.12f,.10f,.15f,.6f,.9f}},.15f);
    assert(tracker.tracks().size()==2);
    assert(tracker.visible()[0].id==left_id && tracker.visible()[1].id==right_id);
    tracker.update({Box{.14f,.10f,.15f,.6f,.9f}},.15f);
    assert(tracker.visible().size()==1);
    tracker.update({Box{.63f,.12f,.15f,.6f,.9f},Box{.16f,.1f,.15f,.6f,.9f}},.15f);
    assert(tracker.visible().size()==2);
    for (auto &tr: tracker.visible()) assert(tr.id==left_id || tr.id==right_id);
    Track selected;
    assert(choose_subject(tracker.visible(), "left", 0, 0, selected));
    assert(selected.id == left_id);
    assert(choose_subject(tracker.visible(), "right", 0, 0, selected));
    assert(selected.id == right_id);
    assert(choose_subject(tracker.visible(), "locked", left_id, 0, selected));
    assert(selected.id == left_id);
    assert(!choose_subject(tracker.visible(), "locked", 9999, 0, selected));
    assert(choose_subject(tracker.visible(), "auto", 0, right_id, selected));
    assert(selected.id == right_id);
    Options close = o; close.subject_height = 2.20f; close.max_zoom=8.0f;
    auto tight = target_for(Box{.3f,.2f,.25f,.55f,.9f}, close);
    assert(tight.zoom > 3.f); // intentional torso crop, not full body
    auto tight_crop = crop(tight);
    assert(tight_crop.top >= 0 && tight_crop.bottom <= 1);
    // Two walking subjects crossing with detection order sorted left-to-right.
    // Their IDs should remain associated with their walking trajectories.
    PeopleTracker walkers;
    for(int step=0;step<10;step++) {
        float ax=.1f+step*.066f, bx=.74f-step*.066f;
        std::vector<Box> candidates={{ax,.12f,.09f,.42f,.9f},
                                     {bx,.12f,.09f,.42f,.9f}};
        std::sort(candidates.begin(),candidates.end(),
                  [](const Box &a,const Box &b){return a.cx()<b.cx();});
        walkers.update(candidates,.15f);
    }
    assert(walkers.visible().size()==2);
    assert(walkers.visible()[0].box.cx()>walkers.visible()[1].box.cx());
    // Moving left-to-right is followed with a smooth, bounded per-frame pan.
    f = { .5f, .5f, 2.f};
    Options speed = o; speed.max_pan_speed=.1f;
    auto next=advance(f,{.9f,.5f,2.f},1.f/30.f,speed,true);
    assert(next.x-f.x<=.1f/30.f+.0001f);
    std::cout << "Framing, motion constraints, multi-person track ID tests: PASS\n";
}