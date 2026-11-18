"""Describes a recording made by QEMU's wav audio backend: where the sound
is, what pitch each stretch has, and whether there are gaps or clicks."""
import struct
import sys
import wave

w = wave.open(sys.argv[1], 'rb')
rate, channels, width, frames = w.getframerate(), w.getnchannels(), w.getsampwidth(), w.getnframes()
raw = w.readframes(frames)
print('format: %d Hz, %d channel(s), %d-bit, %.2f s' % (rate, channels, width * 8, frames / rate))

samples = struct.unpack('<%dh' % (len(raw) // 2), raw)
left = samples[0::channels]
right = samples[1::channels] if channels > 1 else left

# 20 ms windows: loud or quiet
win = rate // 50
loud = []
for i in range(0, len(left) - win, win):
    peak = max(abs(v) for v in left[i:i + win])
    loud.append(peak)

segments = []
start = None
for i, peak in enumerate(loud + [0]):
    if peak > 200 and start is None:
        start = i
    elif peak <= 200 and start is not None:
        # allow gaps shorter than 60 ms inside a sound
        if any(p > 200 for p in loud[i:i + 3]):
            continue
        segments.append((start, i))
        start = None

print('sounds found:', len(segments))
for a, b in segments:
    s0, s1 = a * win, b * win
    part = left[s0:s1]
    crossings = sum(1 for k in range(1, len(part)) if part[k - 1] < 0 <= part[k])
    seconds = (s1 - s0) / rate
    freq = crossings / seconds if seconds else 0
    peak = max(abs(v) for v in part)
    rpeak = max(abs(v) for v in right[s0:s1])
    # a click shows as a jump far bigger than a smooth wave of this pitch makes
    jumps = sum(1 for k in range(1, len(part)) if abs(part[k] - part[k - 1]) > 6000)
    # dropouts: runs of exact zeros inside the sound longer than 2 ms
    zero_runs = 0
    run = 0
    for v in part:
        if v == 0:
            run += 1
        else:
            if run > rate // 500:
                zero_runs += 1
            run = 0
    print('  at %6.2f s for %5.2f s: about %5.0f Hz, peak L %5d R %5d, big jumps %d, dropouts %d'
          % (s0 / rate, seconds, freq, peak, rpeak, jumps, zero_runs))
