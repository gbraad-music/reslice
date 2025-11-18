import sys
import aubio
import numpy as np

# Load a short audio file
filename = sys.argv[1]
samplerate = 44100
win_s = 1024
hop_s = win_s // 2

# Source object
src = aubio.source(filename, samplerate, hop_s)
samplerate = src.samplerate

# Create aubio objects
tempo = aubio.tempo("default", win_s, hop_s, samplerate)
onset = aubio.onset("default", win_s, hop_s, samplerate)
notes = aubio.notes("default", win_s, hop_s, samplerate)

# Buffers
total_frames = 0

print("=== Analysis of", filename, "===")
while True:
    samples, read = src()
    
    # BPM / beat tracking
    if tempo(samples):
        bpm = tempo.get_bpm()
        print(f"Beat detected at {total_frames/samplerate:.3f}s (BPM estimate: {bpm:.2f})")
    
    # Onset detection
    if onset(samples):
        print(f"Onset at {onset.get_last_s():.3f}s")
    
    # Note detection
    note = notes(samples)
    if note[0] != 0:  # nonzero means a note event
        pitch = int(note[0])
        dur = note[1]
        timestamp = total_frames / float(samplerate)
        print(f"Note {pitch} at {timestamp:.3f}s, duration {dur:.3f}s")   
 
    total_frames += read
    if read < hop_s:
        break

