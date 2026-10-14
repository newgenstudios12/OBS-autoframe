/*
 * OBS AutoFrame v0.2.0 - experimental, native, full-body tracking filter.
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Rendering architecture informed by norihiro/obs-face-tracker (GPL-2.0).
 * This is a separate implementation; no dependency on the Face Tracker plugin.
 */
#include <obs-module.h>
#include <util/bmem.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <graphics/vec2.h>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect.hpp>
#include <opencv2/dnn.hpp>
#include "framing.hpp"
#include "people.hpp"
#include "selection.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

OBS_DECLARE_MODULE()
using Clock = std::chrono::steady_clock;

struct FrameJob {
    int width=0, height=0;
    std::vector<uint8_t> pixels; // tightly packed BGRA, copied from OBS GPU
};
struct Filter {
    obs_source_t *source=nullptr;
    gs_texrender_t *render=nullptr, *sample=nullptr;
    gs_stagesurf_t *stage=nullptr;
    std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    bool terminate=false, job_waiting=false;
    FrameJob job;
    autoframe::Options options;
    autoframe::Framing view;
    std::string model_path;
    float min_confidence=.40f, detection_fps=5.f, hold_seconds=2.f;
    bool enabled=true, freeze=false, debug_boxes=false;
    std::string mode="auto";
    int lock_ordinal=1, locked_id=0;
    int active_target_id=0; // updated by worker, protected by mutex
    std::vector<autoframe::Track> tracks_view;
    autoframe::Box last_person;
    float last_vx=0, last_vy=0;
    Clock::time_point last_person_time=Clock::now()-std::chrono::hours(1);
    Clock::time_point last_capture=Clock::now()-std::chrono::hours(1);
};

class BodyDetector {
    cv::dnn::Net net;
    std::string loaded_path;
    bool has_onnx=false;
public:
    BodyDetector() = default;
    void configure(const std::string &path) {
        if (path==loaded_path) return;
        loaded_path=path;
        has_onnx=false;
        net=cv::dnn::Net();
        if (path.empty()) { blog(LOG_INFO,"[AutoFrame] Using OpenCV HOG full-body detection"); return; }
        try {
            net=cv::dnn::readNetFromONNX(path);
            if (net.empty()) throw std::runtime_error("readNetFromONNX returned empty network");
            net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
            net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
            has_onnx=true;
            blog(LOG_INFO,"[AutoFrame] Loaded YOLO ONNX body detector: %s",path.c_str());
        } catch (const std::exception &e) {
            blog(LOG_ERROR,"[AutoFrame] ONNX model failed (%s), using HOG fallback: %s",path.c_str(),e.what());
        }
    }
    std::vector<autoframe::Box> detect(const FrameJob &job, float threshold) {
        cv::Mat bgra(job.height,job.width,CV_8UC4,const_cast<uint8_t *>(job.pixels.data()));
        cv::Mat bgr;
        cv::cvtColor(bgra,bgr,cv::COLOR_BGRA2BGR);
        if (has_onnx) {
            try { return detect_yolo(bgr,threshold); }
            catch (const cv::Exception &e) {
                blog(LOG_WARNING,"[AutoFrame] YOLO inference failed, using HOG fallback: %s",e.what());
                has_onnx=false;
            }
        }
        return detect_hog(bgr);
    }
private:
    static std::vector<autoframe::Box> detect_hog(const cv::Mat &bgr) {
        // Construct HOG on this thread only. Works without an external model, but
        // typically misses seated/partially occluded people; ONNX is recommended.
        static thread_local cv::HOGDescriptor detector=[] {
            cv::HOGDescriptor h;
            h.setSVMDetector(cv::HOGDescriptor::getDefaultPeopleDetector());
            return h;
        }();
        std::vector<cv::Rect> raw;
        std::vector<double> weights;
        detector.detectMultiScale(bgr,raw,weights,0.,cv::Size(8,8),cv::Size(8,8),1.05,2.0,false);
        std::vector<autoframe::Box> results;
        for (size_t i=0;i<raw.size();i++) {
            const auto &r=raw[i];
            results.push_back({autoframe::clamp((float)r.x/bgr.cols,0,1),
                               autoframe::clamp((float)r.y/bgr.rows,0,1),
                               autoframe::clamp((float)r.width/bgr.cols,0,1),
                               autoframe::clamp((float)r.height/bgr.rows,0,1), .75f});
        }
        return results;
    }
    std::vector<autoframe::Box> detect_yolo(const cv::Mat &bgr,float threshold) {
        // Ultralytics YOLOv8/YOLO11 detection export: [1, 84, 8400], sometimes
        // transposed to [1, 8400, 84]. Only COCO class 0 ('person') is used.
        constexpr int S=640;
        float scale=std::min((float)S/bgr.cols,(float)S/bgr.rows);
        int rw=std::max(1,(int)std::round(bgr.cols*scale));
        int rh=std::max(1,(int)std::round(bgr.rows*scale));
        int padx=(S-rw)/2, pady=(S-rh)/2;
        cv::Mat resized,canvas(S,S,CV_8UC3,cv::Scalar(114,114,114));
        cv::resize(bgr,resized,cv::Size(rw,rh));
        resized.copyTo(canvas(cv::Rect(padx,pady,rw,rh)));
        cv::Mat blob=cv::dnn::blobFromImage(canvas,1.f/255.f,cv::Size(S,S),cv::Scalar(),true,false);
        net.setInput(blob);
        cv::Mat raw=net.forward();
        if (raw.dims!=3 || raw.size[0]!=1 || raw.type()!=CV_32F) {
            throw cv::Exception(cv::Error::StsError,"Expected YOLOv8/v11 raw [1,84,N] output; export with nms=False",
                                "detect_yolo",__FILE__,__LINE__);
        }
        bool channels_first=raw.size[1]<=128 && raw.size[2]>128;
        int channels=channels_first?raw.size[1]:raw.size[2];
        int n=channels_first?raw.size[2]:raw.size[1];
        if (channels<5 || channels>128 || n<1) {
            throw cv::Exception(cv::Error::StsError,"Unsupported model output dimensions", "detect_yolo",__FILE__,__LINE__);
        }
        const float *p=raw.ptr<float>();
        auto cell=[&](int index,int ch)->float {
            return channels_first?p[(size_t)ch*n+index]:p[(size_t)index*channels+ch];
        };
        std::vector<cv::Rect> boxes;
        std::vector<float> scores;
        for (int i=0;i<n;i++) {
            float person_conf=(channels==85)?cell(i,4)*cell(i,5):cell(i,4);
            if (person_conf<threshold) continue;
            const float cx=cell(i,0),cy=cell(i,1),w=cell(i,2),h=cell(i,3);
            float x=(cx-.5f*w-padx)/scale;
            float y=(cy-.5f*h-pady)/scale;
            float bw=w/scale, bh=h/scale;
            float x0=autoframe::clamp(x,0.f,(float)bgr.cols);
            float y0=autoframe::clamp(y,0.f,(float)bgr.rows);
            float x1=autoframe::clamp(x+bw,0.f,(float)bgr.cols);
            float y1=autoframe::clamp(y+bh,0.f,(float)bgr.rows);
            if(x1-x0<8 || y1-y0<12) continue;
            boxes.emplace_back((int)x0,(int)y0,(int)(x1-x0),(int)(y1-y0));
            scores.push_back(person_conf);
        }
        std::vector<int> keep;
        cv::dnn::NMSBoxes(boxes,scores,threshold,.45f,keep);
        std::vector<autoframe::Box> people;
        for (int k:keep) {
            const auto &b=boxes[k];
            people.push_back({(float)b.x/bgr.cols,(float)b.y/bgr.rows,
                              (float)b.width/bgr.cols,(float)b.height/bgr.rows,scores[k]});
        }
        return people;
    }
};


static void detector_thread(Filter *f) {
    BodyDetector detector;
    autoframe::PeopleTracker tracker;
    auto last=Clock::now();
    while (true) {
        FrameJob job;
        std::string model,mode;
        float confidence,hold;
        int locked_id,previous_id;
        Clock::time_point last_selected_time;
        {
            std::unique_lock<std::mutex> lock(f->mutex);
            f->wake.wait(lock,[&]{return f->terminate||f->job_waiting;});
            if(f->terminate) break;
            job=std::move(f->job);
            f->job_waiting=false;
            model=f->model_path; confidence=f->min_confidence;
            mode=f->mode; locked_id=f->locked_id;
            previous_id=f->active_target_id;
            last_selected_time=f->last_person_time;
            hold=f->hold_seconds;
        }
        try {
            detector.configure(model);
            auto people=detector.detect(job,confidence);
            const auto now=Clock::now();
            const float dt=std::chrono::duration<float>(now-last).count();
            last=now;
            tracker.update(people,dt);
            auto visible=tracker.visible();
            autoframe::Track chosen;
            bool acquired=false;
            if(mode=="auto" && previous_id!=0 &&
               std::chrono::duration<float>(now-last_selected_time).count()<hold) {
                // Stick to a briefly occluded subject; never jump to another
                // person just because the active speaker turns around.
                for(const auto &t:visible) if(t.id==previous_id) {chosen=t; acquired=true; break;}
            } else {
                acquired=autoframe::choose_subject(visible,mode,locked_id,previous_id,chosen);
            }
            {
                std::lock_guard<std::mutex> lock(f->mutex);
                f->tracks_view=tracker.tracks();
                if(acquired) {
                    f->last_person=chosen.box;
                    f->last_person_time=now;
                    f->last_vx=chosen.vx;
                    f->last_vy=chosen.vy;
                    f->active_target_id=chosen.id;
                } else if(mode!="auto" && mode!="locked") {
                    f->active_target_id=0;
                }
            }
        } catch (const cv::Exception &e) {
            blog(LOG_WARNING,"[AutoFrame] Detection error: %s",e.what());
        } catch (const std::exception &e) {
            blog(LOG_WARNING,"[AutoFrame] Detector error: %s",e.what());
        }
    }
}

static const char* filter_name(void*) {return "AutoFrame (Full-Body Tracker) [Preview]";}
static void update(void *data,obs_data_t *s) {
    auto *f=static_cast<Filter *>(data);
    std::lock_guard<std::mutex> lock(f->mutex);
    f->enabled=obs_data_get_bool(s,"enabled");
    f->freeze=obs_data_get_bool(s,"freeze");
    f->model_path=obs_data_get_string(s,"onnx_path");
    f->min_confidence=(float)obs_data_get_double(s,"min_confidence");
    std::string next_mode=obs_data_get_string(s,"follow_mode");
    int next_id=(int)obs_data_get_int(s,"locked_id");
    if(next_mode!=f->mode || next_id!=f->locked_id) {
        // Don't pan at the old speaker while the user selects a new speaker.
        f->last_person={};
        f->last_person_time=Clock::now()-std::chrono::hours(1);
        f->active_target_id=0;
    }
    f->mode=std::move(next_mode);
    f->lock_ordinal=(int)obs_data_get_int(s,"lock_ordinal");
    f->locked_id=next_id;
    f->options.subject_x=(float)obs_data_get_double(s,"subject_x")/100.f;
    f->options.subject_height=(float)obs_data_get_double(s,"subject_height")/100.f;
    f->options.headroom=(float)obs_data_get_double(s,"headroom")/100.f;
    f->options.max_zoom=(float)obs_data_get_double(s,"max_zoom");
    f->options.pan_smooth_seconds=(float)obs_data_get_double(s,"pan_smooth");
    f->options.zoom_smooth_seconds=(float)obs_data_get_double(s,"zoom_smooth");
    f->options.max_pan_speed=(float)obs_data_get_double(s,"max_pan_speed")/100.f;
    f->options.max_zoom_speed=(float)obs_data_get_double(s,"max_zoom_speed");
    f->options.dead_zone=(float)obs_data_get_double(s,"dead_zone")/100.f;
    f->options.lead_seconds=(float)obs_data_get_double(s,"lead_seconds");
    f->options.return_to_wide=obs_data_get_bool(s,"return_wide");
    f->hold_seconds=(float)obs_data_get_double(s,"hold_seconds");
    f->detection_fps=(float)obs_data_get_double(s,"detection_fps");
    f->debug_boxes=obs_data_get_bool(s,"debug_boxes");
}
static void defaults(obs_data_t*s) {
    obs_data_set_default_bool(s,"enabled",true);
    obs_data_set_default_bool(s,"freeze",false);
    obs_data_set_default_string(s,"onnx_path","");
    obs_data_set_default_double(s,"min_confidence",.4);
    obs_data_set_default_string(s,"follow_mode","auto");
    obs_data_set_default_int(s,"lock_ordinal",1);
    obs_data_set_default_int(s,"locked_id",0);
    obs_data_set_default_double(s,"subject_x",50);
    obs_data_set_default_double(s,"subject_height",70);
    obs_data_set_default_double(s,"headroom",10);
    obs_data_set_default_double(s,"max_zoom",3.5);
    obs_data_set_default_double(s,"pan_smooth",.75);
    obs_data_set_default_double(s,"zoom_smooth",1.2);
    obs_data_set_default_double(s,"max_pan_speed",55);
    obs_data_set_default_double(s,"max_zoom_speed",2.0);
    obs_data_set_default_double(s,"dead_zone",2.5);
    obs_data_set_default_double(s,"lead_seconds",.15);
    obs_data_set_default_bool(s,"return_wide",true);
    obs_data_set_default_double(s,"hold_seconds",2.0);
    obs_data_set_default_double(s,"detection_fps",5.0);
    obs_data_set_default_bool(s,"debug_boxes",false);
}
static bool reset_clicked(obs_properties_t*,obs_property_t*,void *data) {
    if(!data) return false;
    auto *f=static_cast<Filter *>(data);
    std::lock_guard<std::mutex> lock(f->mutex);
    f->view={};f->last_person={};f->last_person_time=Clock::now()-std::chrono::hours(1);
    f->active_target_id=0;
    return true;
}
static bool lock_clicked(obs_properties_t*,obs_property_t*,void *data) {
    if(!data) return false;
    auto *f=static_cast<Filter *>(data);
    std::vector<autoframe::Track> list;
    int index=1;
    {
        std::lock_guard<std::mutex> lock(f->mutex);
        for(const auto &tr:f->tracks_view) if(tr.visible) list.push_back(tr);
        index=f->lock_ordinal;
    }
    std::sort(list.begin(),list.end(),[](auto &a,auto &b){return a.box.cx()<b.box.cx();});
    if(index<=0||index>(int)list.size()) {
        blog(LOG_WARNING,"[AutoFrame] Cannot lock person %d; only %zu visible",index,list.size());
        return false;
    }
    int selected=list[index-1].id;
    obs_data_t *settings=obs_source_get_settings(f->source);
    obs_data_set_int(settings,"locked_id",selected);
    obs_data_set_string(settings,"follow_mode","locked");
    obs_source_update(f->source,settings);
    obs_data_release(settings);
    blog(LOG_INFO,"[AutoFrame] Locked person #%d (tracker ID %d)",index,selected);
    return true;
}
static bool unlock_clicked(obs_properties_t*,obs_property_t*,void *data) {
    if(!data) return false;
    auto *f=static_cast<Filter *>(data);
    obs_data_t *settings=obs_source_get_settings(f->source);
    obs_data_set_int(settings,"locked_id",0);
    obs_data_set_string(settings,"follow_mode","auto");
    obs_source_update(f->source,settings);
    obs_data_release(settings);
    return true;
}
static obs_properties_t *properties(void *data) {
    obs_properties_t *props=obs_properties_create();
    obs_properties_add_bool(props,"enabled","Enable body tracking");
    obs_properties_add_bool(props,"freeze","Freeze current crop");
    obs_property_t *model=obs_properties_add_path(props,"onnx_path","YOLOv8 / YOLO11 ONNX model (optional)",OBS_PATH_FILE,"ONNX model (*.onnx);;All files (*.*)",nullptr);
    (void)model;
    obs_properties_add_float_slider(props,"min_confidence","YOLO confidence",.1,.95,.05);
    obs_properties_add_float_slider(props,"detection_fps","Detection updates per second",1,15,1);
    auto *mode=obs_properties_add_list(props,"follow_mode","Follow subject",OBS_COMBO_TYPE_LIST,OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(mode,"Auto (keep current person)","auto");
    obs_property_list_add_string(mode,"Largest person","largest");
    obs_property_list_add_string(mode,"Leftmost person","left");
    obs_property_list_add_string(mode,"Person nearest center","center");
    obs_property_list_add_string(mode,"Rightmost person","right");
    obs_property_list_add_string(mode,"Locked selected person","locked");
    obs_properties_add_int(props,"lock_ordinal","Pick person # from left (current frame)",1,20,1);
    obs_properties_add_button(props,"lock_person","Lock this person now",lock_clicked);
    obs_properties_add_button(props,"unlock_person","Unlock: follow automatically",unlock_clicked);
    obs_properties_add_float_slider(props,"subject_x","Subject position left / right (%)",15,85,1);
    obs_properties_add_float_slider(props,"subject_height","Body size: 70% full / 140% waist / 220% close-up",25,240,5);
    obs_properties_add_float_slider(props,"headroom","Space above head (%)",0,35,1);
    obs_properties_add_float_slider(props,"max_zoom","Maximum digital zoom",1,8,.1);
    obs_properties_add_float_slider(props,"pan_smooth","Pan smoothing (seconds)",.05,3,.05);
    obs_properties_add_float_slider(props,"zoom_smooth","Zoom smoothing (seconds)",.05,4,.05);
    obs_properties_add_float_slider(props,"max_pan_speed","Maximum pan rate (% of source / second)",2,150,1);
    obs_properties_add_float_slider(props,"max_zoom_speed","Maximum zoom rate (x per second)",.2,6,.1);
    obs_properties_add_float_slider(props,"dead_zone","Ignore small movements (%)",0,15,.5);
    obs_properties_add_float_slider(props,"lead_seconds","Walking direction anticipation (seconds)",0,.7,.05);
    obs_properties_add_float_slider(props,"hold_seconds","Keep last crop on loss (seconds)",0,10,.25);
    obs_properties_add_bool(props,"return_wide","Zoom out to wide shot if subject disappears");
    obs_properties_add_bool(props,"debug_boxes","Show tracked body rectangles in output (debug)");
    obs_properties_add_button(props,"reset","Reset crop",reset_clicked);
    if (data) {
        auto *f=static_cast<Filter*>(data);
        std::string status;
        {
            std::lock_guard<std::mutex> lock(f->mutex);
            int n=0;for(const auto &t:f->tracks_view) if(t.visible) ++n;
            status="Detected people: "+std::to_string(n)+"  |  Tracking ID: "+
                   std::to_string(f->active_target_id)+"  |  Locked ID: "+std::to_string(f->locked_id)+
                   "\n(Status updates when filter properties are reopened.)";
        }
        obs_properties_add_text(props,"status",status.c_str(),OBS_TEXT_INFO);
    }
    return props;
}
static void* create(obs_data_t *settings,obs_source_t *source) {
    auto *f=new Filter();
    f->source=source;
    update(f,settings);
    f->worker=std::thread(detector_thread,f);
    blog(LOG_INFO,"[AutoFrame] Native full-body filter loaded v0.2 (HOG/ONNX)");
    return f;
}
static void destroy(void *data) {
    auto *f=static_cast<Filter*>(data);
    {
        std::lock_guard<std::mutex> lock(f->mutex);
        f->terminate=true;f->job_waiting=false;
    }
    f->wake.notify_all();
    if(f->worker.joinable()) f->worker.join();
    obs_enter_graphics();
    gs_stagesurface_destroy(f->stage);
    gs_texrender_destroy(f->sample);
    gs_texrender_destroy(f->render);
    obs_leave_graphics();
    delete f;
}
static void tick(void *data,float seconds) {
    auto *f=static_cast<Filter*>(data);
    std::lock_guard<std::mutex> lock(f->mutex);
    if(f->freeze) return;
    if(!f->enabled) {f->view=autoframe::advance(f->view,{},seconds,f->options,false);return;}
    float age=std::chrono::duration<float>(Clock::now()-f->last_person_time).count();
    if(f->last_person.valid() && age <= f->hold_seconds) {
        auto desired=autoframe::target_for(f->last_person,f->options,f->last_vx,f->last_vy);
        f->view=autoframe::advance(f->view,desired,seconds,f->options,true);
    } else if(f->options.return_to_wide) {
        f->view=autoframe::advance(f->view,{},seconds,f->options,false);
    }
}
static uint32_t width(void *data) {
    auto*f=static_cast<Filter*>(data);
    auto*source=obs_filter_get_target(f->source);
    return source?obs_source_get_base_width(source):0;
}
static uint32_t height(void *data) {
    auto*f=static_cast<Filter*>(data);
    auto*source=obs_filter_get_target(f->source);
    return source?obs_source_get_base_height(source):0;
}
static void draw_quad(float w,float h,const autoframe::Crop &uv) {
    gs_render_start(false);
    gs_vertex2f(0,0); gs_vertex2f(w,0); gs_vertex2f(0,h); gs_vertex2f(w,h);
    struct vec2 coord;
    vec2_set(&coord,uv.left,uv.top);gs_texcoord2v(&coord,0);
    vec2_set(&coord,uv.right,uv.top);gs_texcoord2v(&coord,0);
    vec2_set(&coord,uv.left,uv.bottom);gs_texcoord2v(&coord,0);
    vec2_set(&coord,uv.right,uv.bottom);gs_texcoord2v(&coord,0);
    gs_render_stop(GS_TRISTRIP);
}
static void capture(Filter*f,gs_texture_t*src,uint32_t w,uint32_t h,float fps) {
    if(!src||!w||!h) return;
    auto now=Clock::now();
    if(std::chrono::duration<float>(now-f->last_capture).count()<1.f/std::max(1.f,fps)) return;
    {
        std::lock_guard<std::mutex> lock(f->mutex);
        if(f->job_waiting || f->terminate) return;
    }
    f->last_capture=now;
    float ratio=std::min(1.f,640.f/std::max(w,h));
    uint32_t sw=std::max(1u,(uint32_t)std::lround(w*ratio));
    uint32_t sh=std::max(1u,(uint32_t)std::lround(h*ratio));
    if(!f->sample) f->sample=gs_texrender_create(GS_BGRA,GS_ZS_NONE);
    if(!f->sample) return;
    gs_texrender_reset(f->sample);
    bool rendered=false;
    if(gs_texrender_begin(f->sample,sw,sh)) {
        struct vec4 black;vec4_zero(&black);
        gs_clear(GS_CLEAR_COLOR,&black,0,0);
        gs_ortho(0,(float)sw,0,(float)sh,-100,100);
        gs_effect_t *effect=obs_get_base_effect(OBS_EFFECT_DEFAULT);
        if(effect) {
            gs_effect_set_texture(gs_effect_get_param_by_name(effect,"image"),src);
            while(gs_effect_loop(effect,"Draw")) gs_draw_sprite(src,0,sw,sh);
            rendered=true;
        }
        gs_texrender_end(f->sample);
    }
    if(!rendered) return;
    gs_texture_t*small=gs_texrender_get_texture(f->sample);
    if(!small) return;
    if(!f->stage||gs_stagesurface_get_width(f->stage)!=sw||gs_stagesurface_get_height(f->stage)!=sh) {
        gs_stagesurface_destroy(f->stage);
        f->stage=gs_stagesurface_create(sw,sh,GS_BGRA);
    }
    if(!f->stage) return;
    gs_stage_texture(f->stage,small);
    uint8_t *p=nullptr;
    uint32_t pitch=0;
    if(!gs_stagesurface_map(f->stage,&p,&pitch)) return;
    FrameJob job;
    job.width=(int)sw;job.height=(int)sh;
    job.pixels.resize((size_t)sw*sh*4);
    for(uint32_t y=0;y<sh;y++)
        std::memcpy(job.pixels.data()+(size_t)y*sw*4,p+(size_t)y*pitch,sw*4);
    gs_stagesurface_unmap(f->stage);
    {
        std::lock_guard<std::mutex> lock(f->mutex);
        if(!f->job_waiting&&!f->terminate) {
            f->job=std::move(job);f->job_waiting=true;f->wake.notify_one();
        }
    }
}
static void draw_debug_boxes(const std::vector<autoframe::Track>& tracks,int active,
                             const autoframe::Crop&uv,float w,float h) {
    gs_effect_t*effect=obs_get_base_effect(OBS_EFFECT_SOLID);
    if(!effect) return;
    const float cw=std::max(.001f,uv.right-uv.left);
    const float ch=std::max(.001f,uv.bottom-uv.top);
    for (const auto &t:tracks) {
        if(!t.visible) continue;
        const float x0=(t.box.x-uv.left)*w/cw, y0=(t.box.y-uv.top)*h/ch;
        const float x1=(t.box.x+t.box.w-uv.left)*w/cw;
        const float y1=(t.box.y+t.box.h-uv.top)*h/ch;
        gs_effect_set_color(gs_effect_get_param_by_name(effect,"color"),
                            t.id==active?0xFF00FF00:0xFF00A5FF);
        while(gs_effect_loop(effect,"Solid")) {
            gs_render_start(false);
            gs_vertex2f(x0,y0);gs_vertex2f(x1,y0);gs_vertex2f(x1,y1);
            gs_vertex2f(x0,y1);gs_vertex2f(x0,y0);
            gs_render_stop(GS_LINESTRIP);
        }
    }
}
static void render(void*data,gs_effect_t*) {
    auto*f=static_cast<Filter*>(data);
    obs_source_t *target=obs_filter_get_target(f->source);
    obs_source_t *parent=obs_filter_get_parent(f->source);
    if(!target||!parent) {obs_source_skip_video_filter(f->source);return;}
    uint32_t w=obs_source_get_base_width(target),h=obs_source_get_base_height(target);
    if(!w||!h){obs_source_skip_video_filter(f->source);return;}
    if(!f->render) f->render=gs_texrender_create(GS_RGBA,GS_ZS_NONE);
    if(!f->render){obs_source_skip_video_filter(f->source);return;}
    gs_texrender_reset(f->render);
    gs_blend_state_push();gs_blend_function(GS_BLEND_ONE,GS_BLEND_ZERO);
    bool rendered=false;
    if(gs_texrender_begin(f->render,w,h)) {
        struct vec4 black;vec4_zero(&black);
        gs_clear(GS_CLEAR_COLOR,&black,0,0);
        gs_ortho(0,(float)w,0,(float)h,-100,100);
        const uint32_t flags=obs_source_get_output_flags(target);
        if(target==parent && !(flags&OBS_SOURCE_CUSTOM_DRAW) && !(flags&OBS_SOURCE_ASYNC))
            obs_source_default_render(target);
        else obs_source_video_render(target);
        gs_texrender_end(f->render);
        rendered=true;
    }
    gs_blend_state_pop();
    if(!rendered){obs_source_skip_video_filter(f->source);return;}
    gs_texture_t *image=gs_texrender_get_texture(f->render);
    if(!image){obs_source_skip_video_filter(f->source);return;}
    autoframe::Framing view;
    std::vector<autoframe::Track> tracks;
    float fps;
    bool should_detect,debug;
    int active;
    {
        std::lock_guard<std::mutex> lock(f->mutex);
        view=f->view;
        fps=f->detection_fps;
        should_detect=f->enabled && !f->freeze;
        debug=f->debug_boxes;active=f->active_target_id;
        if(debug) tracks=f->tracks_view;
    }
    if(should_detect) capture(f,image,w,h,fps);
    gs_effect_t *effect=obs_get_base_effect(OBS_EFFECT_DEFAULT);
    if(!effect){obs_source_skip_video_filter(f->source);return;}
    gs_effect_set_texture(gs_effect_get_param_by_name(effect,"image"),image);
    const auto uv=autoframe::crop(view);
    while(gs_effect_loop(effect,"Draw")) draw_quad((float)w,(float)h,uv);
    if(debug) draw_debug_boxes(tracks,active,uv,(float)w,(float)h);
}
bool obs_module_load(void) {
    obs_source_info info={};
    info.id="obs_autoframe_body_filter";
    info.type=OBS_SOURCE_TYPE_FILTER;
    info.output_flags=OBS_SOURCE_VIDEO|OBS_SOURCE_CUSTOM_DRAW;
    info.get_name=filter_name;
    info.create=create;info.destroy=destroy;info.update=update;
    info.get_defaults=defaults;info.get_properties=properties;
    info.video_tick=tick;info.video_render=render;
    info.get_width=width;info.get_height=height;
    obs_register_source(&info);
    blog(LOG_INFO,"[AutoFrame] Full-body filter registered (v0.2.0)");
    return true;
}