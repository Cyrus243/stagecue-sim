# StageCue Soundcard Simulator

A terminal UI (FTXUI) that shows up to StageCue's `PluginHost` as a
soundcard with a configurable number of inputs and outputs, so the
frontend → backend → audio path can be tested without real hardware.

- **Inputs** (sim → PluginHost): each one plays silence, a sine tone
  (frequency and level), or an audio file (WAV/AIFF/FLAC, mixed to mono
  and resampled to the device rate). All files follow one shared transport, so
  they play in sync like a live band (see [Transport](#transport)).
- **Outputs** (PluginHost → sim): each one has a live meter, a
  **● SIGNAL** indicator (above −60 dBFS, held for 1 s) and "last signal"
  time. Outputs ticked "main out" are mixed to the computer's speakers
  (odd outputs → L, even → R).
- The header shows whether PluginHost currently has the device open as input
  and/or output, plus its channel count and rate, read from `/proc/asound`.

Linux only. The simulator uses the kernel's ALSA loopback driver (`snd-aloop`).

## How it works

```
             sim side (dev 0)                     PluginHost side (dev 1)
 sources ──► playback hw:CARD=StageCueSim,DEV=0 ═► capture  ┐
                                                            ├─ "StageCue Soundcard Sim"
 meters  ◄── capture  hw:CARD=StageCueSim,DEV=0 ◄═ playback ┘   (ALSA alias stagecue_sim)
   └──► main out (ALSA "default" → PipeWire/Pulse)
```

`snd-aloop` sends anything played on device 0 to capture on device 1, and the
reverse. The side that opens a direction first fixes its channel count,
sample rate, format and period size. The other side has to match. So:

- **Start the simulator before selecting the device in PluginHost.**
- PluginHost's buffer size follows the simulator's `--period` (default 512),
  and its sample rate follows `--rate` (default 48000). Those are PluginHost's
  own defaults.
- After changing the IO count (Apply), re-select the device in PluginHost.

`setup-loopback.sh` also adds a user-level ALSA alias, `stagecue_sim`, with
the description "StageCue Soundcard Sim". Without it, both loopback devices
would appear in PluginHost under the same name ("Loopback, Loopback PCM").

## Setup

```bash
sudo apt install libasound2-dev        # ALSA headers (PluginHost needs them too)
./setup-loopback.sh                    # sudo modprobe snd-aloop + ALSA alias
./setup-loopback.sh --status           # check
./setup-loopback.sh --persist          # optional: load the card at every boot
./setup-loopback.sh --remove           # undo everything
```

If `snd-aloop` is missing: `sudo apt install linux-modules-extra-$(uname -r)`.

## Build

Standalone (also builds the unit tests):

```bash
cmake -S . -B build -G Ninja
cmake --build build
./build/SoundcardSimTest
```

Inside stagecue-backend, where this repo is the `tools/soundcard-sim`
submodule, it is built as part of the main project (`STAGECUE_BUILD_SOUNDCARD_SIM`,
default ON on Linux):

```bash
cmake --build out/build/HostLib --target StageCueSoundcardSim
```

Dependencies are fetched with CPM: FTXUI v6.1.9, dr_libs (dr_wav and dr_flac),
and googletest (tests only).

## Run

```bash
StageCueSoundcardSim --inputs 4 --outputs 2 \
    --source 1=sine:1000:-12 \
    --source 2=file:~/samples/drums.wav:-6
```

| Option | Default | |
|---|---|---|
| `--inputs N` / `--outputs N` | 8 / 8 | 1–32 each |
| `--rate HZ` | 48000 | |
| `--period FRAMES` | 512 | PluginHost's buffer size follows it |
| `--card ID` | StageCueSim | must match `CARD_ID` used by the setup script |
| `--monitor PCM` | default | ALSA device for the main out |
| `--source SPEC` | | `N=silence`, `N=sine[:HZ[:DB]]`, `N=file:PATH[:DB]` |
| `--play` | off | start the song transport playing |

### Transport

File inputs play like a live band: they share one song position, so they
start together and stay sample-aligned. The song loops when the longest file
ends, and shorter files are silent until then. **▶ Play / ■ Stop** in the
Control panel starts and pauses the song. **⏮ Restart** rewinds it to 0:00.
A file loaded during playback joins at the current song position. The
transport starts stopped, so you can load every stem and then press Play
(or pass `--play`). Sine sources ignore the transport.

Keys: Tab / arrows move between controls, ←/→ adjust sliders and toggles, Enter
loads a file path, Esc or Ctrl+C quits.

## Testing with PluginHost

1. `./setup-loopback.sh`, then start the simulator.
2. Start PluginHost. In the frontend's audio settings, select
   **StageCue Soundcard Sim** as both input and output device. The header
   should show `PluginHost input: connected` and `PluginHost output: connected`.
3. Set In 1 to a 1 kHz sine at −12 dB. PluginHost's `channelsLevels` meter
   for the channel fed by hardware input 1 should read about −12 dB.
4. Route that channel to the main bus. Out 1/2 in the simulator should light
   **● SIGNAL**, and you should hear the tone on the speakers.

Without PluginHost you can check the loopback with ALSA tools while the
simulator runs:

```bash
arecord -D stagecue_sim -c 4 -r 48000 -f S32_LE -d 2 /tmp/in.wav   # records the sim's inputs
speaker-test -D stagecue_sim -c 2 -r 48000 -F S32_LE -t sine        # lights the sim's outputs
```
