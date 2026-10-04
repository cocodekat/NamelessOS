#!/usr/bin/env bash

set -u

die() {
    printf 'Error: %s\n' "$*" >&2
    exit 1
}

command_exists() {
    command -v "$1" >/dev/null 2>&1
}

choose_number() {
    local prompt="$1"
    local maximum="$2"
    local answer

    while :; do
        printf '%s' "$prompt" >&2
        IFS= read -r answer || exit 1
        case "$answer" in
            *[!0-9]*|'') printf 'Enter a number from 1 to %s.\n' "$maximum" >&2 ;;
            *)
                if [ "$answer" -ge 1 ] && [ "$answer" -le "$maximum" ]; then
                    printf '%s\n' "$answer"
                    return 0
                fi
                printf 'Enter a number from 1 to %s.\n' "$maximum" >&2
                ;;
        esac
    done
}

platform=$(uname -s)
disks=()

case "$platform" in
    Darwin)
        command_exists diskutil || die "diskutil is required on macOS"
        while IFS= read -r disk; do
            [ -n "$disk" ] && disks+=("$disk")
        done < <(diskutil list external physical 2>/dev/null |
                 awk '/^\/dev\/disk[0-9]+ / { print $1 }')
        ;;
    Linux)
        command_exists lsblk || die "lsblk is required on Linux"
        while IFS= read -r disk; do
            [ -n "$disk" ] && disks+=("$disk")
        done < <(lsblk -dpno NAME,TYPE,TRAN,RM |
                 awk '$2 == "disk" && ($3 == "usb" || $4 == "1") { print $1 }')
        ;;
    *)
        die "unsupported operating system: $platform"
        ;;
esac

if [ "${#disks[@]}" -eq 0 ]; then
    die "no external/removable whole disks found; connect the USB drive and retry"
fi

printf 'External/removable disks:\n\n'
for ((i = 0; i < ${#disks[@]}; i++)); do
    printf '  %d) %s\n' "$((i + 1))" "${disks[$i]}"
    if [ "$platform" = Darwin ]; then
        diskutil info "${disks[$i]}" 2>/dev/null |
            awk -F: '/Media Name|Disk Size|Protocol/ {
                sub(/^[[:space:]]+/, "", $2); printf "       %s: %s\n", $1, $2
            }'
    else
        lsblk -dno SIZE,MODEL,TRAN "${disks[$i]}" 2>/dev/null |
            awk '{$1=$1; print "       " $0}'
    fi
done

disk_choice=$(choose_number "Select the target disk: " "${#disks[@]}")
target_disk=${disks[$((disk_choice - 1))]}

# Never permit selecting the disk that contains the running OS, even when
# that OS itself was booted from an external/removable drive.
if [ "$platform" = Darwin ]; then
    root_whole=$(diskutil info / 2>/dev/null |
                 awk -F: '/Part of Whole/ {gsub(/[[:space:]]/, "", $2); print "/dev/" $2}')
    [ "$target_disk" != "$root_whole" ] || die "refusing to overwrite the current macOS startup disk"
else
    root_source=$(findmnt -nro SOURCE / 2>/dev/null || true)
    case "$root_source" in
        "$target_disk"|"$target_disk"[0-9]*|"$target_disk"p[0-9]*)
            die "refusing to overwrite the current Linux root disk"
            ;;
    esac
fi

isos=()
while IFS= read -r iso; do
    [ -n "$iso" ] && isos+=("$iso")
done < <(find "$PWD" -maxdepth 3 -type f -iname '*.iso' -print 2>/dev/null | sort)

selected_iso=''
if [ "${#isos[@]}" -gt 0 ]; then
    printf '\nISO images found:\n\n'
    for ((i = 0; i < ${#isos[@]}; i++)); do
        printf '  %d) %s (%s)\n' "$((i + 1))" "${isos[$i]}" \
            "$(du -h "${isos[$i]}" | awk '{print $1}')"
    done
    printf '  %d) Enter another path\n' "$((${#isos[@]} + 1))"
    iso_choice=$(choose_number "Select the ISO: " "$((${#isos[@]} + 1))")
    if [ "$iso_choice" -le "${#isos[@]}" ]; then
        selected_iso=${isos[$((iso_choice - 1))]}
    fi
fi

if [ -z "$selected_iso" ]; then
    printf 'Path to ISO: '
    IFS= read -r selected_iso || exit 1
fi

[ -f "$selected_iso" ] || die "ISO does not exist: $selected_iso"
case "${selected_iso##*.}" in
    iso|ISO) ;;
    *) die "selected file does not end in .iso" ;;
esac

selected_iso=$(cd "$(dirname "$selected_iso")" && pwd -P)/$(basename "$selected_iso")

# dd cannot continue reliably after unmounting the same disk that holds its
# input image, and choosing it is almost always accidental.
if [ "$platform" = Darwin ]; then
    iso_device=$(df "$selected_iso" | awk 'NR == 2 {print $1}')
    case "$iso_device" in
        "$target_disk"|"$target_disk"s[0-9]*)
            die "the selected ISO is stored on the target disk; copy it elsewhere first"
            ;;
    esac
else
    iso_device=$(findmnt -nro SOURCE -T "$selected_iso" 2>/dev/null || true)
    case "$iso_device" in
        "$target_disk"|"$target_disk"[0-9]*|"$target_disk"p[0-9]*)
            die "the selected ISO is stored on the target disk; copy it elsewhere first"
            ;;
    esac
fi

printf '\nWARNING: this permanently overwrites the entire target disk.\n'
printf '  Source: %s\n' "$selected_iso"
printf '  Target: %s\n\n' "$target_disk"
printf 'Type exactly "FLASH %s" to continue: ' "$target_disk"
IFS= read -r confirmation || exit 1
[ "$confirmation" = "FLASH $target_disk" ] || die "confirmation did not match; nothing was written"

if [ "$platform" = Darwin ]; then
    diskutil unmountDisk "$target_disk" || die "could not unmount $target_disk"
    raw_target=${target_disk/\/dev\/disk/\/dev\/rdisk}
    printf '\nFlashing with dd (macOS shows the final count when complete)...\n'
    sudo dd if="$selected_iso" of="$raw_target" bs=4m || die "dd failed"
else
    while IFS= read -r node; do
        [ -n "$node" ] && sudo umount "$node" 2>/dev/null || true
    done < <(lsblk -lnpo NAME "$target_disk" | tail -n +2)
    printf '\nFlashing with dd...\n'
    sudo dd if="$selected_iso" of="$target_disk" bs=4M status=progress conv=fsync || die "dd failed"
fi

sync
printf '\nFlash completed successfully on %s.\n' "$target_disk"
diskutil eject $target_disk
