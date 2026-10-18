#!/usr/bin/env bash
# Sets up the loopback devices used by StageCueSoundcardSim.
#
# Linux: loads the ALSA loopback card (snd-aloop), registers a user-level ALSA alias
# so PluginHost lists it as "StageCue Soundcard Sim", and tells WirePlumber to leave
# the card alone.
# macOS: builds BlackHole (CoreAudio loopback driver) from source with a patch that
# crosses its two devices like snd-aloop: "StageCue Soundcard Sim" (PluginHost's side,
# one device for input and output) and "StageCue Soundcard Sim (sim side)". Installs it
# into /Library/Audio/Plug-Ins/HAL. Needs Xcode.
#
#   ./setup-loopback.sh            set up (asks for sudo)
#   ./setup-loopback.sh --persist  same, and load the card at every boot (macOS: always the case)
#   ./setup-loopback.sh --remove   undo everything
#   ./setup-loopback.sh --status   show what is installed
#
# Env (Linux): CARD_ID (default StageCueSim), ASOUNDRC (default ~/.asoundrc)
set -euo pipefail

CARD_ID="${CARD_ID:-StageCueSim}"
ASOUNDRC="${ASOUNDRC:-$HOME/.asoundrc}"
MODPROBE_CONF=/etc/modprobe.d/stagecue-soundcard-sim.conf
MODULES_LOAD_CONF=/etc/modules-load.d/stagecue-soundcard-sim.conf
BEGIN_MARK="# >>> stagecue-soundcard-sim >>>"
END_MARK="# <<< stagecue-soundcard-sim <<<"
MODULE_OPTIONS="id=${CARD_ID} pcm_substreams=1"
WIREPLUMBER_CONF="${XDG_CONFIG_HOME:-$HOME/.config}/wireplumber/wireplumber.conf.d/51-stagecue-soundcard-sim.conf"

info() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

card_loaded() { [[ -e "/proc/asound/${CARD_ID}" ]]; }

remove_alias() {
    [[ -f "$ASOUNDRC" ]] || return 0
    local tmp
    tmp="$(mktemp)"
    awk -v begin="$BEGIN_MARK" -v end="$END_MARK" '
        $0 == begin { skip = 1; next }
        $0 == end   { skip = 0; next }
        !skip       { print }
    ' "$ASOUNDRC" > "$tmp"
    cat "$tmp" > "$ASOUNDRC"
    rm -f "$tmp"
}

install_alias() {
    remove_alias
    # `type empty` is a pure pass-through: the loopback's channel/format/period
    # constraints reach PluginHost unchanged, but the hint gives it a readable name.
    cat >> "$ASOUNDRC" <<EOF
${BEGIN_MARK}
pcm.stagecue_sim {
    type empty
    slave.pcm "hw:CARD=${CARD_ID},DEV=1"
    hint {
        show on
        description "StageCue Soundcard Sim"
    }
}
${END_MARK}
EOF
    info "ALSA alias 'stagecue_sim' (\"StageCue Soundcard Sim\") written to ${ASOUNDRC}"
}

# Left alone, WirePlumber turns the loopback card into regular PipeWire nodes and
# may make "Loopback Analog Stereo" the default source. Those nodes need the same
# substream the simulator holds (pcm_substreams=1), so any app recording from
# "default" - PluginHost included - stalls or crashes in PipeWire's ALSA plugin.
restart_wireplumber() {
    if command -v systemctl >/dev/null && systemctl --user is-active --quiet wireplumber; then
        systemctl --user restart wireplumber
        info "wireplumber restarted"
    fi
}

install_wireplumber_rule() {
    command -v wireplumber >/dev/null || return 0
    mkdir -p "$(dirname "$WIREPLUMBER_CONF")"
    cat > "$WIREPLUMBER_CONF" <<EOF
# Written by stagecue soundcard-sim setup-loopback.sh: keep PipeWire off the
# simulator's loopback card so it never becomes the default source/sink.
monitor.alsa.rules = [
  {
    matches = [ { alsa.id = "${CARD_ID}" } ]
    actions = { update-props = { device.disabled = true } }
  }
]
EOF
    info "WirePlumber rule written to ${WIREPLUMBER_CONF}"
    restart_wireplumber
}

remove_wireplumber_rule() {
    [[ -f "$WIREPLUMBER_CONF" ]] || return 0
    rm -f "$WIREPLUMBER_CONF"
    info "WirePlumber rule removed"
    restart_wireplumber
}

load_module() {
    if card_loaded; then
        info "loopback card '${CARD_ID}' already loaded"
        return 0
    fi

    modinfo snd-aloop >/dev/null 2>&1 \
        || die "kernel module snd-aloop not found (Ubuntu: sudo apt install linux-modules-extra-\$(uname -r))"

    if lsmod | grep -q '^snd_aloop'; then
        die "snd-aloop is already loaded with another id. Unload it first: sudo modprobe -r snd-aloop"
    fi

    info "loading snd-aloop (${MODULE_OPTIONS}) - sudo may ask for your password"
    sudo modprobe snd-aloop ${MODULE_OPTIONS}

    for _ in 1 2 3 4 5 6 7 8 9 10; do
        card_loaded && break
        sleep 0.2
    done
    card_loaded || die "snd-aloop loaded but /proc/asound/${CARD_ID} did not appear"
    info "loopback card '${CARD_ID}' is ready"
}

persist() {
    info "configuring snd-aloop to load at boot"
    echo "options snd-aloop ${MODULE_OPTIONS}" | sudo tee "$MODPROBE_CONF" >/dev/null
    echo "snd-aloop" | sudo tee "$MODULES_LOAD_CONF" >/dev/null
}

remove_all() {
    if lsmod | grep -q '^snd_aloop'; then
        info "unloading snd-aloop"
        sudo modprobe -r snd-aloop || warn "could not unload snd-aloop (close PluginHost and the simulator first)"
    fi
    if [[ -f "$MODPROBE_CONF" || -f "$MODULES_LOAD_CONF" ]]; then
        info "removing boot configuration"
        sudo rm -f "$MODPROBE_CONF" "$MODULES_LOAD_CONF"
    fi
    remove_alias
    info "ALSA alias removed from ${ASOUNDRC}"
    remove_wireplumber_rule
}

status() {
    if card_loaded; then
        echo "card:        ${CARD_ID} loaded ($(readlink -f "/proc/asound/${CARD_ID}"))"
    else
        echo "card:        ${CARD_ID} not loaded"
    fi
    if [[ -f "$ASOUNDRC" ]] && grep -qF "$BEGIN_MARK" "$ASOUNDRC"; then
        echo "alsa alias:  stagecue_sim in ${ASOUNDRC}"
    else
        echo "alsa alias:  not installed"
    fi
    if [[ -f "$WIREPLUMBER_CONF" ]]; then
        echo "wireplumber: card ignored (${WIREPLUMBER_CONF})"
    else
        echo "wireplumber: no rule (PipeWire may grab the card)"
    fi
    if [[ -f "$MODPROBE_CONF" ]]; then
        echo "boot load:   yes (${MODPROBE_CONF})"
    else
        echo "boot load:   no"
    fi
}

# ---------------------------------------------------------------------------
# macOS: one patched BlackHole driver with two crossed devices
# ---------------------------------------------------------------------------
#
# PluginHost must open a single device for input and output: given two devices, JUCE
# bridges them with its AudioIODeviceCombiner, which underruns constantly on loopback
# devices (they report zero latency), so the outputs glitch or stay silent. The patch
# gives BlackHole's main device and its mirror (Device2) separate ring buffers, crossed:
#
#   sim side:   "StageCue Soundcard Sim (sim side)"   out -> ring A   in <- ring B
#   PluginHost: "StageCue Soundcard Sim"              in  <- ring A   out -> ring B
#
# Both devices share the driver's clock, so nothing drifts.

BLACKHOLE_REPO=https://github.com/ExistentialAudio/BlackHole.git
BLACKHOLE_TAG=v0.7.2
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BLACKHOLE_PATCH="${SCRIPT_DIR}/patches/blackhole-crossed-loopback.patch"
HAL_DIR=/Library/Audio/Plug-Ins/HAL
CACHE_DIR="${HOME}/Library/Caches/StageCueSoundcardSim"

# Must match SimConfig's hostDevice / inputsDevice / outputsDevice defaults.
MAC_HOST_DEVICE="StageCue Soundcard Sim"
MAC_SIM_DEVICE="StageCue Soundcard Sim (sim side)"
MAC_BUNDLE=StageCueSoundcardSim
MAC_BUNDLE_ID=org.indelible.stagecue.sim
MAC_FACTORY_UUID=817FC046-EE6B-43AD-BA8D-C92678821D22   # fixed, so the driver loads on its own
# The earlier two-driver setup; removed on install and --remove.
MAC_LEGACY_BUNDLES=(StageCueSimInputs StageCueSimOutputs)

mac_device_listed() { system_profiler SPAudioDataType 2>/dev/null | grep -qF "$1:"; }

mac_restart_coreaudio() {
    info "restarting coreaudiod - audio drops out for a moment"
    sudo killall coreaudiod 2>/dev/null || true
}

# Clones BlackHole once and applies the crossed-loopback patch (idempotent).
mac_fetch_source() {
    local src="$1"
    if [[ ! -d "$src" ]]; then
        info "fetching BlackHole ${BLACKHOLE_TAG}"
        mkdir -p "$CACHE_DIR"
        git clone --quiet --depth 1 --branch "$BLACKHOLE_TAG" "$BLACKHOLE_REPO" "$src" 2>/dev/null \
            || die "could not clone ${BLACKHOLE_REPO}"
    fi
    if git -C "$src" apply --reverse --check "$BLACKHOLE_PATCH" 2>/dev/null; then
        return 0   # already patched
    fi
    git -C "$src" apply "$BLACKHOLE_PATCH" || die "could not apply ${BLACKHOLE_PATCH} to ${src} (delete it and retry)"
}

mac_build_driver() {
    local src="$1" out="$2"
    local host_name="${MAC_HOST_DEVICE// /\\ }" sim_name="${MAC_SIM_DEVICE// /\\ }"

    info "building ${MAC_HOST_DEVICE}"
    xcodebuild -quiet \
        -project "${src}/BlackHole.xcodeproj" -configuration Release -target BlackHole \
        CONFIGURATION_BUILD_DIR="${out}" SYMROOT="${out}/sym" OBJROOT="${out}/obj" \
        PRODUCT_BUNDLE_IDENTIFIER="${MAC_BUNDLE_ID}" \
        CODE_SIGN_IDENTITY=- CODE_SIGN_STYLE=Manual DEVELOPMENT_TEAM= \
        GCC_PREPROCESSOR_DEFINITIONS="\$GCC_PREPROCESSOR_DEFINITIONS kNumber_Of_Channels=32 kHas_Driver_Name_Format=false kDriver_Name=\\\"${MAC_BUNDLE}\\\" kDevice_Name=\\\"${host_name}\\\" kDevice2_Name=\\\"${sim_name}\\\" kDevice2_IsHidden=false kPlugIn_BundleID=\\\"${MAC_BUNDLE_ID}\\\" kManufacturer_Name=\\\"StageCue\\\" kCanBeDefaultDevice=false kCanBeDefaultSystemDevice=false kEnableVolumeControl=false" \
        >/dev/null || die "building ${MAC_HOST_DEVICE} failed (run with a working Xcode: xcode-select -p)"

    local plist="${out}/BlackHole.driver/Contents/Info.plist"
    sed -i '' "s/e395c745-4eea-4d94-bb92-46224221047c/${MAC_FACTORY_UUID}/g" "$plist"
    rm -rf "${out}/${MAC_BUNDLE}.driver"
    mv "${out}/BlackHole.driver" "${out}/${MAC_BUNDLE}.driver"
    codesign --force --deep --sign - "${out}/${MAC_BUNDLE}.driver" >/dev/null 2>&1 \
        || die "ad-hoc signing ${MAC_BUNDLE}.driver failed"
}

# Removes the earlier two-driver setup. Returns 0 if anything was removed.
mac_remove_legacy() {
    local removed=1 bundle
    for bundle in "${MAC_LEGACY_BUNDLES[@]}"; do
        if [[ -d "${HAL_DIR}/${bundle}.driver" ]]; then
            info "removing the old ${HAL_DIR}/${bundle}.driver"
            sudo rm -rf "${HAL_DIR}/${bundle}.driver"
            removed=0
        fi
    done
    return $removed
}

mac_install() {
    local has_legacy=0 bundle
    for bundle in "${MAC_LEGACY_BUNDLES[@]}"; do
        [[ -d "${HAL_DIR}/${bundle}.driver" ]] && has_legacy=1
    done
    if [[ -d "${HAL_DIR}/${MAC_BUNDLE}.driver" && $has_legacy -eq 0 ]]; then
        info "loopback device already installed (--remove first to rebuild)"
        return 0
    fi

    command -v xcodebuild >/dev/null && xcode-select -p >/dev/null 2>&1 \
        || die "Xcode is required to build the loopback driver (install it from the App Store, then: sudo xcode-select -s /Applications/Xcode.app)"

    local src="${CACHE_DIR}/BlackHole-${BLACKHOLE_TAG}" out="${CACHE_DIR}/build"
    mac_fetch_source "$src"
    mkdir -p "$out"
    mac_build_driver "$src" "$out"

    info "installing into ${HAL_DIR} - sudo may ask for your password"
    sudo mkdir -p "$HAL_DIR"
    mac_remove_legacy || true
    sudo rm -rf "${HAL_DIR}/${MAC_BUNDLE}.driver"
    sudo cp -R "${out}/${MAC_BUNDLE}.driver" "${HAL_DIR}/"
    sudo chown -R root:wheel "${HAL_DIR}/${MAC_BUNDLE}.driver"
    mac_restart_coreaudio

    local name
    for name in "$MAC_HOST_DEVICE" "$MAC_SIM_DEVICE"; do
        for _ in $(seq 1 20); do
            mac_device_listed "$name" && break
            sleep 0.5
        done
        mac_device_listed "$name" || die "${name} is installed but CoreAudio does not list it yet.
coreaudiod loads HAL drivers one after another, and a third-party driver that hangs while
loading blocks every driver after it. Installed drivers: $(ls "$HAL_DIR" | grep '\.driver$' | tr '\n' ' ')
Temporarily move the suspect one out of ${HAL_DIR}, run 'sudo killall coreaudiod', then: $0 --status"
        info "${name} is ready"
    done
}

mac_remove() {
    local removed=0
    if [[ -d "${HAL_DIR}/${MAC_BUNDLE}.driver" ]]; then
        info "removing ${HAL_DIR}/${MAC_BUNDLE}.driver"
        sudo rm -rf "${HAL_DIR}/${MAC_BUNDLE}.driver"
        removed=1
    fi
    mac_remove_legacy && removed=1
    [[ $removed -eq 1 ]] && mac_restart_coreaudio
    rm -rf "${CACHE_DIR}/build"
    info "loopback device removed"
}

mac_status() {
    local installed="not installed" name bundle
    [[ -d "${HAL_DIR}/${MAC_BUNDLE}.driver" ]] && installed="installed (${HAL_DIR}/${MAC_BUNDLE}.driver)"
    printf '%-35s %s\n' "driver:" "$installed"
    for name in "$MAC_HOST_DEVICE" "$MAC_SIM_DEVICE"; do
        local listed="not listed"
        mac_device_listed "$name" && listed="listed by CoreAudio"
        printf '%-35s %s\n' "${name}:" "$listed"
    done
    for bundle in "${MAC_LEGACY_BUNDLES[@]}"; do
        [[ -d "${HAL_DIR}/${bundle}.driver" ]] \
            && printf '%-35s %s\n' "old ${bundle}.driver:" "still installed (run $0 to replace it)"
    done
    return 0
}

if [[ "$(uname -s)" == "Darwin" ]]; then
    case "${1:-}" in
        "")        mac_install ;;
        --persist) mac_install; info "HAL drivers load at every boot; nothing else to do" ;;
        --remove)  mac_remove ;;
        --status)  mac_status ;;
        -h|--help) sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//' ;;
        *)         die "unknown option: $1 (see --help)" ;;
    esac
    exit 0
fi

case "${1:-}" in
    "")        load_module; install_alias; install_wireplumber_rule ;;
    --persist) load_module; install_alias; install_wireplumber_rule; persist ;;
    --remove)  remove_all ;;
    --status)  status ;;
    -h|--help) sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//' ;;
    *)         die "unknown option: $1 (see --help)" ;;
esac
