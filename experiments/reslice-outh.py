#!/usr/bin/env python3
import sys
import aubio
import librosa
import mido
import numpy as np

def seconds_to_ticks(sec: float, bpm: float, ppqn: int) -> int:
    return max(int(sec * (bpm / 60.0) * ppqn), 0)

def quantize_ticks(abs_tick: int, row_ticks: int) -> int:
    return round(abs_tick / row_ticks) * row_ticks

def main():
    if len(sys.argv) < 2:
        print("Usage: python hybrid_reslice.py <audiofile>")
        sys.exit(1)

    filename = sys.argv[1]

    # Tracker grid
    ppqn = 24
    row_ticks = ppqn // 4  # 6 ticks/row
    velocity = 80

    # -------- Step 1: Detect BPM with aubio --------
    samplerate = 44100
    win_s = 1024
    hop_s = win_s // 2

    src = aubio.source(filename, samplerate, hop_s)
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

    # -------- Step 2: Onset detection with aubio --------
    src = aubio.source(filename, samplerate, hop_s)
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

    # -------- Step 3: Pitch estimation with librosa.pyin --------
    y, sr = librosa.load(filename, sr=samplerate)

    events = []
    slice_len = int(1.0 * sr)  # 1 second slice

    for onset_s in onsets:
        start = int(onset_s * sr)
        end = min(len(y), start + slice_len)
        y_slice = y[start:end]

        if len(y_slice) > 0:
            pitches, voiced_flags, _ = librosa.pyin(
                y_slice,
                fmin=librosa.note_to_hz('C1'),
                fmax=librosa.note_to_hz('C8'),
                sr=sr
            )
            midi_pitches = librosa.hz_to_midi(pitches)
            valid = [float(p) for p in midi_pitches if p is not None and not np.isnan(p)]
            print(f"Onset {onset_s:.2f}s → {valid}")  # debug

            if valid:
                pitch = int(round(np.median(valid)))
                abs_on = quantize_ticks(seconds_to_ticks(onset_s, detected_bpm, ppqn), row_ticks)
                abs_off = abs_on + row_ticks
                events.append((abs_on, abs_off, pitch))

    # Convert to MIDI numbers
    midi_pitches = librosa.hz_to_midi(pitches)

    # -------- Step 4: Build MIDI events --------
    events = []
    for onset_s in onsets:
        # Find nearest frame in librosa pitch track
        frame = int(onset_s * sr / hop_s)
        if frame < len(midi_pitches):
            pitch = midi_pitches[frame]
            if pitch and not np.isnan(pitch):
                pitch = int(round(pitch))
                abs_on = quantize_ticks(seconds_to_ticks(onset_s, detected_bpm, ppqn), row_ticks)
                abs_off = abs_on + row_ticks
                events.append((abs_on, abs_off, pitch))

    # -------- Step 5: Write MIDI --------
    ev_msgs = []
    for abs_on, abs_off, midi_note in events:
        ev_msgs.append((abs_on, 'on', midi_note))
        ev_msgs.append((abs_off, 'off', midi_note))

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
    print(f"Saved {out_name} with {len(events)} notes at {detected_bpm:.2f} BPM, PPQN={ppqn}")

if __name__ == "__main__":
    main()

