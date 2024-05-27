# zcfan | [![Tests](https://img.shields.io/github/actions/workflow/status/cdown/zcfan/ci.yml?branch=master)](https://github.com/cdown/zcfan/actions?query=branch%3Amaster)

Zero-configuration fan control daemon for ThinkPads.

## Features

- Extremely small (~250 lines), simple, and easy to understand code
- Sensible out of the box, configuration is optional (see "usage" below)
- Strong focus on stopping the fan as soon as safe to do so, without inducing
  throttling
- Automatic temperature- and time-based hysteresis: no bouncing between fan
  levels
- Watchdog support
- Minimal resource usage
- No dependencies

## Usage

zcfan has the following default fan states:

| Config name | thinkpad_acpi fan level           | Default trip temperature (C) |
|-------------|-----------------------------------|------------------------------|
| max_temp    | full-speed (or 7 if unsupported)  | 90                           |
| med_temp    | 4                                 | 80                           |
| low_temp    | 1                                 | 70                           |

If no trip temperature is reached, the fan will be turned off.

The fan will also only be reduced once the temperature is now at least 10C
below the trip temperature for the current fan state. This can be tuned with
the config parameter `temp_hysteresis`.

To override these defaults, you can place a file at `/etc/zcfan.conf` with
updated trip temperatures in degrees celsius and/or fan levels. As an example:

    max_temp 85
    med_temp 70
    low_temp 55
    temp_hysteresis 20

    max_level full-speed
    med_level 4
    low_level 1

### Hysteresis

We will only reduce the fan level again once:

1. The temperature is now at least `temp_hysteresis` Celsius (default 10C)
   below the trip point, and
2. At least 3 seconds have elapsed since the initial trip.

This avoids unnecessary fluctuations in fan speed.

## Comparison with thinkfan

I wrote zcfan because I found thinkfan's configuration and code complexity too
much for my tastes. Use whichever suits your needs.

## Compilation

Run `make`.

## Installation

1. Compile zcfan or install from the [AUR
   package](https://aur.archlinux.org/packages/zcfan)
2. Load your thinkpad_acpi module with `fan_control=1`
    - At runtime: `rmmod thinkpad_acpi && modprobe thinkpad_acpi fan_control=1`
    - By default: `echo options thinkpad_acpi fan_control=1 > /etc/modprobe.d/99-fancontrol.conf`
3. Run `zcfan` as root (or use the `zcfan` systemd service provided)

## Disclaimer

While the author uses this on their own machine, obviously there is no warranty
whatsoever.
