# Barista GamePad motion support

This Cemu fork adds gyroscope and accelerometer input from a physical Wii U
GamePad connected through [Barista](https://github.com/BetaZay/Barista). It keeps
the existing Barista video, audio, buttons, sticks, and touch integration and
feeds the full 180 Hz input-report stream into Cemu's motion-fusion code.

The default orientation was tested on real hardware with motion aiming in *The
Legend of Zelda: The Wind Waker HD*. This remains experimental software and is
not affiliated with Nintendo, Barista, or upstream Cemu.

## Requirements

- Linux on x86_64.
- A physical Wii U GamePad.
- A Barista-compatible 5 GHz Wi-Fi adapter. Barista takes control of this
  adapter while the GamePad is connected, so use Ethernet or a second Wi-Fi
  adapter for Internet access.
- Barista installed and able to connect the GamePad.

Barista's Preview packages can currently be installed with:

```sh
curl -fsSL https://betazay.github.io/Barista/install.sh | sudo sh -s -- preview
```

Follow Barista's own documentation for supported distributions, pairing, Wi-Fi
hardware, and package security. Always run the Barista desktop application and
Cemu as your normal user, not with `sudo`.

## Install the AppImage (recommended)

Download the `x86_64.AppImage` file from the
[latest release](https://github.com/Nerostalgeek/Cemu-Barista-Motion/releases/latest),
make it executable, and run it:

```sh
chmod +x Cemu-Barista-Motion-*-x86_64.AppImage
./Cemu-Barista-Motion-*-x86_64.AppImage
```

There is nothing else to install for this Cemu fork. Barista remains a separate
application and must be running with the GamePad connected before Cemu starts.

## Build this fork

Building from source is optional. It requires the normal Cemu build
dependencies listed in [BUILD.md](BUILD.md).

```sh
git clone --recursive https://github.com/Nerostalgeek/Cemu-Barista-Motion.git
cd Cemu-Barista-Motion
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=/usr/bin/clang \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++
cmake --build build --parallel
```

The binary is written to `bin/Cemu_release`.

## Configure Cemu

1. Open Barista and connect the GamePad.
2. Start this Cemu build as your normal desktop user.
3. In **Options -> General settings -> Barista**, keep the bridge enabled and
   leave the socket path empty to use `/run/barista/media-<uid>.sock`.
4. In **Options -> Input settings**, select **Wii U GamePad** as the emulated
   controller, add the **WiiUGamePad** input API device, and save the profile.
5. Launch a game that supports GamePad motion.

The controller advertises motion support automatically. Existing profiles may
need to be opened and saved once if they explicitly stored motion as disabled.

The supplied launcher checks the default socket and starts the locally built
binary with the hardware-tested orientation:

```sh
./scripts/run-barista-motion.sh
```

Pass normal Cemu arguments after the script. Set `CEMU_BIN` to use a binary in
another location:

```sh
CEMU_BIN=/opt/cemu/Cemu_release ./scripts/run-barista-motion.sh -f
```

## Motion calibration

The following environment variables are read once when Cemu receives its first
motion sample:

| Variable | Default | Accepted values | Purpose |
| --- | --- | --- | --- |
| `CEMU_BARISTA_AXIS_ROTATION` | `rz+90` | `rz+90`, `rz-90`, `original`, `none` | Rotates accelerometer and gyroscope axes together. |
| `CEMU_BARISTA_INVERT_PITCH` | `1` | `0`, `false`, `off` to disable | Corrects forward/back aiming and its gravity component. |
| `CEMU_BARISTA_INVERT_ROLL` | `1` | `0`, `false`, `off` to disable | Corrects left/right roll and its gravity component. |
| `CEMU_BARISTA_INVERT_YAW` | `1` | `0`, `false`, `off` to disable | Corrects left/right yaw. Only gyro Z is changed because gravity has no yaw reference. |
| `CEMU_BARISTA_GYRO_GAIN` | `1` | Number from `0.25` to `16` | Multiplies angular velocity after axis calibration. |
| `CEMU_BARISTA_MOTION_CSV` | unset | Writable file path | Records sensor-only diagnostic CSV data. No buttons or touch data are saved. |

For example, to keep the tested rotation but restore the un-inverted yaw:

```sh
CEMU_BARISTA_INVERT_YAW=0 ./scripts/run-barista-motion.sh
```

To compare the raw Barista orientation without the tested transform:

```sh
CEMU_BARISTA_AXIS_ROTATION=original ./scripts/run-barista-motion.sh
```

Close and restart Cemu after changing any calibration variable.

## How it works

Barista sends 128-byte GamePad input reports over its per-user AppHook Unix
socket. The connector keeps an ordered queue instead of exposing only the most
recent report. Cemu then:

1. decodes the 16-bit accelerometer and 24-bit gyroscope fields;
2. accounts for report sequence gaps at the GamePad's 180 Hz sample rate;
3. applies one coherent axis rotation to both sensors;
4. applies the independently configurable pitch, roll, and yaw corrections;
5. passes the result to Cemu's existing `WiiUMotionHandler` sensor fusion; and
6. resets the filter whenever the Barista socket reconnects.

Compile-time fixtures cover signed decoding, gyro-axis mapping, packet sequence
handling, rotations, and all three inversion controls.

## Troubleshooting

### Cemu does not see the GamePad

- Confirm Barista says the GamePad is connected.
- Open **General settings -> Barista** and confirm the bridge is enabled and the
  status is connected.
- Check the socket exists:

  ```sh
  test -S "/run/barista/media-$(id -u).sock" && echo connected
  ```

- Do not run Cemu as root. The default socket belongs to the desktop user.
- If Barista uses a non-default endpoint, set `BARISTA_MUG_SOCKET` before
  starting Cemu or select that socket in Cemu's Barista settings.

### Buttons work but motion does not

- Reopen the input profile and ensure motion is enabled for the Wii U GamePad
  device, then save it.
- Fully quit all Cemu processes before retrying; environment changes do not
  affect an already-running process.
- Start with the supplied launcher, which sets the validated defaults.

### An axis moves in the wrong direction

Disable only the corresponding inversion first. If pitch and roll are exchanged,
try `CEMU_BARISTA_AXIS_ROTATION=rz-90`. Use `original` only for diagnosis because
it bypasses all three tested inversion corrections.

## Current limitations

- The tested defaults were validated with one physical GamePad and need broader
  hardware and game coverage.
- Combined 3D rotations may expose calibration or sensor-fusion drift that is
  not visible during simple single-axis aiming tests.
- Camera, microphone, and NFC are outside this motion patch.
- The Barista bridge is Linux-only even though upstream Cemu supports other
  operating systems.

Please include the Cemu commit, Barista version, game, axis settings, and exact
physical movement when reporting a motion issue.
