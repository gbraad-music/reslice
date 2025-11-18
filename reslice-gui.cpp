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

static int samplerate = 44100;
static std::vector<float> waveform; // mono
static std::vector<SliceMarker> markers;
static float detected_bpm = 120.f;
static int ppqn = 480;
static int base_note = 36;
static std::string loaded_filename;

int seconds_to_ticks(float sec, float bpm, int ppqn) { return (int)(sec * (bpm / 60.f) * ppqn); }

bool load_wav_mono(const char *path) {
    // Use SDL_LoadWAV for simplicity (PCM only). Convert to mono by averaging channels.
    SDL_AudioSpec spec; Uint8 *buf; Uint32 len;
    if (!SDL_LoadWAV(path, &spec, &buf, &len)) return false;
    samplerate = spec.freq;
    int channels = spec.channels;
    int bytes_per_sample = SDL_AUDIO_BITSIZE(spec.format) / 8;
    int frames = len / (channels * bytes_per_sample);
    waveform.resize(frames);
    for (int i = 0; i < frames; ++i) {
        float accum = 0.f;
        for (int c = 0; c < channels; ++c) {
            Uint8 *p = buf + (i * channels + c) * bytes_per_sample;
            int16_t s = 0;
            if (bytes_per_sample == 2) s = *(int16_t*)p; // assume S16
            accum += s / 32768.f;
        }
        waveform[i] = accum / channels;
    }
    SDL_FreeWAV(buf);
    loaded_filename = std::string(path);
    return true;
}

void detect_onsets_aubio() {
    markers.clear();
    uint_t hop_size = 512; uint_t win_size = 1024;
    aubio_onset_t *onset = new_aubio_onset("default", win_size, hop_size, samplerate);
    aubio_tempo_t *tempo = new_aubio_tempo("default", win_size, hop_size, samplerate);
    fvec_t *buf = new_fvec(hop_size);
    int total_frames = waveform.size();
    for (int pos = 0; pos < total_frames; pos += hop_size) {
        int remain = std::min<int>(hop_size, total_frames - pos);
        for (int i = 0; i < hop_size; ++i) buf->data[i] = (i < remain) ? waveform[pos + i] : 0.f;
        aubio_tempo_do(tempo, buf, NULL);
        smpl_t tempo_time = aubio_tempo_get_last(tempo);
        if (tempo_time > 0) {
            float bpm = aubio_tempo_get_bpm(tempo);
            if (bpm > 0) detected_bpm = bpm; // take first valid
        }

        aubio_onset_do(onset, buf, NULL);
        smpl_t onset_time = aubio_onset_get_last(onset);
        if (onset_time > 0) {
            float t = aubio_onset_get_last_s(onset);
            markers.push_back({t});
        }
    }
    del_fvec(buf); del_aubio_onset(onset); del_aubio_tempo(tempo);
}

void export_sfz(const char *out_path) {
    std::ofstream f(out_path);
    f << "<group>\n";
    int total_frames = waveform.size();
    for (size_t i = 0; i < markers.size(); ++i) {
        int start = (int)(markers[i].time * samplerate);
        int end = (i + 1 < markers.size()) ? (int)(markers[i+1].time * samplerate) : total_frames;
        int note = base_note + (int)i;
        f << "<region> sample=" << loaded_filename << " key=" << note << " offset=" << start << " end=" << end << "\n";
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

void draw_waveform(float height) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    int frames = waveform.size();
    if (frames == 0) { ImGui::TextUnformatted("No waveform loaded"); return; }
    // simple subsampling
    int step = std::max(1, frames / (int)w);
    for (int x = 0; x < (int)w; ++x) {
        int idx = x * step; if (idx >= frames) break; float v = waveform[idx];
        float y = p0.y + height * 0.5f - v * height * 0.45f;
        dl->AddLine(ImVec2(p0.x + x, p0.y + height * 0.5f), ImVec2(p0.x + x, y), IM_COL32(100,200,255,255));
    }
    // markers
    for (size_t i=0;i<markers.size();++i){
        float t = markers[i].time; int idx = (int)(t * samplerate); float x = p0.x + (idx / (float)frames) * w;
        ImU32 col = markers[i].selected?IM_COL32(255,100,100,255):IM_COL32(255,255,0,200);
        dl->AddLine(ImVec2(x, p0.y), ImVec2(x, p0.y + height), col, 2.f);
    }
    ImGui::InvisibleButton("wave", ImVec2(w, height));
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(0)) {
        float mx = ImGui::GetIO().MousePos.x - p0.x; float t = (mx / w) * (frames / (float)samplerate);
        markers.push_back({t,true}); std::sort(markers.begin(), markers.end(),[](auto&a,auto&b){return a.time<b.time;});
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(1)) {
        float mx = ImGui::GetIO().MousePos.x - p0.x; float t = (mx / w) * (frames / (float)samplerate);
        // remove closest
        if (!markers.empty()) {
            auto it = std::min_element(markers.begin(), markers.end(),[&](auto&a,auto&b){return fabs(a.time - t) < fabs(b.time - t);});
            markers.erase(it);
        }
    }
}

int main(int argc, char **argv) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) return 1;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_Window *window = SDL_CreateWindow("Reslice GUI", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1200, 600, SDL_WINDOW_OPENGL|SDL_WINDOW_RESIZABLE);
    SDL_GLContext glctx = SDL_GL_CreateContext(window);
    SDL_GL_SetSwapInterval(1);
    IMGUI_CHECKVERSION(); ImGui::CreateContext(); ImGuiIO &io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOpenGL(window, glctx);
    ImGui_ImplOpenGL2_Init();

    if (argc > 1) { load_wav_mono(argv[1]); detect_onsets_aubio(); }

    bool running = true; while (running) {
        SDL_Event e; while (SDL_PollEvent(&e)) { ImGui_ImplSDL2_ProcessEvent(&e); if (e.type == SDL_QUIT) running=false; }
        ImGui_ImplOpenGL2_NewFrame(); ImGui_ImplSDL2_NewFrame(); ImGui::NewFrame();
        ImGui::Begin("Reslice");
        ImGui::Text("File: %s", loaded_filename.c_str());
        ImGui::SliderInt("Base Note", &base_note, 0, 100);
        ImGui::SliderFloat("Detected BPM", &detected_bpm, 40.f, 240.f);
        if (ImGui::Button("Open WAV")) {
            // Placeholder: integrate file dialog if desired
        }
        ImGui::SameLine(); if (ImGui::Button("Auto Detect")) detect_onsets_aubio();
        ImGui::SameLine(); if (ImGui::Button("Clear")) markers.clear();
        ImGui::Separator();
        draw_waveform(200.f);
        ImGui::Separator();
        if (ImGui::Button("Export slices.sfz")) export_sfz("slices.sfz");
        ImGui::SameLine(); if (ImGui::Button("Export slices.mid")) export_midi("slices.mid");
        ImGui::Text("Markers: %zu", markers.size());
        for (size_t i=0;i<markers.size();++i) {
            ImGui::PushID((int)i);
            float t = markers[i].time; if (ImGui::DragFloat("time", &markers[i].time, 0.001f, 0.f, waveform.size()/(float)samplerate)) {
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
