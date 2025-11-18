#!/usr/bin/env python3
import sys
import aubio
import mido
import statistics

def seconds_to_ticks(sec: float, bpm: float, ppqn: int) -> int:
    return max(int(sec * (bpm / 60.0) * ppqn), 0)

def quantize_ticks(abs_tick: int, row_ticks: int) -> int:
    return round(abs_tick / row_ticks) * row_ticks

def main():
    if len(sys.argv) < 2:
        print("Usage: python reslice_out.py <audiofile>")
        sys.exit(1)

    filename = sys.argv[1]

    # Analysis config
    samplerate = 44100
    win_s = 1024
    hop_s = win_s // 2  # 512

    # Tracker grid
    ppqn = 24
    row_ticks = ppqn // 4  # 6 ticks/row
    velocity = 80

    # Expected register (soft clamp; adjust to your material)
    midi_min = 55  # G3
    midi_max = 88  # E6

    # -------- BPM detection (first pass) --------
    src = aubio.source(filename, samplerate, hop_s)
    samplerate = src.samplerate
    tempo = aubio.tempo("default", win_s, hop_s, samplerate)

    detected_bpm = None
    total_frames = 0
    while True:
        samples, read = src()
        if tempo(samples):
            bpm = tempo.get_bpm()
            if bpm and bpm > 0:
                detected_bpm = float(bpm)
                break
        total_frames += read
        if read < hop_s:
            break
    if not detected_bpm:
        detected_bpm = 120.0
    print(f"Detected BPM: {detected_bpm:.2f}")

    # -------- Onset-gated pitch (second pass) --------
    src = aubio.source(filename, samplerate, hop_s)
    onset = aubio.onset("default", win_s, hop_s, samplerate)

    # YIN for fundamental
    pitch = aubio.pitch("yin", win_s, hop_s, samplerate)
    pitch.set_unit("midi")
    pitch.set_silence(-40)
    pitch.set_tolerance(0.8)

    events = []  # (abs_on, abs_off, midi_note)
    total_frames = 0
    smoothing_hops = 6
    last_note = None

    while True:
        samples, read = src()
        if onset(samples):
            onset_s = total_frames / float(samplerate)
            abs_on = quantize_ticks(seconds_to_ticks(onset_s, detected_bpm, ppqn), row_ticks)

            # Collect pitch estimates after onset
            ests = []
            confs = []
            frames_left = smoothing_hops
            lok_frames = total_frames

            while frames_left > 0:
                p = float(pitch(samples))  # cast to float
                c = float(pitch.get_confidence())
                if p > 0 and c >= 0.4:
                    ests.append(p)
                    confs.append(c)

                lok_frames += read
                frames_left -= 1
                samples2, read2 = src()
                samples = samples2
                read = read2
                if read < hop_s:
                    break

            if ests:
                med = statistics.median(ests)  # now a Python float
                candidate = int(round(float(med)))

                # Octave normalization
                if last_note is not None:
                    diff = candidate - last_note
                    strong = statistics.median(confs) if confs else 0.0
                    if abs(diff) >= 8 and strong < 0.85:
                        options = [candidate - 12, candidate, candidate + 12]
                        candidate = min(options, key=lambda n: abs(n - last_note))

                # Soft clamp
                if candidate < midi_min:
                    candidate += 12 * ((midi_min - candidate + 11) // 12)
                if candidate > midi_max:
                    candidate -= 12 * ((candidate - midi_max + 11) // 12)

                abs_off = abs_on + row_ticks
                events.append((abs_on, abs_off, candidate))
                last_note = candidate

        total_frames += read
        if read < hop_s:
            break

    # -------- Build MIDI --------
    ev_msgs = []
    for abs_on, abs_off, midi_note in events:
        ev_msgs.append((abs_on, 'on', midi_note))
        off_tick = abs_off if abs_off > abs_on else abs_on + 1
        ev_msgs.append((off_tick, 'off', midi_note))

    ev_msgs.sort(key=lambda e: (e[0], 0 if e[1] == 'off' else 1))

    mid = mido.MidiFile(ticks_per_beat=ppqn)
    track = mido.MidiTrack()
    mid.tracks.append(track)

    microseconds_per_beat = int(60_000_000 / detected_bpm)
    track.append(mido.MetaMessage('set_tempo', tempo=microseconds_per_beat))

    last_tick = 0
    for abs_tick, etype, note in ev_msgs:
        delta = abs_tick - last_tick
        if delta < 0:
            delta = 0
        if etype == 'on':
            track.append(mido.Message('note_on', note=note, velocity=velocity, time=delta))
        else:
            track.append(mido.Message('note_off', note=note, velocity=velocity, time=delta))
        last_tick = abs_tick

    track.append(mido.MetaMessage('end_of_track', time=0))
    out_name = "output.mid"
    mid.save(out_name)
    print(f"Saved {out_name} with {len(events)} quantized notes at {detected_bpm:.2f} BPM, PPQN={ppqn}")

if __name__ == "__main__":
    main()

