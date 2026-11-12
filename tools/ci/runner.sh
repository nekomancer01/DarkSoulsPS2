#!/bin/bash
# Usage: tools/ci/runner.sh install [<version> <sha256>] | uninstall | status
# The self-hosted GitHub Actions runner for .github/workflows/pcsx2-smoke.yml, on the maintainer's PC (the one with
# the game data, the PS2 toolchain and PCSX2). Run it inside the build distrobox; needs `gh` logged in as a repo admin
# (it fetches the registration / removal token itself).
#   install    downloads actions/runner into $RUNNER_DIR (default ~/DarkSoulsPS2/runner, git-ignored via
#              .git/info/exclude), checks the release's SHA-256, registers it with the labels self-hosted,dsr-pcsx2,
#              and installs a host systemd user unit (dsr-runner.service) that starts it inside the distrobox
#              (`distrobox enter -T <box> --`), restarted on failure and at login
#   uninstall  stops the unit, removes it, unregisters the runner
#   status     unit state + the runner as GitHub sees it
# Env: DSR_MAIN (main checkout, default ~/DarkSoulsPS2), RUNNER_DIR, DSR_BOX (distrobox name, default Ubuntu),
#      REPO (default nekomancer01/DarkSoulsPS2).
set -e
MAIN=${DSR_MAIN:-$HOME/DarkSoulsPS2}
DIR=${RUNNER_DIR:-$MAIN/runner}
BOX=${DSR_BOX:-Ubuntu}
REPO=${REPO:-nekomancer01/DarkSoulsPS2}
NAME=${RUNNER_NAME:-$(hostname -s)-pcsx2}
UNIT=dsr-runner.service
HOSTHOME=$(distrobox-host-exec sh -c 'echo $HOME' 2>/dev/null || echo "$HOME")
UDIR=$HOSTHOME/.config/systemd/user
hsys() { if [ -e /run/.containerenv ]; then distrobox-host-exec systemctl --user "$@"; else systemctl --user "$@"; fi; }
token() { gh api -X POST "repos/$REPO/actions/runners/$1-token" --jq .token; }
case $1 in
    install)
        VER=${2:-2.338.2}; SUM=${3:-baebf204307bac6193480852732f0aba2ae97b6c5597c244f4d5558390147c40}
        mkdir -p "$DIR"; cd "$DIR"
        grep -qx '/runner/' "$MAIN/.git/info/exclude" 2>/dev/null || echo '/runner/' >> "$MAIN/.git/info/exclude"
        if [ ! -x ./config.sh ]; then
            T=actions-runner-linux-x64-$VER.tar.gz
            curl -fsSL -o "$T" "https://github.com/actions/runner/releases/download/v$VER/$T"
            echo "$SUM  $T" | sha256sum -c -
            tar xzf "$T" && rm -f "$T"
        fi
        [ -f .runner ] || ./config.sh --unattended --url "https://github.com/$REPO" --token "$(token registration)" \
            --name "$NAME" --labels dsr-pcsx2 --work _work --replace
        # the job's shell: same PATH / HOME as an interactive distrobox shell; DSR_MAIN for the workflow
        printf 'DSR_MAIN=%s\nLANG=C.UTF-8\n' "$MAIN" > .env
        mkdir -p "$UDIR"
        cat > "$UDIR/$UNIT" <<EOF
[Unit]
Description=GitHub Actions runner for DarkSoulsPS2 (PCSX2 smoke, inside the $BOX distrobox)
After=network-online.target

[Service]
ExecStart=/usr/bin/distrobox enter -T $BOX -- $DIR/run.sh
Restart=always
RestartSec=30
KillMode=mixed
TimeoutStopSec=60

[Install]
WantedBy=default.target
EOF
        hsys daemon-reload
        hsys enable --now "$UNIT"
        hsys status "$UNIT" --no-pager | head -5
        ;;
    uninstall)
        hsys disable --now "$UNIT" 2>/dev/null || true
        rm -f "$UDIR/$UNIT"; hsys daemon-reload
        [ -f "$DIR/.runner" ] && (cd "$DIR" && ./config.sh remove --token "$(token remove)")
        ;;
    status)
        hsys status "$UNIT" --no-pager | head -5 || true
        gh api "repos/$REPO/actions/runners" --jq '.runners[] | "\(.name) \(.status) busy=\(.busy) \([.labels[].name] | join(","))"'
        ;;
    *) sed -n '2p' "$0"; exit 2 ;;
esac
