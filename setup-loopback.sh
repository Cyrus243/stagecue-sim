#!/usr/bin/env bash
# Loads the ALSA loopback card used by StageCueSoundcardSim and registers a
# user-level ALSA alias so PluginHost lists it as "StageCue Soundcard Sim".
#
#   ./setup-loopback.sh            load snd-aloop (sudo) + install the ALSA alias
#   ./setup-loopback.sh --persist  same, and load the card at every boot
#   ./setup-loopback.sh --remove   unload snd-aloop, drop the boot config and the alias
#   ./setup-loopback.sh --status   show what is installed
#
# Env: CARD_ID (default StageCueSim), ASOUNDRC (default ~/.asoundrc)
set -euo pipefail

CARD_ID="${CARD_ID:-StageCueSim}"
ASOUNDRC="${ASOUNDRC:-$HOME/.asoundrc}"
MODPROBE_CONF=/etc/modprobe.d/stagecue-soundcard-sim.conf
MODULES_LOAD_CONF=/etc/modules-load.d/stagecue-soundcard-sim.conf
BEGIN_MARK="# >>> stagecue-soundcard-sim >>>"
END_MARK="# <<< stagecue-soundcard-sim <<<"
MODULE_OPTIONS="id=${CARD_ID} pcm_substreams=1"

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
    if [[ -f "$MODPROBE_CONF" ]]; then
        echo "boot load:   yes (${MODPROBE_CONF})"
    else
        echo "boot load:   no"
    fi
}

case "${1:-}" in
    "")        load_module; install_alias ;;
    --persist) load_module; install_alias; persist ;;
    --remove)  remove_all ;;
    --status)  status ;;
    -h|--help) sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//' ;;
    *)         die "unknown option: $1 (see --help)" ;;
esac
