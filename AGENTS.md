# LLM Development Guide

Instructions for building, running, and validating the two components of this project.

## ESP32 Firmware (C++)

The firmware lives in `src/` and is built with [PlatformIO](https://platformio.org/).

### Platform IO

On windows, the system should attempt to execute it with
```bash
& "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run 2>&1
```

### Build

```bash
pio run
```

This compiles the firmware for the `esp32-s3-devkitc-1` board using the Arduino framework.

### Upload to device

```bash
pio run --target upload
```

### Monitor serial output

```bash
pio device monitor
```

### Clean build artifacts

```bash
pio run --target clean
```

### Validate (compile check)

A successful `pio run` with exit code 0 confirms the C++ compiles without errors. There is no unit test suite for the firmware at this time.

---

## Python Listener

The listener application lives in `listener/` and uses [Poetry](https://python-poetry.org/) for dependency management.

### Install dependencies

```bash
cd listener
poetry install
```

### Run the listener

```bash
cd listener
poetry run birdnet-listener
```

### Validate

```bash
cd listener
poetry check          # validates pyproject.toml structure
poetry install        # ensures all deps resolve and install cleanly
poetry run python -c "import birdnet_listener"  # verifies the package imports successfully
```

If linting or type-checking tools are added later (e.g. ruff, mypy), run them via:

```bash
poetry run ruff check src/
poetry run mypy src/
```

### Docker (alternative)

```bash
cd listener
docker compose up --build
```

---

## Quick full-project validation

From the repository root:

```bash
# Firmware
pio run

# Listener
cd listener && poetry install && poetry check && poetry run python -c "import birdnet_listener"
```

Both commands exiting with code 0 means the project is in a valid state.
