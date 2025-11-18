#!/usr/bin/env python3
import sys
import aubio
import mido

def seconds_to_ticks(sec: float, bpm: float, ppqn: int) -> int:
    # ticks = seconds * (beats/sec) * ticks_per_beat
    return max(int(sec * (bpm / 60.0) * ppqn), 0)

def main():
    if len(sys.argv) < 2:
        print("Usage: python reslice_out.py <audiofile>")
        sys.exit(1)

    filename = sys.argv[1]

    # Analysis config
    samplerate = 44100
    win_s = 1024
    hop_s = win_s // 2

    # Tracker-style resolution
    ppqn = 24   # 4 rows/beat × 6 ticks/row
    row_ticks = ppqn // 4  # ticks per row = 6
    velocity = 80

    # First pass: detect BPM
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
        detected_bpm = 120.0  # fallback
    print(f"Detected BPM: {detected_bpm:.2f}")

    # Second pass: note extraction
    src = aubio.source(filename, samplerate, hop_s)
    notes = aubio.notes("default", win_s, hop_s, samplerate)

    events = []  # list of (abs_ticks, type, pitch)
    total_frames = 0

    while True:
        samples, read = src()
        note_vec = notes(samples)
        if note_vec[0] != 0:
            pitch = int(note_vec[0])
            onset_s = total_frames / float(samplerate)

            # Convert seconds → ticks, then quantize to nearest row
            abs_on = seconds_to_ticks(onset_s, detected_bpm, ppqn)
            abs_on = round(abs_on / row_ticks) * row_ticks

            # Fixed duration: 1 row
            abs_off = abs_on + row_ticks

            events.append((abs_on, 'on', pitch))
            events.append((abs_off, 'off', pitch))

        total_frames += read
        if read < hop_s:
            break

    # Sort events by absolute tick
    events.sort(key=lambda e: (e[0], 0 if e[1] == 'off' else 1))

    # Build MIDI with safe delta times
    mid = mido.MidiFile(ticks_per_beat=ppqn)
    track = mido.MidiTrack()
    mid.tracks.append(track)

    microseconds_per_beat = int(60_000_000 / detected_bpm)
    track.append(mido.MetaMessage('set_tempo', tempo=microseconds_per_beat))

    last_tick = 0
    for abs_tick, etype, pitch in events:
        delta = abs_tick - last_tick
        if delta < 0:
            delta = 0
        if etype == 'on':
            track.append(mido.Message('note_on', note=pitch, velocity=velocity, time=delta))
        else:
            track.append(mido.Message('note_off', note=pitch, velocity=velocity, time=delta))
        last_tick = abs_tick

    track.append(mido.MetaMessage('end_of_track', time=0))

    out_name = "output.mid"
    mid.save(out_name)
    print(f"Saved {out_name} with {len(events)//2} notes at {detected_bpm:.2f} BPM, PPQN={ppqn}")

if __name__ == "__main__":
    main()

