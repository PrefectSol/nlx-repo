#!/usr/bin/env bash

set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    exec sudo "$0" "$@"
fi

GRUB_FILE="/etc/default/grub"
GRUB_PARAM="GRUB_CMDLINE_LINUX_DEFAULT"
GRUB_CFG="/boot/grub/grub.cfg"

if [[ ! -f "$GRUB_FILE" ]]; then
    echo "$GRUB_FILE not found" >&2
    exit 1
fi

if ! command -v grub-mkconfig &>/dev/null; then
    echo "grub-mkconfig not found" >&2
    exit 1
fi

total_cpus() {
    nproc --all
}

active_isolated() {
    local cmdline
    cmdline=$(cat /proc/cmdline)
    if [[ "$cmdline" =~ isolcpus=([0-9,]+) ]]; then
        echo "${BASH_REMATCH[1]}"
    fi
}

pending_isolated() {
    local line
    line=$(grep "^${GRUB_PARAM}=" "$GRUB_FILE" || true)
    if [[ "$line" =~ isolcpus=([0-9,]+) ]]; then
        echo "${BASH_REMATCH[1]}"
    fi
}

validate_cpus() {
    local cpus="$1"
    local total
    total=$(total_cpus)

    if [[ ! "$cpus" =~ ^[0-9]+(,[0-9]+)*$ ]]; then
        echo "invalid format: use comma-separated numbers" >&2
        return 1
    fi

    local IFS=','
    for cpu in $cpus; do
        if (( cpu >= total )); then
            echo "cpu $cpu out of range (0-$((total - 1)))" >&2
            return 1
        fi
    done
}

merge_cpus() {
    local a="$1"
    local b="$2"
    echo "${a},${b}" | tr ',' '\n' | grep -v '^$' | sort -nu | paste -sd,
}

remove_cpus() {
    local a="$1"
    local b="$2"
    local IFS=','
    local result=""
    for x in $a; do
        local keep=1
        for y in $b; do
            if [[ "$x" == "$y" ]]; then
                keep=0
                break
            fi
        done
        if (( keep )); then
            result="${result},${x}"
        fi
    done
    echo "${result#,}"
}

diff_cpus() {
    local a="$1"
    local b="$2"
    local IFS=','
    local result=""
    for x in $a; do
        local found=0
        for y in $b; do
            if [[ "$x" == "$y" ]]; then
                found=1
                break
            fi
        done
        if (( ! found )); then
            result="${result},${x}"
        fi
    done
    echo "${result#,}"
}

write_grub() {
    local cpus="$1"
    local line
    line=$(grep "^${GRUB_PARAM}=" "$GRUB_FILE" || echo "${GRUB_PARAM}=\"\"")

    local cleaned
    cleaned=$(echo "$line" | \
        sed -E 's/ ?isolcpus=[0-9,]+//g' | \
        sed -E 's/ ?nohz_full=[0-9,]+//g' | \
        sed -E 's/ ?rcu_nocbs=[0-9,]+//g')

    local value
    value=$(echo "$cleaned" | sed -E "s/^${GRUB_PARAM}=\"?//;s/\"?$//")

    if [[ -n "$cpus" ]]; then
        value="${value} isolcpus=${cpus} nohz_full=${cpus} rcu_nocbs=${cpus}"
    fi

    value=$(echo "$value" | sed 's/  */ /g; s/^ //; s/ $//')

    local new_line="${GRUB_PARAM}=\"${value}\""

    if grep -q "^${GRUB_PARAM}=" "$GRUB_FILE"; then
        sed -i "s|^${GRUB_PARAM}=.*|${new_line}|" "$GRUB_FILE"
    else
        echo "$new_line" >> "$GRUB_FILE"
    fi

    grub-mkconfig -o "$GRUB_CFG" >/dev/null
}

cmd_status() {
    local total
    total=$(total_cpus)
    local active
    active=$(active_isolated)
    local pending
    pending=$(pending_isolated)

    local to_add
    to_add=$(diff_cpus "$pending" "$active")
    local to_remove
    to_remove=$(diff_cpus "$active" "$pending")

    echo "total cpus:   $total"
    echo "active:       ${active:-none}"
    echo "to isolate:   ${to_add:-none}"
    echo "to release:   ${to_remove:-none}"

    if [[ "$active" != "$pending" ]]; then
        echo "reboot required"
    fi
}

cmd_enable() {
    local cpus="$1"
    validate_cpus "$cpus"

    local current
    current=$(pending_isolated)
    local merged
    merged=$(merge_cpus "$current" "$cpus")

    local total
    total=$(total_cpus)
    local count
    count=$(echo "$merged" | tr ',' '\n' | wc -l)
    if (( count >= total )); then
        echo "cannot isolate all cpus" >&2
        exit 1
    fi

    write_grub "$merged"

    echo "isolated: $merged"
    echo "reboot required"
}

cmd_disable() {
    local target="$1"
    validate_cpus "$target"

    local current
    current=$(pending_isolated)

    if [[ -z "$current" ]]; then
        echo "nothing to disable"
        return
    fi

    local result
    result=$(remove_cpus "$current" "$target")

    write_grub "$result"

    echo "isolated: ${result:-none}"
    echo "reboot required"
}

case "${1:-}" in
    status)
        cmd_status
        ;;
    enable)
        [[ -z "${2:-}" ]] && { echo "usage: $0 enable <cpus>" >&2; exit 1; }
        cmd_enable "$2"
        ;;
    disable)
        [[ -z "${2:-}" ]] && { echo "usage: $0 disable <cpus>" >&2; exit 1; }
        cmd_disable "$2"
        ;;
    *)
        echo "usage: $0 {status | enable <cpus> | disable <cpus>}" >&2
        exit 1
        ;;
esac
