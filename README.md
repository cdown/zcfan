# zcfan

Zero-configuration fan control daemon for ThinkPads.

## Features

- Extremely small, simple, and easy to understand code
- Sensible out of the box, configuration is optional (see "usage" below)
- Strong focus on stopping the fan as soon as safe to do so
- Automatic temperature- and time-based hysteresis: no bouncing between fan
  levels
- Minimal resource usage

## Usage

zcfan has the following default fan states:

| Config name | thinkpad_acpi fan level | Default trip temperature (C) |
|-------------|-------------------------|------------------------------|
| max_temp    | 7                       | 90                           |
| med_temp    | 4                       | 80                           |
| low_temp    | 2                       | 70                           |

If no trip temperature is reached, the fan will be turned off.

To override these defaults, you can place a file at `/etc/zcfan.conf`. As an
example:

    max_temp 85
    med_temp 70
    low_temp 55

## Installation

1. Load your thinkpad_acpi module with `fan_control=1`;
2. Run `zcfan` as root.
