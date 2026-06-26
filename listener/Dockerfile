FROM python:3.12-slim AS base

# Python
ENV PYTHONFAULTHANDLER=1 \
    PYTHONUNBUFFERED=1 \
    PYTHONHASHSEED=random \
    PIP_NO_CACHE_DIR=off \
    PIP_DISABLE_PIP_VERSION_CHECK=on \
    PIP_DEFAULT_TIMEOUT=100

# Poetry
ENV POETRY_NO_INTERACTION=1 \
    POETRY_VIRTUALENVS_CREATE=false \
    POETRY_CACHE_DIR='/var/cache/pypoetry' \
    POETRY_HOME='/usr/local' \
    POETRY_VERSION=1.8.4

# Install system dependencies required by opuslib-next, scipy, and mDNS
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        libopus0 \
        libffi-dev \
        avahi-daemon \
        avahi-utils \
        libnss-mdns \
    && rm -rf /var/lib/apt/lists/*

RUN pip install --no-cache-dir "poetry==$POETRY_VERSION"

WORKDIR /app

# Copy full project
COPY pyproject.toml poetry.lock README.md ./
COPY src/ ./src/

# Export pinned dependencies to requirements.txt, then install with pip
RUN poetry export --only main --without-hashes -o requirements.txt && \
    pip install --no-cache-dir -r requirements.txt && \
    pip install --no-cache-dir --no-deps .

# Verify the entry point works
RUN birdnet-listener --help

# Expose default HTTP and UDP ports
EXPOSE 8086/tcp
EXPOSE 4000/udp

# Environment variables for Docker bridge networking:
#   BIRDNET_HOST_IP    - Host's real LAN IP (advertised to ESP32)
#   BIRDNET_ESP32_IP   - ESP32 IP (skips mDNS discovery)
#
# For mDNS to work, use --network host instead of bridge networking.

ENTRYPOINT ["birdnet-listener"]
