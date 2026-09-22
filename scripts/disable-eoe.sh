#!/usr/bin/env bash
# Temporarily disable only the selected IgH master's EoE virtual interfaces.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: sudo ./scripts/disable-eoe.sh [--master-id ID] [--dry-run]

  --master-id ID  Non-negative decimal master ID (default: 0).
  --dry-run      List matching interfaces and commands without changing them.
  -h, --help     Show this help.

Stop the master application before use. This disables EoE/IP access to the
selected slaves, not the EtherCAT physical interface or PDO communication.
Matches eoe<ID>s<position> and eoe<ID>a<alias>. Changes are temporary;
network managers or interface recreation may bring the interfaces up again.
Restore an interface with: sudo ip link set dev <interface> up
EOF
}

master_id=0
dry_run=false
master_seen=false
while (($#)); do
    case "$1" in
        --master-id)
            if [[ $master_seen == true || $# -lt 2 || ! $2 =~ ^[0-9]+$ ]]; then
                echo 'Error: --master-id requires one non-negative decimal ID, without duplicates.' >&2
                exit 2
            fi
            master_id=$2
            # Normalize leading zeros without arithmetic overflow.
            while [[ ${#master_id} -gt 1 && $master_id == 0* ]]; do
                master_id=${master_id:1}
            done
            master_seen=true
            shift 2
            ;;
        --dry-run) dry_run=true; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Error: unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

interfaces=()
for path in /sys/class/net/eoe*; do
    [[ -e $path ]] || continue
    name=${path##*/}
    if [[ $name =~ ^eoe${master_id}[sa][0-9]+$ ]]; then
        interfaces+=("$name")
    fi
done

if ((${#interfaces[@]} == 0)); then
    echo "No EoE interfaces found for master $master_id; nothing changed."
    exit 0
fi

if [[ $dry_run == false ]]; then
    if ((EUID != 0)); then
        echo 'Error: run with sudo, or use --dry-run to preview.' >&2
        exit 1
    fi
    command -v ip >/dev/null || { echo 'Error: ip (iproute2) is required.' >&2; exit 1; }
fi

failed=0
for name in "${interfaces[@]}"; do
    if [[ $dry_run == true ]]; then
        printf 'Would run: ip link set dev %s down\n' "$name"
    elif ip link set dev "$name" down; then
        printf 'Disabled EoE interface: %s\n' "$name"
    else
        printf 'Error: failed to disable %s\n' "$name" >&2
        failed=1
    fi
done
exit "$failed"
