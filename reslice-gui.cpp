// Minimal SDL2 + ImGui + aubio GUI.
// Tracker-aligned, always-tall rows: 16 rows per bar (default), rows stay ~36px tall (adjustable).
// Supports any phrase length: 64 rows for 4 bars, 10s at 140bpm gives 94 tracker rows, etc.
// Waveform, gridlines, row numbers, markers and play buttons all align vertically in a large scrollable area.

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
#include <mutex>
#include <atomic>
#include <aubio/aubio.h>

struct SliceMarker {
    float time; // seconds
    bool selected = false;
};

struct Waveform {
    std::vector<float> envelope;
    int samplesPerBlock = 128;
};

static int samplerate = 44100;
static std::vector<float> waveform;
static Waveform g_wf;
static std::vector<SliceMarker> markers;
static float detected_bpm = 140.f;
static int base_note = 36;
static std::string loaded_filename, base_no_ext;
static std::mutex markers_mtx;
static std::atomic<bool> detect_in_progress{false};

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
    int frames = len / (int)sizeof(float);
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

int seconds_to_ticks(float sec, float bpm, int ppqn) { return (int)std::lround(sec * (bpm / 60.f) * ppqn); }
float ticks_to_seconds(int ticks, float bpm, int ppqn) { return (ticks / (float)ppqn) * (60.0f / bpm); }
float quantize_time(float sec, float bpm, int ppqn) {
    if (bpm <= 0.0f) return sec;
    int ticks = (int)std::lround(sec * (bpm / 60.f) * ppqn);
    return ticks_to_seconds(ticks, bpm, ppqn);
}

bool load_wav_mono(const char *path) {
    SDL_AudioSpec spec; Uint8 *buf; Uint32 len;
    if (!SDL_LoadWAV(path, &spec, &buf, &len)) return false;
    samplerate = spec.freq;
    SDL_AudioCVT cvt; if (SDL_BuildAudioCVT(&cvt, spec.format, spec.channels, spec.freq, AUDIO_F32, spec.channels, spec.freq) < 0) { SDL_FreeWAV(buf); return false; }
    cvt.len = len; cvt.buf = (Uint8*)SDL_malloc(cvt.len * cvt.len_mult); if (!cvt.buf) { SDL_FreeWAV(buf); return false; }
    SDL_memcpy(cvt.buf, buf, len); if (SDL_ConvertAudio(&cvt) < 0) { SDL_free(cvt.buf); SDL_FreeWAV(buf); return false; }
    int channels = spec.channels;
    int frames = (cvt.len_cvt / (sizeof(float) * channels));
    waveform.resize(frames);
    float *fdata = (float*)cvt.buf;
    for (int i = 0; i < frames; ++i) {
        float accum = 0.f; for (int c = 0; c < channels; ++c) accum += fdata[i * channels + c];
        waveform[i] = accum / channels;
    }
    SDL_free(cvt.buf); SDL_FreeWAV(buf);
    loaded_filename = std::string(path);
    base_no_ext = loaded_filename;
    size_t p = base_no_ext.find_last_of("/\\");
    if (p != std::string::npos) base_no_ext = base_no_ext.substr(p + 1);
    size_t d = base_no_ext.find_last_of('.');
    if (d != std::string::npos) base_no_ext = base_no_ext.substr(0, d);
    g_wf.samplesPerBlock = 128;
    g_wf.envelope.clear();
    for (size_t pos = 0; pos < waveform.size(); pos += g_wf.samplesPerBlock) {
        float peak = 0.f;
        size_t end = std::min(pos + (size_t)g_wf.samplesPerBlock, waveform.size());
        for (size_t i = pos; i < end; ++i) { float a = std::fabs(waveform[i]); if (a > peak) peak = a; }
        g_wf.envelope.push_back(peak);
    }
    return true;
}

void detect_onsets_aubio(float bpm, int ppqn) {
    std::vector<SliceMarker> out;
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
            float _bpm = aubio_tempo_get_bpm(tempo);
            if (_bpm > 0) detected_bpm = _bpm;
        }
        aubio_onset_do(onset, buf, o_out);
        if (o_out->data[0] > 0) {
            float t = quantize_time(aubio_onset_get_last_s(onset), bpm, ppqn);
            out.push_back({t});
        }
    }
    del_fvec(o_out); del_fvec(t_out); del_fvec(buf); del_aubio_onset(onset); del_aubio_tempo(tempo);
    std::sort(out.begin(), out.end(), [](auto&a,auto&b){return a.time<b.time;});
    markers = std::move(out);
}

void draw_tracker_and_wave(float rowNumWidth, float laneWidth, float markerWidth, float panel_height, int rows_per_bar, float bpm) {
    if (waveform.empty() || g_wf.envelope.empty() || samplerate <= 0 || bpm <= 0.0f) {
        ImGui::TextUnformatted("No waveform loaded");
        return;
    }
    constexpr float row_height = 36.f;
    float seconds_per_bar = (60.f / bpm) * 4.0f;
    float audio_length = waveform.size() / float(samplerate);
    int total_rows = std::ceil(audio_length / (seconds_per_bar / rows_per_bar));
    float seconds_per_row = seconds_per_bar / rows_per_bar;

    ImGui::BeginChild("tracker_rows", ImVec2(rowNumWidth + laneWidth + markerWidth, panel_height),
        true, ImGuiWindowFlags_AlwaysVerticalScrollbar | ImGuiWindowFlags_HorizontalScrollbar);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float scrollY = ImGui::GetScrollY();

    // PLAY SEGMENT HIGHLIGHT (draw segment green rect for currently playing range)
    if (g_play.playing) {
        float start_sec = g_play.start / float(samplerate);
        float end_sec   = g_play.end   / float(samplerate);
        int start_row = int(start_sec / seconds_per_row);
        int end_row   = int(end_sec   / seconds_per_row);
        float y1 = origin.y + start_row * row_height - scrollY;
        float y2 = origin.y + end_row * row_height - scrollY;
        // Clamp for visibility
        float panel_bottom = origin.y + panel_height;
        y1 = std::max(y1, origin.y);
        y2 = std::min(y2, panel_bottom);
        if (y2 > y1)
            dl->AddRectFilled(
                ImVec2(origin.x + rowNumWidth, y1),
                ImVec2(origin.x + rowNumWidth + laneWidth, y2),
                IM_COL32(60,255,80,60));
    }

    // Markers thread lock
    std::lock_guard<std::mutex> lg(markers_mtx);

    for (int row = 0; row < total_rows; ++row) {
        float y = origin.y + row * row_height - scrollY;
        float row_start_sec = row * seconds_per_row;
        float row_end_sec = (row+1) * seconds_per_row;
        if (y + row_height < origin.y) continue;
        if (y > origin.y + panel_height) break;

        // Row bg
        ImU32 rowbg = row % 2 ? IM_COL32(38,38,46,220) : IM_COL32(32,32,38,240);
        dl->AddRectFilled(ImVec2(origin.x, y), ImVec2(origin.x + rowNumWidth + laneWidth + markerWidth, y+row_height), rowbg, 0.0f);

        // Row number
        char numb[12]; snprintf(numb, sizeof(numb), "%02d", row+1);
        dl->AddText(ImVec2(origin.x+10, y+10), IM_COL32(200,220,255,255), numb);

        // Gridline (bold if bar boundary, always at top of row)
        float grid_thickness = (row % rows_per_bar == 0) ? 2.5f : 1.2f;
        ImU32 grid_col = (row % rows_per_bar == 0) ? IM_COL32(240,180,40,220) : IM_COL32(120,120,120,90);
        dl->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + rowNumWidth + laneWidth + markerWidth, y), grid_col, grid_thickness);

        // Waveform for this row
        float env_start_frac = row_start_sec / audio_length;
        float env_end_frac = row_end_sec / audio_length;
        int env_start_idx = int(env_start_frac * g_wf.envelope.size());
        int env_end_idx = std::min(int(env_end_frac * g_wf.envelope.size()), int(g_wf.envelope.size()));
        float lane_center = origin.x + rowNumWidth + laneWidth / 2.f;
        float wf_y1 = y + 3, wf_y2 = y + row_height - 4;
        int bars = env_end_idx - env_start_idx;
        for (int e = 0; e < bars; ++e) {
            float amp = g_wf.envelope[env_start_idx + e];
            float half = amp * (laneWidth / 2.f);
            float frac = bars > 1 ? e / float(bars - 1) : 0.5f;
            float cy = wf_y1 + frac * (wf_y2 - wf_y1);
            dl->AddLine(
                ImVec2(lane_center - half, cy), ImVec2(lane_center + half, cy), IM_COL32(70,220,255,205), 1.6f);
        }

        // Marker in this row? (marker at TOP of row, not center!)
        auto marker_it = std::find_if(markers.begin(), markers.end(), [&](const SliceMarker& m) {
            float mt = m.time;
            return mt >= row_start_sec && mt < row_end_sec;
        });
        if (marker_it != markers.end()) {
            // Vertical position at TOP of row
            float marker_y = y;
            dl->AddLine(
                ImVec2(origin.x + rowNumWidth, marker_y), ImVec2(origin.x + rowNumWidth + laneWidth, marker_y),
                IM_COL32(255,70,40,255), 3.5f);

            // Marker info: note name & [PLAY]
            float labelx = origin.x + rowNumWidth + laneWidth + 24.f;
            float labely = marker_y + 5.f;
            size_t mi = std::distance(markers.begin(), marker_it);
            int note = base_note + int(mi);
            static const char* kNoteNames[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
            int pc = (note%12+12)%12, oct = note/12 - 1;
            char nbuf[16]; snprintf(nbuf, sizeof(nbuf), "%s%d", kNoteNames[pc], oct);

            dl->AddText(ImVec2(labelx, labely), IM_COL32(230,230,230,255), nbuf);

            ImGui::SetCursorScreenPos(ImVec2(labelx + 60.f, labely - 3));
            ImGui::PushID(int(row));
            float t0 = marker_it->time;
            float t1 = (mi + 1 < markers.size()) ? markers[mi+1].time : audio_length;
            if (ImGui::SmallButton("PLAY")) play_segment_seconds(t0, t1);
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

int main(int argc, char **argv) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) return 1;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_Window *window = SDL_CreateWindow("Reslice GUI", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1680, 1200, SDL_WINDOW_OPENGL|SDL_WINDOW_RESIZABLE);
    SDL_GLContext glctx = SDL_GL_CreateContext(window);
    SDL_GL_SetSwapInterval(1);
    IMGUI_CHECKVERSION(); ImGui::CreateContext(); ImGuiIO &io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOpenGL(window, glctx);
    ImGui_ImplOpenGL2_Init();

    static int rows_per_bar = 16;
    if (argc > 1 && load_wav_mono(argv[1])) {
        audio_reopen_for_current();
        detect_onsets_aubio(detected_bpm, 24);
    }

    bool running = true; while (running) {
        SDL_Event e; while (SDL_PollEvent(&e)) { ImGui_ImplSDL2_ProcessEvent(&e); if (e.type == SDL_QUIT) running=false; }
        ImGui_ImplOpenGL2_NewFrame(); ImGui_ImplSDL2_NewFrame(); ImGui::NewFrame();
        ImGuiViewport* vp = ImGui::GetMainViewport(); ImVec2 pos = vp->Pos; ImVec2 siz = vp->Size;
        ImGuiWindowFlags pf = ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoBringToFrontOnFocus|ImGuiWindowFlags_NoNavFocus;
        static float rowNumWidth = 70.f, laneWidth = 480.f, markerWidth = 340.f;
        float leftW = 350.f, centerW = rowNumWidth + laneWidth + markerWidth + 60.f;
        float center_h = siz.y - 16.f;

        // Left panel
        ImGui::SetNextWindowPos(pos); ImGui::SetNextWindowSize(ImVec2(leftW, siz.y));
        ImGui::Begin("LeftPanel", nullptr, pf);
        ImGui::Text("File: %s", loaded_filename.c_str());
        ImGui::SliderInt("Base Note", &base_note, 0, 100);
        ImGui::SliderFloat("BPM", &detected_bpm, 40.f, 240.f, "%.1f");
        ImGui::SliderInt("Rows/bar", &rows_per_bar, 1, 32);
        ImGui::SliderFloat("Row# Width", &rowNumWidth, 36.f, 160.f);
        ImGui::SliderFloat("Wave Lane Width", &laneWidth, 160.f, 700.f);
        ImGui::SliderFloat("Marker Area Width", &markerWidth, 100.f, 900.f);
        if (ImGui::Button("Auto Detect")) detect_onsets_aubio(detected_bpm, 24);
        ImGui::SameLine(); if (ImGui::Button("Clear")) markers.clear();
        if (ImGui::Button("Play All")) play_segment_seconds(0.f, waveform.size()/(float)samplerate);
        ImGui::SameLine(); if (ImGui::Button("Stop")) stop_playback();
        ImGui::End();

        // Main panel: tracker/rows/waveform/markers
        ImGui::SetNextWindowPos(ImVec2(pos.x + leftW, pos.y));
        ImGui::SetNextWindowSize(ImVec2(centerW, siz.y));
        ImGui::Begin("WavePanel", nullptr, pf);
        draw_tracker_and_wave(rowNumWidth, laneWidth, markerWidth, center_h, rows_per_bar, detected_bpm);
        ImGui::End();

        ImGui::Render(); glViewport(0,0,(int)io.DisplaySize.x,(int)io.DisplaySize.y); glClearColor(0.1f,0.1f,0.12f,1.f); glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }
    ImGui_ImplOpenGL2_Shutdown(); ImGui_ImplSDL2_Shutdown(); ImGui::DestroyContext();
    SDL_GL_DeleteContext(glctx); SDL_DestroyWindow(window); SDL_Quit();
    return 0;
}
