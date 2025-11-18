// Minimal SDL2 + ImGui + aubio GUI to place onset markers and export slices.sfz and slices.mid
// NOTE: This is a scaffold. You must link against SDL2, aubio, and ImGui (and its SDL2/OpenGL backend).
// Build example (adjust include/library paths):
// g++ -std=c++17 reslice_gui.cpp -Iimgui -Iimgui/backends -lSDL2 -lGL -laubio -o reslice_gui
// plus imgui sources (imgui*.cpp, backends/imgui_impl_sdl2.cpp, backends/imgui_impl_opengl3.cpp)
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl2.h"
#include <SDL.h>
#include <SDL_opengl.h>
#include <vector>
#include <string>
#include <fstream>
#include <algorithm>
#include <cmath>
#include <aubio/aubio.h>

struct SliceMarker {
    float time; // seconds
    bool selected = false;
};

struct Waveform {
    std::vector<float> envelope; // normalized amplitude per block
    int samplesPerBlock = 512;
};

static int samplerate = 44100;
static std::vector<float> waveform; // mono
static Waveform g_wf;
static std::vector<SliceMarker> markers;
static float detected_bpm = 120.f;
static int ppqn = 480;
static int base_note = 36;
static std::string loaded_filename;
static std::string base_no_ext;

struct Playback {
    bool playing = false;
    size_t start = 0, end = 0, cursor = 0;
    float volume = 0.8f;
    SDL_AudioDeviceID dev = 0;
    SDL_AudioSpec spec{};
} g_play;

static void sdl_audio_cb(void* userdata, Uint8* stream, int len) {
    Playback* p = (Playback*)userdata;
    float* out = (float*)stream;
    int frames = len / (int)sizeof(float); // mono
    for (int i = 0; i < frames; ++i) {
        float sample = 0.f;
        if (p->playing && p->cursor < p->end && p->cursor < waveform.size()) {
            sample = waveform[p->cursor++] * p->volume;
            if (p->cursor >= p->end) p->playing = false;
        }
        out[i] = sample;
    }
}

static void audio_reopen_for_current() {
    if (g_play.dev) { SDL_CloseAudioDevice(g_play.dev); g_play.dev = 0; }
    SDL_AudioSpec want{}; want.freq = samplerate; want.format = AUDIO_F32; want.channels = 1; want.samples = 1024; want.callback = sdl_audio_cb; want.userdata = &g_play;
    g_play.dev = SDL_OpenAudioDevice(NULL, 0, &want, &g_play.spec, 0);
    if (g_play.dev) SDL_PauseAudioDevice(g_play.dev, 0);
}

static void play_segment_seconds(float start_s, float end_s) {
    size_t start = (size_t)(start_s * samplerate);
    size_t end = (size_t)(end_s * samplerate);
    if (end <= start) end = std::min(waveform.size(), start + (size_t)(0.5f * samplerate));
    g_play.start = start; g_play.cursor = start; g_play.end = std::min(end, waveform.size()); g_play.playing = true;
}

static void stop_playback() { g_play.playing = false; }

int seconds_to_ticks(float sec, float bpm, int ppqn) { return (int)(sec * (bpm / 60.f) * ppqn); }

bool load_wav_mono(const char *path) {
    SDL_AudioSpec spec; Uint8 *buf; Uint32 len;
    if (!SDL_LoadWAV(path, &spec, &buf, &len)) return false;
    samplerate = spec.freq;

    // Convert to float32 keeping channel count, then average to mono
    SDL_AudioCVT cvt; if (SDL_BuildAudioCVT(&cvt, spec.format, spec.channels, spec.freq, AUDIO_F32, spec.channels, spec.freq) < 0) { SDL_FreeWAV(buf); return false; }
    cvt.len = len; cvt.buf = (Uint8*)SDL_malloc(cvt.len * cvt.len_mult); if (!cvt.buf) { SDL_FreeWAV(buf); return false; }
    SDL_memcpy(cvt.buf, buf, len); if (SDL_ConvertAudio(&cvt) < 0) { SDL_free(cvt.buf); SDL_FreeWAV(buf); return false; }

    int channels = spec.channels;
    int frames = (cvt.len_cvt / (sizeof(float) * channels));
    waveform.resize(frames);
    float *fdata = (float*)cvt.buf;
    for (int i = 0; i < frames; ++i) {
        float accum = 0.f; for (int c = 0; c < channels; ++c) accum += fdata[i * channels + c];
        waveform[i] = accum / channels; // mono average
    }
    SDL_free(cvt.buf); SDL_FreeWAV(buf);

    loaded_filename = std::string(path);
    // derive base name without extension for output files
    base_no_ext = loaded_filename;
    {
        size_t p = base_no_ext.find_last_of("/\\");
        if (p != std::string::npos) base_no_ext = base_no_ext.substr(p + 1);
        size_t d = base_no_ext.find_last_of('.');
        if (d != std::string::npos) base_no_ext = base_no_ext.substr(0, d);
    }
    // compute vertical envelope
    g_wf.envelope.clear();
    g_wf.samplesPerBlock = 512;
    for (size_t pos = 0; pos < waveform.size(); pos += g_wf.samplesPerBlock) {
        float peak = 0.f;
        size_t end = std::min(pos + (size_t)g_wf.samplesPerBlock, waveform.size());
        for (size_t i = pos; i < end; ++i) { float a = std::fabs(waveform[i]); if (a > peak) peak = a; }
        g_wf.envelope.push_back(peak);
    }
    return true;
}

void detect_onsets_aubio() {
    markers.clear();
    uint_t hop_size = 512; uint_t win_size = 1024;
    aubio_onset_t *onset = new_aubio_onset("default", win_size, hop_size, samplerate);
    aubio_tempo_t *tempo = new_aubio_tempo("default", win_size, hop_size, samplerate);
    fvec_t *buf = new_fvec(hop_size);
    fvec_t *o_out = new_fvec(1);
    fvec_t *t_out = new_fvec(1);
    int total_frames = (int)waveform.size();
    for (int pos = 0; pos < total_frames; pos += (int)hop_size) {
        int remain = std::min<int>(hop_size, total_frames - pos);
        for (int i = 0; i < (int)hop_size; ++i) buf->data[i] = (i < remain) ? waveform[pos + i] : 0.f;
        aubio_tempo_do(tempo, buf, t_out);
        if (t_out->data[0] > 0) {
            float bpm = aubio_tempo_get_bpm(tempo);
            if (bpm > 0) detected_bpm = bpm;
        }
        aubio_onset_do(onset, buf, o_out);
        if (o_out->data[0] > 0) {
            float t = aubio_onset_get_last_s(onset);
            markers.push_back({t});
        }
    }
    del_fvec(o_out); del_fvec(t_out); del_fvec(buf); del_aubio_onset(onset); del_aubio_tempo(tempo);
}

void export_sfz(const char *out_path) {
    std::ofstream f(out_path);
    f << "<group>\n";
    int total_frames = waveform.size();
    std::string sample_name = loaded_filename;
    size_t pos = sample_name.find_last_of("/\\");
    if (pos != std::string::npos) sample_name = sample_name.substr(pos + 1);
    for (size_t i = 0; i < markers.size(); ++i) {
        int start = (int)(markers[i].time * samplerate);
        int end = (i + 1 < markers.size()) ? (int)(markers[i+1].time * samplerate) : total_frames;
        int note = base_note + (int)i;
        f << "<region> sample=" << sample_name << " key=" << note << " offset=" << start << " end=" << end << "\n";
    }
}

// Minimal type-0 MIDI writer (single track) with note on/off events.
void export_midi(const char *out_path) {
    struct Event { int tick; bool on; int note; }; std::vector<Event> ev;
    for (size_t i = 0; i < markers.size(); ++i) {
        int tick_on = seconds_to_ticks(markers[i].time, detected_bpm, ppqn);
        ev.push_back({tick_on, true, base_note + (int)i});
        if (i + 1 < markers.size()) {
            int tick_off = seconds_to_ticks(markers[i+1].time, detected_bpm, ppqn);
            ev.push_back({tick_off, false, base_note + (int)i});
        }
    }
    std::sort(ev.begin(), ev.end(), [](auto &a, auto &b){ return a.tick < b.tick || (a.tick==b.tick && !a.on); });
    std::ofstream f(out_path, std::ios::binary);
    // Header chunk MThd
    uint8_t header[] = {'M','T','h','d', 0,0,0,6, 0,0, 0,1, (uint8_t)(ppqn>>8), (uint8_t)(ppqn&0xFF)};
    f.write((char*)header, sizeof(header));
    // Prepare track data in memory
    std::vector<uint8_t> track;
    auto write_varlen=[&](int v){ unsigned char buf[4]; int idx=0; buf[idx++]=v&0x7F; while((v>>=7)) { for(int i=idx;i>0;--i) buf[i]=buf[i-1]; buf[0]=(v&0x7F)|0x80; ++idx; } for(int i=0;i<idx;++i) track.push_back(buf[i]); };
    int last_tick=0;
    // Set tempo meta
    int us_per_beat = (int)(60000000.0 / detected_bpm);
    track.push_back(0); track.push_back(0xFF); track.push_back(0x51); track.push_back(0x03);
    track.push_back((us_per_beat>>16)&0xFF); track.push_back((us_per_beat>>8)&0xFF); track.push_back(us_per_beat&0xFF);
    for (auto &e: ev) {
        int delta = e.tick - last_tick; if (delta < 0) delta = 0; write_varlen(delta);
        track.push_back(e.on ? 0x90 : 0x80); track.push_back(e.note & 0x7F); track.push_back(100); last_tick = e.tick;
    }
    // End of track
    track.push_back(0); track.push_back(0xFF); track.push_back(0x2F); track.push_back(0);
    // Track chunk header
    uint32_t sz = track.size(); uint8_t trkhdr[]={'M','T','r','k', (uint8_t)(sz>>24),(uint8_t)(sz>>16),(uint8_t)(sz>>8),(uint8_t)(sz)};
    f.write((char*)trkhdr, sizeof(trkhdr)); f.write((char*)track.data(), track.size());
}

void draw_waveform_vertical(const Waveform& wf, float laneWidth, float blockH, float height = 0.f) {
    if (wf.envelope.empty()) { ImGui::TextUnformatted("No waveform loaded"); return; }
    if (height <= 0) height = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("wave_lane", ImVec2(laneWidth + 16, height), true);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float childH = ImGui::GetWindowSize().y;
    float scrollY = ImGui::GetScrollY();
    size_t blocks = wf.envelope.size();
    float contentH = blocks * blockH;

    // Interactive area
    ImGui::InvisibleButton("lane_area", ImVec2(laneWidth, contentH));
    bool hovered = ImGui::IsItemHovered();

    // Visible range
    int start = std::max(0, (int)std::floor(scrollY / blockH) - 1);
    int end = std::min((int)blocks - 1, (int)std::ceil((scrollY + childH) / blockH) + 1);

    // Midline
    float cx = origin.x + laneWidth * 0.5f;
    dl->AddLine(ImVec2(cx, origin.y), ImVec2(cx, origin.y + childH), IM_COL32(120,120,120,160), 1.0f);

    // Envelope blocks centered on midline
    for (int i = start; i <= end; ++i) {
        float y_top = origin.y + i * blockH - scrollY;
        float amp = wf.envelope[i] * laneWidth;
        float half = amp * 0.5f;
        dl->AddRectFilled(ImVec2(cx - half, y_top), ImVec2(cx + half, y_top + blockH - 1), IM_COL32(100,200,255,255));
    }

    // Onset lines (horizontal across lane)
    for (auto &m : markers) {
        int bi = (int)std::floor((m.time * samplerate) / (float)wf.samplesPerBlock);
        float y = origin.y + bi * blockH - scrollY + blockH * 0.5f;
        ImU32 col = m.selected ? IM_COL32(255,100,100,255) : IM_COL32(255,255,0,220);
        dl->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + laneWidth, y), col, 2.f);
    }

    // Playhead line
    if (g_play.playing) {
        int bi = (int)std::floor((g_play.cursor) / (float)wf.samplesPerBlock);
        float y = origin.y + bi * blockH - scrollY + blockH * 0.5f;
        dl->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + laneWidth, y), IM_COL32(0,255,0,255), 2.0f);
    }

    // Mouse interactions
    if (hovered) {
        ImVec2 mp = ImGui::GetIO().MousePos;
        float y_in = mp.y - origin.y + scrollY;
        int bi = (int)std::floor(y_in / blockH);
        bi = std::max(0, std::min(bi, (int)blocks - 1));
        float t = (bi * wf.samplesPerBlock) / (float)samplerate;
        if (ImGui::IsMouseClicked(0)) { markers.push_back({t,true}); std::sort(markers.begin(), markers.end(),[](auto&a,auto&b){return a.time<b.time;}); }
        if (ImGui::IsMouseClicked(1) && !markers.empty()) {
            auto it = std::min_element(markers.begin(), markers.end(), [&](auto &a, auto &b){ return std::fabs(a.time - t) < std::fabs(b.time - t); });
            markers.erase(it);
        }
    }

    ImGui::EndChild();
}

int main(int argc, char **argv) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) return 1;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_Window *window = SDL_CreateWindow("Reslice GUI", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1200, 600, SDL_WINDOW_OPENGL|SDL_WINDOW_RESIZABLE);
    SDL_GLContext glctx = SDL_GL_CreateContext(window);
    SDL_GL_SetSwapInterval(1);
    IMGUI_CHECKVERSION(); ImGui::CreateContext(); ImGuiIO &io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOpenGL(window, glctx);
    ImGui_ImplOpenGL2_Init();

    if (argc > 1) { load_wav_mono(argv[1]); detect_onsets_aubio(); audio_reopen_for_current(); }

    bool running = true; while (running) {
        SDL_Event e; while (SDL_PollEvent(&e)) { ImGui_ImplSDL2_ProcessEvent(&e); if (e.type == SDL_QUIT) running=false; }
        ImGui_ImplOpenGL2_NewFrame(); ImGui_ImplSDL2_NewFrame(); ImGui::NewFrame();
        ImGuiViewport* vp = ImGui::GetMainViewport(); ImVec2 pos = vp->Pos; ImVec2 siz = vp->Size;
        ImGuiWindowFlags pf = ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoBringToFrontOnFocus|ImGuiWindowFlags_NoNavFocus;
        static float laneWidth = 160.f; static float blockH = 4.0f; float leftW=300.f, centerW=laneWidth+24.f;

        // Left panel
        ImGui::SetNextWindowPos(pos); ImGui::SetNextWindowSize(ImVec2(leftW, siz.y));
        ImGui::Begin("LeftPanel", nullptr, pf);
        ImGui::Text("File: %s", loaded_filename.c_str());
        ImGui::SliderInt("Base Note", &base_note, 0, 100);
        ImGui::SliderFloat("BPM", &detected_bpm, 40.f, 240.f);
        if (ImGui::Button("Auto Detect")) detect_onsets_aubio();
        ImGui::SameLine(); if (ImGui::Button("Clear")) markers.clear();
        ImGui::SliderFloat("Lane Width", &laneWidth, 40.f, 400.f);
        ImGui::SliderFloat("Block Height", &blockH, 1.0f, 12.0f);
        if (ImGui::Button("Play All")) play_segment_seconds(0.f, waveform.size()/(float)samplerate);
        ImGui::SameLine(); if (ImGui::Button("Stop")) stop_playback();
        ImGui::End();

        // Center panel
        ImGui::SetNextWindowPos(ImVec2(pos.x + leftW, pos.y)); ImGui::SetNextWindowSize(ImVec2(centerW, siz.y));
        ImGui::Begin("CenterPanel", nullptr, pf);
        draw_waveform_vertical(g_wf, laneWidth, blockH);
        ImGui::End();

        // Right panel
        ImGui::SetNextWindowPos(ImVec2(pos.x + leftW + centerW, pos.y)); ImGui::SetNextWindowSize(ImVec2(siz.x - leftW - centerW, siz.y));
        ImGui::Begin("RightPanel", nullptr, pf);
        std::string sfz = base_no_ext.empty() ? std::string("slices.sfz") : (base_no_ext + "-slices.sfz");
        std::string mid = base_no_ext.empty() ? std::string("slices.mid") : (base_no_ext + "-slices.mid");
        ImGui::Text("Output:\n%s\n%s", sfz.c_str(), mid.c_str());
        if (ImGui::Button("Confirm: Generate")) { export_sfz(sfz.c_str()); export_midi(mid.c_str()); }
        ImGui::Separator();
        ImGui::Text("Markers: %zu", markers.size());
        for (size_t i=0;i<markers.size();++i) {
            ImGui::PushID((int)i);
            float maxT = waveform.size()/(float)samplerate;
            if (ImGui::Button("Play")) {
                float st = markers[i].time; float en = (i + 1 < markers.size()) ? markers[i+1].time : (waveform.size()/(float)samplerate);
                play_segment_seconds(st, en);
            }
            ImGui::SameLine();
            if (ImGui::DragFloat("time", &markers[i].time, 0.001f, 0.f, maxT)) {
                markers[i].time = std::max(0.f, std::min(markers[i].time, maxT));
                std::sort(markers.begin(), markers.end(),[](auto&a,auto&b){return a.time<b.time;});
            }
            ImGui::PopID();
        }
        ImGui::End();
        ImGui::Render(); glViewport(0,0,(int)io.DisplaySize.x,(int)io.DisplaySize.y); glClearColor(0.1f,0.1f,0.12f,1.f); glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }
    ImGui_ImplOpenGL2_Shutdown(); ImGui_ImplSDL2_Shutdown(); ImGui::DestroyContext();
    SDL_GL_DeleteContext(glctx); SDL_DestroyWindow(window); SDL_Quit();
    return 0;
}
