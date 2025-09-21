# Multi-stage build for MiniOB
# Stage 1: Build environment
FROM ubuntu:24.04 AS builder

ENV LANG en_US.utf8
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    locales apt-utils && \
    rm -rf /var/lib/apt/lists/* && \
    localedef -i en_US -c -f UTF-8 -A /usr/share/locale/locale.alias en_US.UTF-8

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    wget \
    flex \
    texinfo \
    diffutils \
    bison \
    ccache \
    ninja-build \
    && rm -rf /var/lib/apt/lists/*

# Clone and build MiniOB
WORKDIR /tmp
RUN git clone https://github.com/oceanbase/miniob /tmp/miniob || \
    git clone https://githubfast.com/oceanbase/miniob /tmp/miniob

WORKDIR /tmp/miniob

# Build dependencies with parallel compilation
ENV THIRD_PARTY_INSTALL_PREFIX=/usr/local
RUN bash build.sh init

# Build MiniOB with parallel compilation
RUN mkdir build_release && cd build_release && \
    cmake .. -GNinja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache && \
    ninja -j$(nproc)

# Stage 2: Runtime environment
FROM ubuntu:24.04

ENV LANG en_US.utf8
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    locales apt-utils && \
    rm -rf /var/lib/apt/lists/* && \
    localedef -i en_US -c -f UTF-8 -A /usr/share/locale/locale.alias en_US.UTF-8

# Install runtime dependencies only
RUN apt-get update && apt-get install -y \
    libstdc++6 \
    openssh-server \
    vim \
    sudo \
    zsh \
    git \
    && rm -rf /var/lib/apt/lists/*

# Copy built binaries and libraries from builder stage
COPY --from=builder /usr/local /usr/local
COPY --from=builder /tmp/miniob/build_release/bin /opt/miniob/bin
COPY --from=builder /tmp/miniob/docker/bin /root/docker/bin
COPY --from=builder /tmp/miniob/etc /opt/miniob/etc

# Setup SSH
RUN mkdir /var/run/sshd

# Setup zsh with oh-my-zsh
RUN mkdir ~/.oh-my-zsh && \
    git clone https://gitee.com/mirrors/ohmyzsh.git ~/.oh-my-zsh && \
    cp ~/.oh-my-zsh/templates/zshrc.zsh-template ~/.zshrc && \
    sed -i "s/robbyrussell/bira/" ~/.zshrc && \
    usermod --shell /bin/zsh root && \
    echo "export LD_LIBRARY_PATH=/usr/local/lib64:\$LD_LIBRARY_PATH" >> ~/.zshrc && \
    echo "export PATH=/opt/miniob/bin:\$PATH" >> ~/.zshrc

RUN chmod +x /root/docker/bin/* && \
    touch /etc/.firstrun

# Health check
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s --retries=3 \
    CMD test -f /opt/miniob/bin/observer || exit 1

WORKDIR /root

# Set up entry point
ENTRYPOINT ["/root/docker/bin/starter.sh"]

# Expose SSH port
EXPOSE 22

# Add volume for persistent data
VOLUME ["/data"]