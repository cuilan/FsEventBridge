# Build stage
FROM debian:bookworm-slim AS builder

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    pkg-config \
    liburing-dev \
    libsystemd-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

# Build the project
RUN mkdir build && cd build && cmake .. && make

# Final stage
FROM debian:bookworm-slim

# Install runtime dependencies
RUN apt-get update && apt-get install -y \
    liburing2 \
    libsystemd0 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /app/build/FsEventBridge .
COPY --from=builder /app/configs/config.toml ./config.toml

# Create a default monitor directory
RUN mkdir -p /monitor

# Run the bridge
ENTRYPOINT ["./FsEventBridge"]
CMD ["-d", "/monitor", "-s", "/tmp/feb.sock"]
