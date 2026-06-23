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

RUN mkdir build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release .. && \
    make ag-helper

# --- Runtime Stage ---
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Install compilers (g++/gcc) and runtime libraries
RUN apt-get update && apt-get install -y \
    build-essential \
    libssl-dev \
    libhiredis-dev \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /app/build/ag-helper /usr/local/bin/ag-helper

# Exposed port for the compilation service
EXPOSE 9000

# Set default port via environment variable
ENV AG_PORT=9000

CMD ["/usr/local/bin/ag-helper"]
