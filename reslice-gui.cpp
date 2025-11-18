#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl2.h"
#include <SDL.h>
#include <SDL_opengl.h>
#include <array>
#include <vector>
#include <string>
#include <cmath>
#include <mutex>
#include <algorithm>
#include <aubio/aubio.h>

// ===============================================
// DATA STRUCTURES
// ===============================================
struct SliceMarker {
    float time; // time in seconds
};

struct SampleRegion {
    int start_sample, end_sample, midi_key;
};

// ===============================================
// MIDI NOTE HELPERS
// ===============================================
const char* get_note_name(int midi_key, char* buf, size_t bufsize) {
    static const char* kNoteNames[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    int pc = (midi_key % 12 + 12) % 12;
    int oct = midi_key / 12 - 1;
    snprintf(buf, bufsize, "%s%d", kNoteNames[pc], oct);
    return buf;
}

// ===============================================
// ROW CALCULATION HELPERS
// ===============================================
struct RowCalculator {
    int samples_per_row;
    float seconds_per_row;
    int total_rows;

    RowCalculator(float bpm, int rows_per_bar, int samplerate, int total_samples) {
        float seconds_per_bar = (60.f / bpm) * 4.0f;
        seconds_per_row = seconds_per_bar / rows_per_bar;
        samples_per_row = int(seconds_per_row * samplerate + 0.5f);
        total_rows = int((total_samples + samples_per_row - 1) / samples_per_row);
    }

    int sample_to_row(int sample) const {
        return sample / samples_per_row;
    }

    float row_to_seconds(int row) const {
        return row * seconds_per_row;
    }
};

// ===============================================
// REGION COMPUTATION
// ===============================================
std::vector<SampleRegion> compute_sample_regions(
    const std::vector<SliceMarker>& markers, int samplerate, int total_samples, int base_note = 36)
{
    std::vector<SampleRegion> regions;
    if (markers.empty()) {
        regions.push_back({0, total_samples, base_note});
        return regions;
    }
    for (size_t i = 0; i < markers.size(); ++i) {
        int s0 = int(markers[i].time * samplerate + 0.5f);
        int s1 = (i + 1 < markers.size()) ? int(markers[i + 1].time * samplerate + 0.5f) : total_samples;
        regions.push_back({s0, s1, base_note + int(i)});
    }
    return regions;
}

void print_regions_debug(const std::string& sample_name, const std::vector<SampleRegion>& regions,
                         const RowCalculator* rowCalc = nullptr) {
    for (const auto& reg : regions) {
        printf("<region> sample=%s key=%d offset=%d end=%d",
               sample_name.c_str(), reg.midi_key, reg.start_sample, reg.end_sample);
        if (rowCalc) {
            int start_row = rowCalc->sample_to_row(reg.start_sample);
            int end_row = rowCalc->sample_to_row(reg.end_sample - 1);
            char nbuf[16];
            get_note_name(reg.midi_key, nbuf, sizeof(nbuf));
            printf(" [%s: row %d-%d]", nbuf, start_row, end_row);
        }
        printf("\n");
    }
    fflush(stdout);
}

// ===============================================
// WAVEFORM AND AUDIO STATE
// ===============================================
struct Waveform {
    std::vector<float> envelope;              // amplitude envelope
    std::vector<std::array<float,3>> bands;   // low/mid/high energy per block
    int samplesPerBlock = 1024;
};

struct Playback {
    bool playing = false;
    size_t start = 0, end = 0, cursor = 0;
    float volume = 0.8f;
    SDL_AudioDeviceID dev = 0;
    SDL_AudioSpec spec{};
};

static int samplerate = 44100;
static std::vector<float> waveform;
static Waveform g_wf;
static std::vector<SliceMarker> markers;
static float detected_bpm = 140.f;
static int base_note = 36;
static std::string loaded_filename;
static std::mutex markers_mtx;
static Playback g_play;

void build_waveform_with_fft(const std::vector<float>& audio, int samplerate, Waveform& wf) {
    uint_t win_size = wf.samplesPerBlock;
    uint_t hop_size = win_size / 2;

    aubio_pvoc_t* pv = new_aubio_pvoc(win_size, hop_size);
    fvec_t* in = new_fvec(hop_size);
    cvec_t* fftgrain = new_cvec(win_size);

    size_t total_frames = audio.size();
    for (size_t pos = 0; pos < total_frames; pos += hop_size) {
        // fill input buffer
        for (uint_t i = 0; i < hop_size; i++) {
            in->data[i] = (pos+i < total_frames) ? audio[pos+i] : 0.f;
        }

        // run FFT
        aubio_pvoc_do(pv, in, fftgrain);

        float low=0, mid=0, high=0;
        for (uint_t k = 0; k < fftgrain->length; k++) {
            float freq = (float)k * samplerate / win_size;
            float mag = fftgrain->norm[k];
            if (freq < 200) low += mag;
            else if (freq < 2000) mid += mag;
            else high += mag;
        }

        // normalize bands
        float sum = low+mid+high + 1e-8f;
        wf.bands.push_back({low/sum, mid/sum, high/sum});

        // envelope (RMS)
        float rms = 0.f;
        for (uint_t i = 0; i < hop_size; i++) rms += in->data[i]*in->data[i];
        rms = sqrt(rms / hop_size);
        wf.envelope.push_back(rms);
    }

    del_aubio_pvoc(pv);
    del_fvec(in);
    del_cvec(fftgrain);
}

// ===============================================
// AUDIO PLAYBACK
// ===============================================
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
    if (g_play.dev) {
        SDL_CloseAudioDevice(g_play.dev);
        g_play.dev = 0;
    }
    SDL_AudioSpec want{};
    want.freq = samplerate;
    want.format = AUDIO_F32;
    want.channels = 1;
    want.samples = 1024;
    want.callback = sdl_audio_cb;
    want.userdata = &g_play;
    g_play.dev = SDL_OpenAudioDevice(NULL, 0, &want, &g_play.spec, 0);
    if (g_play.dev) SDL_PauseAudioDevice(g_play.dev, 0);
}

static void play_region(const SampleRegion& reg) {
    printf("[PLAY] region sample=%s key=%d offset=%d end=%d (length=%d)\n",
        loaded_filename.c_str(), reg.midi_key, reg.start_sample, reg.end_sample,
        reg.end_sample - reg.start_sample);
    fflush(stdout);
    g_play.start = reg.start_sample;
    g_play.cursor = reg.start_sample;
    g_play.end = reg.end_sample;
    g_play.playing = true;
}

static void stop_playback() {
    g_play.playing = false;
}

// ===============================================
// WAV FILE LOADING
// ===============================================
bool load_wav_mono(const char *path) {
    SDL_AudioSpec spec;
    Uint8 *buf;
    Uint32 len;
    if (!SDL_LoadWAV(path, &spec, &buf, &len)) return false;

    samplerate = spec.freq;
    SDL_AudioCVT cvt;
    if (SDL_BuildAudioCVT(&cvt, spec.format, spec.channels, spec.freq,
                          AUDIO_F32, spec.channels, spec.freq) < 0) {
        SDL_FreeWAV(buf);
        return false;
    }

    cvt.len = len;
    cvt.buf = (Uint8*)SDL_malloc(cvt.len * cvt.len_mult);
    if (!cvt.buf) {
        SDL_FreeWAV(buf);
        return false;
    }

    SDL_memcpy(cvt.buf, buf, len);
    if (SDL_ConvertAudio(&cvt) < 0) {
        SDL_free(cvt.buf);
        SDL_FreeWAV(buf);
        return false;
    }

    int channels = spec.channels;
    int frames = (cvt.len_cvt / (sizeof(float) * channels));
    waveform.resize(frames);
    float *fdata = (float*)cvt.buf;

    // Convert to mono
    for (int i = 0; i < frames; ++i) {
        float accum = 0.f;
        for (int c = 0; c < channels; ++c) {
            accum += fdata[i * channels + c];
        }
        waveform[i] = accum / channels;
    }

    SDL_free(cvt.buf);
    SDL_FreeWAV(buf);
    loaded_filename = std::string(path);

    // Build waveform envelope for visualization
    g_wf.samplesPerBlock = 1024; // FFT window size
    g_wf.envelope.clear();
    g_wf.bands.clear();

    build_waveform_with_fft(waveform, samplerate, g_wf);

    return true;
}

// ===============================================
// ONSET DETECTION
// ===============================================
void detect_onsets_aubio(float bpm, int ppqn, bool detect_bpm = false) {
    std::vector<SliceMarker> out;
    uint_t hop_size = 512;
    uint_t win_size = 1024;

    aubio_onset_t *onset = new_aubio_onset("default", win_size, hop_size, samplerate);
    aubio_tempo_t *tempo = new_aubio_tempo("default", win_size, hop_size, samplerate);
    fvec_t *buf = new_fvec(hop_size);
    fvec_t *o_out = new_fvec(1);
    fvec_t *t_out = new_fvec(1);

    int total_frames = (int)waveform.size();
    for (int pos = 0; pos < total_frames; pos += (int)hop_size) {
        int remain = std::min<int>(hop_size, total_frames - pos);
        for (int i = 0; i < (int)hop_size; ++i) {
            buf->data[i] = (i < remain) ? waveform[pos + i] : 0.f;
        }

        // Detect tempo only if requested
        if (detect_bpm) {
            aubio_tempo_do(tempo, buf, t_out);
            if (t_out->data[0] > 0) {
                float _bpm = aubio_tempo_get_bpm(tempo);
                if (_bpm > 0) detected_bpm = _bpm;
            }
        }

        // Detect onsets
        aubio_onset_do(onset, buf, o_out);
        if (o_out->data[0] > 0) {
            float t_frames = aubio_onset_get_last(onset);
            float t = t_frames / (float)samplerate;
            out.push_back({t});
        }
    }

    del_fvec(o_out);
    del_fvec(t_out);
    del_fvec(buf);
    del_aubio_onset(onset);
    del_aubio_tempo(tempo);

    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.time < b.time; });
    
    // Quantize onsets to BPM grid
    float seconds_per_beat = 60.0f / bpm;
    float seconds_per_bar = seconds_per_beat * 4.0f;
    float quantize_unit = seconds_per_bar / ppqn;
    
    for (auto& marker : out) {
        float grid_pos = marker.time / quantize_unit;
        marker.time = round(grid_pos) * quantize_unit;
    }
    
    // Remove duplicates after quantization
    auto last = std::unique(out.begin(), out.end(), [](const SliceMarker& a, const SliceMarker& b) {
        return fabs(a.time - b.time) < 0.001f;
    });
    out.erase(last, out.end());
    
    markers = std::move(out);
}

// ===============================================
// RENDERING HELPERS
// ===============================================
struct RenderContext {
    ImDrawList* dl;
    ImVec2 origin;
    float scrollY;
    float rowNumWidth;
    float laneWidth;
    float markerWidth;
    const RowCalculator& rowCalc;
    float audio_length;
};

void draw_playback_indicator(const RenderContext& ctx, const std::vector<SampleRegion>& regions) {
    if (!g_play.playing || regions.empty()) return;

    // Find which region is currently playing
    for (const SampleRegion& reg : regions) {
        if ((int)g_play.cursor >= reg.start_sample && (int)g_play.cursor < reg.end_sample) {
            int row0 = ctx.rowCalc.sample_to_row(reg.start_sample);
            int row1 = ctx.rowCalc.sample_to_row(reg.end_sample - 1);
            int nrows = row1 - row0 + 1;

            // Highlight all rows in this region
            for (int row = row0; row <= row1; ++row) {
                float y1 = ctx.origin.y + row * 36.f - ctx.scrollY;
                float y2 = y1 + 36.f;
                ctx.dl->AddRectFilled(
                    ImVec2(ctx.origin.x + ctx.rowNumWidth, y1),
                    ImVec2(ctx.origin.x + ctx.rowNumWidth + ctx.laneWidth, y2),
                    IM_COL32(60, 255, 80, 60));
            }

            // Draw playhead position within the region
            float frac = float(g_play.cursor - reg.start_sample) /
                         float(std::max(1, reg.end_sample - reg.start_sample));
            float ph_y = ctx.origin.y + row0 * 36.f + frac * (nrows * 36.f) - ctx.scrollY;
            ctx.dl->AddLine(
                ImVec2(ctx.origin.x + ctx.rowNumWidth, ph_y),
                ImVec2(ctx.origin.x + ctx.rowNumWidth + ctx.laneWidth, ph_y),
                IM_COL32(60, 255, 60, 255), 3.2f);
            break;
        }
    }
}

void draw_waveform_for_row(const RenderContext& ctx, int row, int rows_per_bar) {
    float row_start_sec = ctx.rowCalc.row_to_seconds(row);
    float row_end_sec = ctx.rowCalc.row_to_seconds(row + 1);
    float env_start_frac = row_start_sec / ctx.audio_length;
    float env_end_frac = row_end_sec / ctx.audio_length;
    int env_start_idx = int(env_start_frac * g_wf.envelope.size());
    int env_end_idx = std::min(int(env_end_frac * g_wf.envelope.size()), int(g_wf.envelope.size()));

    float y = ctx.origin.y + row * 36.f - ctx.scrollY;
    float lane_center = ctx.origin.x + ctx.rowNumWidth + ctx.laneWidth / 2.f;
    float wf_y1 = y + 3;
    float wf_y2 = y + 36.f - 4;
    int bars = env_end_idx - env_start_idx;

    for (int e = 0; e < bars; ++e) {
        float amp = g_wf.envelope[env_start_idx + e];
        auto band = g_wf.bands[env_start_idx + e];
        float half = amp * (ctx.laneWidth / 2.f);
        float frac = bars > 1 ? e / float(bars - 1) : 0.5f;
        float cy = wf_y1 + frac * (wf_y2 - wf_y1);

        int r = int(255 * band[0]); // low
        int g = int(255 * band[1]); // mid
        int b = int(255 * band[2]); // high

        ctx.dl->AddLine(
            ImVec2(lane_center - half, cy),
            ImVec2(lane_center + half, cy),
            IM_COL32(r,g,b,220), 1.6f);
    }
}

void draw_region_marker(const RenderContext& ctx, int row, const SampleRegion& r, int region_index) {
    float y = ctx.origin.y + row * 36.f - ctx.scrollY;
    float labelx = ctx.origin.x + ctx.rowNumWidth + ctx.laneWidth + 24.f;

    // Offset label vertically based on how many regions start on this row
    float labely = y + 5.f + (region_index * 18.f);

    // Draw slice marker line
    ctx.dl->AddLine(
        ImVec2(ctx.origin.x + ctx.rowNumWidth, y),
        ImVec2(ctx.origin.x + ctx.rowNumWidth + ctx.laneWidth, y),
        IM_COL32(255, 70, 40, 255), 3.5f);

    // Draw note label
    char nbuf[16];
    get_note_name(r.midi_key, nbuf, sizeof(nbuf));
    ctx.dl->AddText(ImVec2(labelx, labely), IM_COL32(230, 230, 230, 255), nbuf);

    // Draw PLAY button
    ImGui::SetCursorScreenPos(ImVec2(labelx + 60.f, labely - 3));
    ImGui::PushID(r.midi_key * 1000 + region_index);
    if (ImGui::SmallButton("PLAY")) {
        play_region(r);
    }
    ImGui::PopID();
}

// ===============================================
// MAIN TRACKER DRAWING FUNCTION
// ===============================================
void draw_tracker_and_wave(
    float rowNumWidth, float laneWidth, float markerWidth, float panel_height,
    int rows_per_bar, float bpm)
{
    if (waveform.empty() || g_wf.envelope.empty() || samplerate <= 0 || bpm <= 0.0f) {
        ImGui::TextUnformatted("No waveform loaded");
        return;
    }

    // Calculate row metrics
    RowCalculator rowCalc(bpm, rows_per_bar, samplerate, waveform.size());
    float audio_length = waveform.size() / float(samplerate);

    // Compute regions and print debug info
    std::vector<SampleRegion> regions = compute_sample_regions(
        markers, samplerate, waveform.size(), base_note);
    static std::string last_region_filename;
    static size_t last_region_count = 0;
    if (loaded_filename != last_region_filename || regions.size() != last_region_count) {
        print_regions_debug(loaded_filename, regions, &rowCalc);
        last_region_filename = loaded_filename;
        last_region_count = regions.size();
    }

    ImGui::BeginChild("tracker_rows", ImVec2(rowNumWidth + laneWidth + markerWidth, panel_height),
        true, ImGuiWindowFlags_AlwaysVerticalScrollbar | ImGuiWindowFlags_HorizontalScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float scrollY = ImGui::GetScrollY();

    RenderContext ctx{dl, origin, scrollY, rowNumWidth, laneWidth, markerWidth, rowCalc, audio_length};

    // Draw playback indicator (if playing)
    draw_playback_indicator(ctx, regions);

    // Get visible viewport bounds for culling
    ImVec2 window_pos = ImGui::GetWindowPos();
    ImVec2 window_size = ImGui::GetWindowSize();
    float visible_min_y = window_pos.y;
    float visible_max_y = window_pos.y + window_size.y;

    // MAIN ROW LOOP
    for (int row = 0; row < rowCalc.total_rows; ++row) {
        float y = origin.y + row * 36.f - scrollY;
        float row_height = 36.f;
        
        // Viewport culling: skip rows outside visible area
        if (y + row_height < visible_min_y || y > visible_max_y) continue;

        // Draw row background
        ImU32 rowbg = row % 2 ? IM_COL32(38, 38, 46, 220) : IM_COL32(32, 32, 38, 240);
        dl->AddRectFilled(
            ImVec2(origin.x, y),
            ImVec2(origin.x + rowNumWidth + laneWidth + markerWidth, y + 36.f),
            rowbg, 0.0f);

        // Draw row number
        char numb[12];
        snprintf(numb, sizeof(numb), "%02d", row + 1);
        dl->AddText(ImVec2(origin.x + 10, y + 10), IM_COL32(200, 220, 255, 255), numb);

        // Draw grid line
        float grid_thick = (row % rows_per_bar == 0) ? 2.5f : 1.2f;
        ImU32 grid_col = (row % rows_per_bar == 0) ?
            IM_COL32(240, 180, 40, 220) : IM_COL32(120, 120, 120, 90);
        dl->AddLine(
            ImVec2(origin.x, y),
            ImVec2(origin.x + rowNumWidth + laneWidth + markerWidth, y),
            grid_col, grid_thick);

        // Draw waveform for this row
        draw_waveform_for_row(ctx, row, rows_per_bar);

        // Draw region markers (only on the start row of each region)
        // Count how many regions start on this row for vertical offset
        int region_idx_on_row = 0;
        for (size_t m = 0; m < regions.size(); ++m) {
            int region_row = rowCalc.sample_to_row(regions[m].start_sample);
            if (row == region_row) {
                draw_region_marker(ctx, row, regions[m], region_idx_on_row);
                region_idx_on_row++;
            }
        }

        // Handle marker add/remove with mouse clicks
        float gridbtn_height = 12.0f;
        ImGui::SetCursorScreenPos(ImVec2(origin.x, y));
        ImGui::InvisibleButton(
            ("gridbtn_" + std::to_string(row)).c_str(),
            ImVec2(rowNumWidth + laneWidth + markerWidth, gridbtn_height));

        // Check if there's already a marker on this row
        bool marker_drawn = false;
        for (size_t m = 0; m < regions.size(); ++m) {
            int region_row = rowCalc.sample_to_row(regions[m].start_sample);
            if (row == region_row) {
                marker_drawn = true;
                break;
            }
        }

        bool grid_hovered = ImGui::IsItemHovered() && ImGui::IsWindowFocused();

        // Left-click to add marker
        if (grid_hovered && ImGui::IsMouseClicked(0) && !marker_drawn) {
            markers.push_back({rowCalc.row_to_seconds(row)});
            std::sort(markers.begin(), markers.end(),
                [](auto& a, auto& b) { return a.time < b.time; });
        }

        // Right-click to remove marker
        if (grid_hovered && ImGui::IsMouseClicked(1) && marker_drawn) {
            markers.erase(std::remove_if(markers.begin(), markers.end(),
                [&](const SliceMarker& m) {
                    int marker_row = rowCalc.sample_to_row(
                        int(m.time * samplerate + 0.5f));
                    return marker_row == row;
                }), markers.end());
        }
    }

    ImGui::SetCursorPosY(rowCalc.total_rows * 36.f);
    ImGui::Dummy(ImVec2(0, 0));
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
        detect_onsets_aubio(detected_bpm, 24, true);
    }

    bool running = true;
    while (running) {
        SDL_Event e; while (SDL_PollEvent(&e)) { ImGui_ImplSDL2_ProcessEvent(&e); if (e.type == SDL_QUIT) running=false; }
        ImGui_ImplOpenGL2_NewFrame(); ImGui_ImplSDL2_NewFrame(); ImGui::NewFrame();
        ImGuiViewport* vp = ImGui::GetMainViewport(); ImVec2 pos = vp->Pos; ImVec2 siz = vp->Size;
        ImGuiWindowFlags pf = ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoBringToFrontOnFocus|ImGuiWindowFlags_NoNavFocus;
        static float rowNumWidth = 70.f, laneWidth = 480.f, markerWidth = 340.f;
        float leftW = 350.f, centerW = rowNumWidth + laneWidth + markerWidth + 60.f;
        float center_h = siz.y - 16.f;

        ImGui::SetNextWindowPos(pos); ImGui::SetNextWindowSize(ImVec2(leftW, siz.y));
        ImGui::Begin("LeftPanel", nullptr, pf);
        ImGui::Text("File: %s", loaded_filename.c_str());
        ImGui::SliderInt("Base Note", &base_note, 0, 100);
        ImGui::SliderFloat("BPM", &detected_bpm, 40.f, 240.f, "%.1f");
        ImGui::SliderInt("Rows/bar", &rows_per_bar, 1, 32);
        ImGui::SliderFloat("Row# Width", &rowNumWidth, 36.f, 160.f);
        ImGui::SliderFloat("Wave Lane Width", &laneWidth, 160.f, 700.f);
        ImGui::SliderFloat("Marker Area Width", &markerWidth, 100.f, 900.f);
        if (ImGui::Button("Auto Detect")) detect_onsets_aubio(detected_bpm, 24, false);
        ImGui::SameLine(); if (ImGui::Button("Clear")) markers.clear();
        if (ImGui::Button("Play All")) {
            g_play.start = 0; g_play.cursor = 0; g_play.end = waveform.size(); g_play.playing = true;
        }
        ImGui::SameLine(); if (ImGui::Button("Stop")) stop_playback();
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(pos.x + leftW, pos.y));
        ImGui::SetNextWindowSize(ImVec2(centerW, siz.y));
        ImGui::Begin("WavePanel", nullptr, pf);
        draw_tracker_and_wave(rowNumWidth, laneWidth, markerWidth, center_h, rows_per_bar, detected_bpm);
        ImGui::End();

        ImGui::Render();
        glViewport(0,0,(int)io.DisplaySize.x,(int)io.DisplaySize.y);
        glClearColor(0.1f,0.1f,0.12f,1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }
    ImGui_ImplOpenGL2_Shutdown(); ImGui_ImplSDL2_Shutdown(); ImGui::DestroyContext();
    SDL_GL_DeleteContext(glctx); SDL_DestroyWindow(window); SDL_Quit();
    return 0;
}
