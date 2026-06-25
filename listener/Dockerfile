FROM python:3.12-slim AS base

# Install system dependencies required by opuslib-next and scipy
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        libopus0 \
        libffi-dev \
    && rm -rf /var/lib/apt/lists/*

# Install Poetry
ENV POETRY_VERSION=1.8.4 \
    POETRY_HOME="/opt/poetry" \
    POETRY_NO_INTERACTION=1 \
    POETRY_VIRTUALENVS_CREATE=false

RUN pip install --no-cache-dir "poetry==$POETRY_VERSION"

WORKDIR /app

# Copy dependency files first for better layer caching
COPY pyproject.toml poetry.lock ./

# Install dependencies (no dev dependencies in production)
RUN poetry install --no-root --only main

# Copy application source
COPY src/ ./src/

# Install the project itself
RUN poetry install --only main

# Expose default HTTP and UDP ports
EXPOSE 8080/tcp
EXPOSE 4000/udp

ENTRYPOINT ["birdnet-listener"]
CMD ["--skip-discovery"]
