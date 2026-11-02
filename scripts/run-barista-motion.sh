#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd -- "$script_dir/.." && pwd)"

if [[ -n "${CEMU_BIN:-}" ]]; then
	cemu_bin="$CEMU_BIN"
elif [[ -x "$repo_root/bin/Cemu_release" ]]; then
	cemu_bin="$repo_root/bin/Cemu_release"
elif [[ -x "$repo_root/bin/Cemu_debug" ]]; then
	cemu_bin="$repo_root/bin/Cemu_debug"
else
	printf 'Cemu binary not found. Build the fork first or set CEMU_BIN.\n' >&2
	exit 1
fi

if [[ ! -x "$cemu_bin" ]]; then
	printf 'Cemu binary is not executable: %s\n' "$cemu_bin" >&2
	exit 1
fi

barista_socket="${BARISTA_MUG_SOCKET:-/run/barista/media-$(id -u).sock}"
if [[ ! -S "$barista_socket" ]]; then
	printf 'Warning: Barista socket not found: %s\n' "$barista_socket" >&2
	printf 'Connect the GamePad in Barista; Cemu will keep trying to reconnect.\n' >&2
fi

export CEMU_BARISTA_GYRO_GAIN="${CEMU_BARISTA_GYRO_GAIN:-1}"
export CEMU_BARISTA_AXIS_ROTATION="${CEMU_BARISTA_AXIS_ROTATION:-rz+90}"
export CEMU_BARISTA_INVERT_PITCH="${CEMU_BARISTA_INVERT_PITCH:-1}"
export CEMU_BARISTA_INVERT_ROLL="${CEMU_BARISTA_INVERT_ROLL:-1}"
export CEMU_BARISTA_INVERT_YAW="${CEMU_BARISTA_INVERT_YAW:-1}"

exec "$cemu_bin" "$@"
