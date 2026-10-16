# StageCue Soundcard Simulator

A terminal UI (FTXUI) that shows up to StageCue's `PluginHost` as a
soundcard with a configurable number of inputs and outputs, so the
frontend → backend → audio path can be tested without real hardware.

- **Inputs** (sim → PluginHost): each one plays silence, a sine tone
  (frequency and level), or an audio file (WAV/FLAC/MP3, mixed to mono
  and resampled to the device rate, up to 20 minutes). All files follow one shared transport, so
  they play in sync like a live band (see [Transport](#transport)).
- **Outputs** (PluginHost → sim): each one has a live meter, a
  **● SIGNAL** indicator (above −60 dBFS, held for 1 s) and "last signal"
  time. Outputs ticked "main out" are mixed to the computer's speakers
  (odd outputs → L, even → R).
- The header shows whether PluginHost currently has the device open as input
  and/or output, plus its channel count and rate (from `/proc/asound` on Linux,
  from the CoreAudio HAL on macOS 14+).

Runs on Linux, using the kernel's ALSA loopback driver (`snd-aloop`), and on
macOS, using a CoreAudio loopback driver built from a patched
[BlackHole](https://github.com/ExistentialAudio/BlackHole) (see [macOS](#macos)).
Audio I/O, decoding and resampling come from [miniaudio](https://miniaud.io),
so almost all of the code is the same on both platforms.

## Design

```
 platform/      loopback checks, PluginHost link status (/proc or CoreAudio HAL)
 AudioIo        three miniaudio devices, each driven by its own callback
 Mixer          sources -> inputs, outputs -> meters + monitor mix (pure, unit-tested)
 SourceSlots    hands sources to the audio thread without locks
 StatusPoller   link status on a background thread, once a second
 SimEngine      the one API the UI (or --headless) uses
```

Built to run for hours without loading the machine:

- **No extra audio threads and no polling.** Each device's callback does its own
  work: the inputs callback renders the sources, the outputs callback meters
  PluginHost's outputs and feeds the main out through a lock-free ring.
- **Nothing on the audio thread locks, allocates or frees.** Sources reach the
  callback as raw pointers. A replaced source is freed on the UI thread once the
  callback has moved past it.
- **Bounded main-out latency.** When the loopback and speaker clocks drift apart,
  the main out drops or pads a block (counted as *dropouts*) instead of queueing
  audio forever.
- **The UI never waits on the system.** The link status is polled once a second
  on its own thread, files load on a worker, and at most one redraw is ever
  queued. The UI redraws at 20 Hz.
- macOS: the HAL is told to deliver its notifications on its own thread. A
  terminal app never runs the main run loop they would otherwise queue up on.

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

Linux:

```bash
sudo apt install libasound2-dev        # ALSA headers (PluginHost needs them too)
./setup-loopback.sh                    # sudo modprobe snd-aloop + ALSA alias
./setup-loopback.sh --status           # check
./setup-loopback.sh --persist          # optional: load the card at every boot
./setup-loopback.sh --remove           # undo everything
```

If `snd-aloop` is missing: `sudo apt install linux-modules-extra-$(uname -r)`.

macOS (needs Xcode, which builds the driver):

```bash
./setup-loopback.sh                    # build + install both loopback devices (sudo)
./setup-loopback.sh --status           # check
./setup-loopback.sh --remove           # uninstall
```

## macOS

There is no kernel loopback on macOS, so `setup-loopback.sh` builds BlackHole
v0.7.2 from source with `patches/blackhole-crossed-loopback.patch` and installs
it into `/Library/Audio/Plug-Ins/HAL`. The driver has two devices, crossed like
the two ends of `snd-aloop`:

```
             sim side                                  PluginHost side
 sources ──► "StageCue Soundcard Sim (sim side)" out ═► in  ┐
                                                            ├─ "StageCue Soundcard Sim" (32 ch)
 meters  ◄── "StageCue Soundcard Sim (sim side)" in  ◄═ out ┘
   └──► main out (system default output, or --monitor NAME)
```

Both devices share one clock. Neither can become the system default, so macOS
never routes other sounds through them.

- In PluginHost, select **StageCue Soundcard Sim** as both the input and the
  output device, as on Linux.
- Why one device: given two different devices, JUCE bridges them with its
  `AudioIODeviceCombiner`. Loopback devices report zero latency, so the
  combiner's FIFO underruns all the time and the outputs glitch (small buffers)
  or stay silent (512 and up). The earlier two-driver setup ("StageCue Sim
  Inputs" / "StageCue Sim Outputs") had that problem; `./setup-loopback.sh`
  replaces it.
- The simulator switches the driver to `--rate`. PluginHost has to run at the
  same rate.
- `--period` only sets the simulator's own buffer size. PluginHost picks its own
  buffer size on macOS.
- The main out resamples, so it works whatever rate the speakers run at.
- The PluginHost link rows show **CONNECTED** on macOS 14 and later, where the
  HAL reports which processes use a device. The channel count isn't reported.

## Build

Standalone (also builds the unit tests):

```bash
cmake -S . -B build -G Ninja
cmake --build build
./build/SoundcardSimTest
```

Inside stagecue-backend, where this repo is the `tools/soundcard-sim`
submodule, it is built as part of the main project (`STAGECUE_BUILD_SOUNDCARD_SIM`,
default ON on Linux and macOS):

```bash
cmake --build out/build/HostLib --target StageCueSoundcardSim
```

Dependencies are fetched with CPM: FTXUI v6.1.9, miniaudio 0.11.25, and
googletest (tests only). Build with `-DCMAKE_BUILD_TYPE=RelWithDebInfo` for
everyday use: a Debug build of the UI costs several times the CPU.

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
| `--card ID` | StageCueSim | Linux: must match `CARD_ID` used by the setup script |
| `--inputs-device NAME` | StageCue Soundcard Sim (sim side) | macOS: device the inputs are written to |
| `--outputs-device NAME` | StageCue Soundcard Sim (sim side) | macOS: device the outputs are read from |
| `--host-device NAME` | StageCue Soundcard Sim | macOS: device PluginHost opens (link status) |
| `--monitor DEVICE` | default | main out: an ALSA PCM on Linux, a CoreAudio device name on macOS |
| `--source SPEC` | | `N=silence`, `N=sine[:HZ[:DB]]`, `N=file:PATH[:DB]` |
| `--play` | off | start the song transport playing |
| `--headless` | off | no UI: print a status line (xruns, dropouts, peaks, link, memory) every 5 s |

### Transport

File inputs play like a live band: they share one song position, so they
start together and stay sample-aligned. The song loops when the longest file
ends, and shorter files are silent until then. **▶ Play / ■ Stop** in the
Control panel starts and pauses the song. **⏮ Restart** rewinds it to 0:00.
A file loaded during playback joins at the current song position. The
transport starts stopped, so you can load every stem and then press Play
(or pass `--play`). Sine sources ignore the transport.

### Health checks

The header shows **stalled** when the loopback stops calling back for over a
second. If a device disappears (driver reinstalled, card unloaded), the error
line says so and **Apply** reopens it. *Xruns* count loopback callbacks that came
much later than the block length allows. A soak test:

```bash
StageCueSoundcardSim --headless --source 1=sine:1000:-12 --play
```

On macOS you can also check the loopback without PluginHost: pass
`--outputs-device "StageCue Soundcard Sim"` (PluginHost's end) and the outputs
read back what the inputs play (peak out = peak in).

Keys: Tab / arrows move between controls, ←/→ adjust sliders and toggles, Enter
loads a file path, Esc or Ctrl+C quits.

## Testing with PluginHost

1. `./setup-loopback.sh`, then start the simulator.
2. Start PluginHost. In the frontend's audio settings, select
   **StageCue Soundcard Sim** as both input and output device (Linux and
   macOS). The PluginHost link panel should show Input and Output as CONNECTED.
3. Set In 1 to a 1 kHz sine at −12 dB. PluginHost's `channelsLevels` meter
   for the channel fed by hardware input 1 should read about −12 dB.
4. Route that channel to the main bus. Out 1/2 in the simulator should light
   **● SIGNAL**, and you should hear the tone on the speakers.

Without PluginHost you can check the loopback with ALSA tools while the
simulator runs (Linux):

```bash
arecord -D stagecue_sim -c 4 -r 48000 -f S32_LE -d 2 /tmp/in.wav   # records the sim's inputs
speaker-test -D stagecue_sim -c 2 -r 48000 -F S32_LE -t sine        # lights the sim's outputs
```
