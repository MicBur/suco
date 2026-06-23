# --- Build Stage ---
FROM ubuntu:24.04 AS builder

# Prevent interactive prompts during package installation
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    libssl-dev \
    libhiredis-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

RUN mkdir -p build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release .. && \
    make suco-helper

# --- Runtime Stage ---
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Install compilers (g++/gcc) and runtime libraries
RUN apt-get update && apt-get install -y \
    build-essential \
    libssl-dev \
    libhiredis-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /usr/local/bin

COPY --from=builder /app/build/suco-helper /usr/local/bin/suco-helper
COPY --from=builder /app/dashboard.html /usr/local/bin/dashboard.html

# Exposed ports: 9000 (compiler service), 9001 (dashboard web server)
EXPOSE 9000 9001

# Set default port via environment variable
ENV SUCO_PORT=9000

CMD ["/usr/local/bin/suco-helper"]
