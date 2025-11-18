#!/usr/bin/env python3
import sys
import os
import aubio
import mido

def seconds_to_ticks(sec: float, bpm: float, ppqn: int) -> int:
    return int(sec * (bpm / 60.0) * ppqn)

def main():
    if len(sys.argv) < 2:
        print("Usage: python reslice_sfz_offsets.py <audiofile>")
        sys.exit(1)

    filepath = sys.argv[1]
    filename = os.path.basename(filepath)  # only filename for SFZ

    # Parameters
    samplerate = 44100
    win_s = 1024
    hop_s = win_s // 2
    ppqn = 480
    base_note = 36 

    # -------- BPM detection --------
    src = aubio.source(filepath, samplerate, hop_s)
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

    # -------- Onset detection --------
    src = aubio.source(filepath, samplerate, hop_s)
    onset = aubio.onset("default", win_s, hop_s, samplerate)
    onsets = []
    total_frames = 0
    while True:
        samples, read = src()
        if onset(samples):
            onset_s = total_frames / float(samplerate)
            onsets.append(onset_s)
        total_frames += read
        if read < hop_s:
            break
    print(f"Detected {len(onsets)} onsets")

    # -------- Generate SFZ with offset/end --------
    sfz_lines = ["<group>"]
    for i, onset_s in enumerate(onsets):
        start = int(onset_s * samplerate)
        if i < len(onsets) - 1:
            end = int(onsets[i+1] * samplerate)
        else:
            end = total_frames  # last slice goes to end of file
        note = base_note + i
        sfz_lines.append(
            f"<region> sample={filename} key={note} offset={start} end={end}"
        )
    with open("slices.sfz", "w") as f:
        f.write("\n".join(sfz_lines))
    print("Wrote slices.sfz")

    # -------- Generate MIDI --------
    events = []
    for i, onset_s in enumerate(onsets):
        note = base_note + i
        on_tick = seconds_to_ticks(onset_s, detected_bpm, ppqn)
        events.append((on_tick, 'on', note))
        if i < len(onsets) - 1:
            off_tick = seconds_to_ticks(onsets[i+1], detected_bpm, ppqn)
            events.append((off_tick, 'off', note))
        # last slice: no note_off (let it play to end)

    # Sort events
    events.sort(key=lambda e: (e[0], 0 if e[1] == 'off' else 1))

    mid = mido.MidiFile(ticks_per_beat=ppqn)
    track = mido.MidiTrack()
    mid.tracks.append(track)
    tempo = mido.bpm2tempo(detected_bpm)
    track.append(mido.MetaMessage('set_tempo', tempo=tempo))

    last_tick = 0
    for abs_tick, etype, note in events:
        delta = abs_tick - last_tick
        if delta < 0:
            delta = 0
        if etype == 'on':
            track.append(mido.Message('note_on', note=note, velocity=100, time=delta))
        else:
            track.append(mido.Message('note_off', note=note, velocity=100, time=delta))
        last_tick = abs_tick

    track.append(mido.MetaMessage('end_of_track', time=0))
    mid.save("slices.mid")
    print("Wrote slices.mid")

if __name__ == "__main__":
    main()

