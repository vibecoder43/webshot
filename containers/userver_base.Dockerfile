FROM debian:forky

ENV DEBIAN_FRONTEND=noninteractive

# Step 1: minimal base tools to clone userver
RUN apt-get update && \
    apt-get install -y --no-install-recommends ca-certificates git && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /usr/src/userver

# Step 2: clone userver v2.13 sources
RUN git clone --branch v2.13 --depth 1 https://github.com/userver-framework/userver.git .

# Step 3: install userver build dependencies from the in-repo Debian 12 list
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
      $(cat scripts/docs/en/deps/debian-12.md) \
      clang-21 mold && \
    rm -rf /var/lib/apt/lists/*

# Step 4: configure, build, and install userver with PostgreSQL and S3 enabled
RUN cmake -DCMAKE_BUILD_TYPE=Release \
          -DUSERVER_FEATURE_POSTGRESQL=ON \
          -DUSERVER_FEATURE_S3API=ON \
          -DUSERVER_INSTALL=ON \
          -DCMAKE_C_COMPILER=/usr/bin/clang-21 \
          -DCMAKE_CXX_COMPILER=/usr/bin/clang++-21 \
          -DCMAKE_ASM_COMPILER=/usr/bin/clang-21 \
          -DUSERVER_USE_LD=mold \
          -G Ninja \
          -S . -B build_release && \
    cmake --build build_release && \
    cmake --build build_release --target install && \
    rm -rf build_release

# Make userver CMake package discoverable for webshot builds
ENV USERVER_DIR=/usr/local/lib/cmake/userver
