# zcfan

Zero-configuration fan control daemon for ThinkPads.

## Features

- No configuration
- Strong focus on stopping the fan as soon as safe to do so
- Temperature hysteresis support: no bouncing between fan levels
- Minimal resource usage
- Simple and easy to understand code

## Installation

1. Load your thinkpad_acpi module with `fan_control=1`;
2. Run `zcfan` as root.
