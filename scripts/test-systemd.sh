#!/usr/bin/env bash
# Generate or start a transient instance with the packaged unit's sandbox.
set -euo pipefail

case "${1:---print}" in
    --print|--run) mode="${1:---print}" ;;
    *) echo "Usage: $0 [--print|--run]" >&2; exit 2 ;;
esac
if [[ $# -gt 1 ]]; then
    echo "Usage: $0 [--print|--run]" >&2
    exit 2
fi

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
unit="$script_dir/../packaging/systemd/sshforum.service"
if [[ ! -f "$unit" ]]; then
    unit=/usr/lib/systemd/system/sshforum.service
fi
if [[ ! -f "$unit" ]]; then
    echo "Cannot find sshforum.service; run from the source tree or install the RPM." >&2
    exit 1
fi

command=(sudo systemd-run --unit=sshforum-test --collect --service-type=exec)
in_service=false
continued=''
while IFS= read -r line || [[ -n "$line" ]]; do
    line="${line%$'\r'}"
    if [[ "$line" == *\\ ]]; then
        continued+="${line%\\} "
        continue
    fi
    line="$continued$line"
    continued=''
    case "$line" in
        '[Service]') in_service=true; continue ;;
        '['*']') in_service=false; continue ;;
        ''|'#'*|';'*) continue ;;
    esac
    if [[ "$in_service" != true || "$line" != *=* ]]; then continue; fi
    key="${line%%=*}"
    # Replace instance identity and launch parameters, retaining all sandbox
    # properties. Never eval unit contents or expand their environment syntax.
    case "$key" in
        Type|ExecStart|Environment|User|Group|StateDirectory|WorkingDirectory|Restart|SocketBindAllow)
            continue ;;
    esac
    line="${line//\/var\/lib\/sshforum/\/var\/lib\/sshforum-test}"
    command+=("--property=$line")
done < "$unit"

command+=(
    --property=StateDirectory=sshforum-test
    --property=WorkingDirectory=/var/lib/sshforum-test
    --property=Restart=no
    --property=RuntimeMaxSec=10min
    --property=SocketBindAllow=ipv4:tcp:22222
    --property=SocketBindAllow=ipv6:tcp:22222
    /usr/bin/sshforum --bind 127.0.0.1 --port 22222
    --db /var/lib/sshforum-test/forum.db
    --host-key /var/lib/sshforum-test/ssh_host_ed25519_key
    --max-sessions 128
)

if [[ "$mode" == --print ]]; then
    printf '%q ' "${command[@]}"
    printf '\n'
else
    if [[ ! -x /usr/bin/sshforum ]]; then
        echo "Install the RPM on this host first: /usr/bin/sshforum is required." >&2
        exit 1
    fi
    "${command[@]}"
    echo 'Connect: ssh -tt -p 22222 anonymous@127.0.0.1'
    echo 'Logs: sudo journalctl -u sshforum-test.service -f'
    echo 'Stop: sudo systemctl stop sshforum-test.service'
fi
