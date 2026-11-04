# syntax=docker/dockerfile:1
#
# simple_http 的自包含容器编译 / 测试环境。
#
# 设计原则：镜像内部完成全部工作，不读取宿主的任何东西——不挂载源码、不共享
# 宿主 ~/.xmake、不复用宿主的 test/.venv，也不依赖宿主的编译器/Go/Python。
# 源码在镜像构建时 COPY 进来，xmake 依赖（Boost/OpenSSL4/ngtcp2/nghttp3/...）
# 和 Go module 都在镜像内下载并编译，测试目标也在镜像内预编译。
#
# 因此运行时只需要：
#
#   docker run --rm -it simple-http-dev
#
# 进入后就是 /work，可以直接 xmake / test/run.sh。改了源码需要重新
# `docker build`（Docker layer cache 会复用未变化的步骤）。
#
# 阶段：
#   base   工具链（Ubuntu 26.04 + GCC 15 + xmake + Go + Python venv）
#   built  在 base 上 COPY 源码、下载 Go module、编译全部测试目标并自检
#   full   built + 外部一致性/压力工具（h2spec/h3spec/k6/h2load/docker CLI）
#   dev    默认目标，等于 built
#
# 构建：
#   docker build -t simple-http-dev .                 # dev / built
#   docker build --target full -t simple-http-full .  # full
#
# 如果构建容器访问不了 GitHub/PyPI，用宿主的系统代理（代理只监听 127.0.0.1 时
# 必须加 --network host，让构建容器共享宿主网络命名空间）：
#
#   docker build --network host \
#       --build-arg http_proxy="$http_proxy" \
#       --build-arg https_proxy="$https_proxy" \
#       --build-arg no_proxy="$no_proxy" \
#       -t simple-http-dev .
#
# 注意：这里的代理只用于“构建镜像”。镜像构建完成后，普通编译和
# unit/openapi/client-diff/server-diff 都不再需要网络或宿主资源。
# conformance 的 Autobahn/Deno 兄弟容器仍需要运行时挂 docker socket，这是
# 外部工具本身的约束，不是编译环境的一部分。
#
# 需要把构建产物拿回宿主时：
#   docker cp "$(docker create simple-http-dev):/work/build" ./build-container

ARG UBUNTU_VERSION=26.04


# ---------------------------------------------------------------------------
# base：自包含的工具链
# ---------------------------------------------------------------------------
FROM ubuntu:${UBUNTU_VERSION} AS base

ARG DEBIAN_FRONTEND=noninteractive
# 版本全部固定，避免半年后同样的 Dockerfile 装出另一套工具链。
ARG XMAKE_VERSION=3.0.9
ARG GO_VERSION=1.26.8
ARG TARGETARCH
# 构建期 HTTP 代理。Docker 会把 --build-arg 传成 RUN 的环境变量，apt / pip /
# curl / git / go 都会直接使用；这些值不会写进最终镜像的 ENV。
ARG HTTP_PROXY
ARG HTTPS_PROXY
ARG NO_PROXY
ARG http_proxy
ARG https_proxy
ARG no_proxy

ENV LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    TZ=UTC \
    # xmake 以 root 运行时需要显式放行，否则会拒绝安装包。
    XMAKE_ROOT=y \
    # 测试脚本优先使用 venv 里的 python；Go 官方二进制在 /usr/local/go。
    PATH=/opt/venv/bin:/usr/local/go/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin \
    GOTOOLCHAIN=local

# 依赖说明：
#   build-essential/autoconf/automake/libtool/perl  —— xmake 从源码构建
#     openssl4、ngtcp2、nghttp3、Boost 时需要
#   libncurses6                                     —— xmake bundle 的运行时依赖
#   python3-venv/pip                                —— OpenAPI 套件与 gcovr
#   libssl-dev/zlib1g-dev/libbz2-dev/liblzma-dev     —— CMake 下游与压缩库
#   nghttp2-client                                  —— h2load（stress 的 h1/h2 档）
#   curl/git/ca-certificates/unzip/xz-utils/bzip2   —— 下载与解包
RUN apt-get -o Acquire::Retries=5 -o Acquire::http::Timeout=60 update \
    && apt-get -o Acquire::Retries=5 -o Acquire::http::Timeout=60 install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        curl \
        git \
        perl \
        pkg-config \
        cmake \
        ninja-build \
        autoconf \
        automake \
        libtool \
        patch \
        file \
        unzip \
        xz-utils \
        bzip2 \
        libncurses6 \
        libssl-dev \
        zlib1g-dev \
        libbz2-dev \
        liblzma-dev \
        python3 \
        python3-venv \
        python3-pip \
        nghttp2-client \
        procps \
        iproute2 \
        netcat-openbsd \
        lsof \
        less \
        vim-tiny \
    && rm -rf /var/lib/apt/lists/*

# xmake：官方 linux-x86_64 单文件 bundle（内含 Lua 运行时与脚本），直接装进
# PATH。仓库测试基线只覆盖 x86_64（BIN_DIR 写死 build/linux/x86_64/release）。
RUN set -eux; \
    case "${TARGETARCH:-amd64}" in \
        amd64) ;; \
        *) echo "xmake bundle is only published for linux/amd64 (got ${TARGETARCH})" >&2; exit 1 ;; \
    esac; \
    curl -fsSL --retry 10 --retry-all-errors --retry-delay 2 \
        --connect-timeout 30 --speed-time 30 --speed-limit 1024 --max-time 900 \
        -o /usr/local/bin/xmake \
        "https://github.com/xmake-io/xmake/releases/download/v${XMAKE_VERSION}/xmake-bundle-v${XMAKE_VERSION}.linux.x86_64"; \
    chmod 0755 /usr/local/bin/xmake; \
    xmake --version

# Go：test/diff 下的参考服务端与探针由各 runner 用 go build 构建。
RUN set -eux; \
    case "${TARGETARCH:-amd64}" in \
        amd64) go_arch=amd64 ;; \
        arm64) go_arch=arm64 ;; \
        *) echo "unsupported TARGETARCH: ${TARGETARCH}" >&2; exit 1 ;; \
    esac; \
    curl -fsSL --retry 10 --retry-all-errors --retry-delay 2 \
        --connect-timeout 30 --speed-time 30 --speed-limit 1024 --max-time 900 \
        -o /tmp/go.tar.gz "https://go.dev/dl/go${GO_VERSION}.linux-${go_arch}.tar.gz"; \
    tar -C /usr/local -xzf /tmp/go.tar.gz; \
    rm -f /tmp/go.tar.gz; \
    go version

# Python 测试依赖装进独立 venv，避免污染系统 Python（PEP 668）。
COPY test/requirements.txt /tmp/requirements.txt
RUN python3 -m venv /opt/venv \
    && /opt/venv/bin/pip install --no-cache-dir --upgrade pip \
    && /opt/venv/bin/pip install --no-cache-dir --retries 10 --timeout 60 \
        -r /tmp/requirements.txt \
    && rm -f /tmp/requirements.txt

# 可 source 的环境脚本：进入 venv，并把 Go / xmake 所在目录补进 PATH。
# /etc/profile.d 让登录 shell 自动生效，`bash -l` 的 CMD 就是为了这个；
# 非登录 shell 里手动 `source /opt/simple_http.env` 即可。
RUN printf '%s\n' \
        '# simple_http container environment' \
        '. /opt/venv/bin/activate' \
        'export PATH="/usr/local/go/bin:/usr/local/bin:$PATH"' \
        'export XMAKE_ROOT=y' \
        > /opt/simple_http.env \
    && printf '%s\n' '. /opt/simple_http.env' > /etc/profile.d/simple_http.sh \
    && chmod 0644 /opt/simple_http.env /etc/profile.d/simple_http.sh

WORKDIR /work
CMD ["bash", "-l"]


# ---------------------------------------------------------------------------
# built：把源码带进镜像，在镜像内完成依赖与目标的编译
# ---------------------------------------------------------------------------
# 只 COPY 构建所需的内容，宿主的 build/、.cache/、.git、test/.venv、v2ray-cpp/
# 等都不会进入镜像（见 .dockerignore）。
FROM base AS built

ARG HTTP_PROXY
ARG HTTPS_PROXY
ARG NO_PROXY
ARG http_proxy
ARG https_proxy
ARG no_proxy

COPY xmake.lua xmake-requires.lock /work/
COPY include/ /work/include/
COPY test/ /work/test/

# Go 差分测试需要的模块全部预下载到镜像内的 GOMODCACHE，运行期不需要联网。
RUN cd /work/test/diff && go mod download

# 首次构建会在镜像内下载并编译 Boost 1.92 / OpenSSL 4 / ngtcp2 / nghttp3 /
# glaze / zlib / brotli / Catch2；这些包缓存也留在镜像里，之后在容器内增量
# 编译不需要重新下载。
RUN xmake f -c -y \
    && for target in \
        test_unit \
        fixture_conformance_server \
        diff_server_cpp \
        diff_openapi_server \
        test_sanitizer_concurrency \
        diff_client_basic \
        diff_client_body \
        diff_client_policy \
        diff_client_ws \
        diff_client_h3; do \
        xmake build -j"$(nproc)" "$target"; \
    done

# 构建期自检：unit + OpenAPI 契约。任何一步失败都不产出可用镜像。
RUN test/run.sh standard

CMD ["bash", "-l"]


# ---------------------------------------------------------------------------
# full：built + 外部一致性 / 压力套件
# ---------------------------------------------------------------------------
# dev 是默认目标，所以 full 必须定义在它之前。
FROM built AS full

ARG DEBIAN_FRONTEND=noninteractive
ARG H2SPEC_VERSION=2.6.0
ARG H3SPEC_VERSION=0.1.14
ARG K6_VERSION=2.3.0
ARG DOCKER_CLI_VERSION=29.5.3
ARG TARGETARCH
# 代理值沿用 --build-arg（每个 stage 需要重新声明才能看到）。
ARG HTTP_PROXY
ARG HTTPS_PROXY
ARG NO_PROXY
ARG http_proxy
ARG https_proxy
ARG no_proxy

# docker CLI 供 conformance 启动 Autobahn / Deno 兄弟容器（运行时挂 socket）。
# 用官方静态包而不是 apt：不依赖 Ubuntu 源，也不会把 dockerd/containerd 拖进来。
RUN set -eux; \
    if [ "${TARGETARCH:-amd64}" != "amd64" ]; then \
        echo "docker static CLI is only packaged for linux/amd64 (got ${TARGETARCH})" >&2; \
        exit 1; \
    fi; \
    curl -fsSL --retry 10 --retry-all-errors --retry-delay 2 \
        --connect-timeout 30 --speed-time 30 --speed-limit 1024 --max-time 900 \
        -o /tmp/docker.tgz \
        "https://download.docker.com/linux/static/stable/x86_64/docker-${DOCKER_CLI_VERSION}.tgz"; \
    tar -xzf /tmp/docker.tgz -C /tmp; \
    install -m 0755 /tmp/docker/docker /usr/local/bin/docker; \
    rm -rf /tmp/docker.tgz /tmp/docker; \
    docker --version

RUN set -eux; \
    if [ "${TARGETARCH:-amd64}" != "amd64" ]; then \
        echo "conformance tools are only packaged for linux/amd64 (got ${TARGETARCH})" >&2; \
        exit 1; \
    fi; \
    # h2spec：HTTP/2 帧层与流状态机
    curl -fsSL --retry 10 --retry-all-errors --retry-delay 2 \
        --connect-timeout 30 --speed-time 30 --speed-limit 1024 --max-time 900 \
        -o /tmp/h2spec.tar.gz \
        "https://github.com/summerwind/h2spec/releases/download/v${H2SPEC_VERSION}/h2spec_linux_amd64.tar.gz"; \
    tar -xzf /tmp/h2spec.tar.gz -C /usr/local/bin h2spec; \
    chmod +x /usr/local/bin/h2spec; \
    # h3spec：QUIC / HTTP/3 错误路径；测试脚本按这个带平台后缀的名字查找
    curl -fsSL --retry 10 --retry-all-errors --retry-delay 2 \
        --connect-timeout 30 --speed-time 30 --speed-limit 1024 --max-time 900 \
        -o /usr/local/bin/h3spec-linux-x86_64 \
        "https://github.com/kazu-yamamoto/h3spec/releases/download/v${H3SPEC_VERSION}/h3spec-linux-x86_64"; \
    chmod +x /usr/local/bin/h3spec-linux-x86_64; \
    # k6：WebSocket 与 h1 压力档的唯一驱动
    curl -fsSL --retry 10 --retry-all-errors --retry-delay 2 \
        --connect-timeout 30 --speed-time 30 --speed-limit 1024 --max-time 900 \
        -o /tmp/k6.tar.gz \
        "https://github.com/grafana/k6/releases/download/v${K6_VERSION}/k6-v${K6_VERSION}-linux-amd64.tar.gz"; \
    tar -xzf /tmp/k6.tar.gz -C /tmp; \
    install -m 0755 "/tmp/k6-v${K6_VERSION}-linux-amd64/k6" /usr/local/bin/k6; \
    rm -rf /tmp/h2spec.tar.gz /tmp/k6.tar.gz "/tmp/k6-v${K6_VERSION}-linux-amd64"; \
    h2spec --version; \
    k6 version; \
    command -v h3spec-linux-x86_64

# conformance/run.sh 会在运行时把 h1spec 脚本下到 .cache/；这里预先取好，
# full 镜像里跑 h1spec 不再依赖 GitHub（脚本只进镜像缓存，不进仓库）。
RUN mkdir -p /work/.cache/conformance/h1spec \
    && curl -fsSL --retry 10 --retry-all-errors --retry-delay 2 \
        --connect-timeout 30 --speed-time 30 --speed-limit 1024 --max-time 900 \
        -o /work/.cache/conformance/h1spec/http_test.ts \
        "https://raw.githubusercontent.com/uNetworking/h1spec/main/http_test.ts"

WORKDIR /work
CMD ["bash", "-l"]


# ---------------------------------------------------------------------------
# dev：默认目标（自包含的编译 + 差分测试环境）
# ---------------------------------------------------------------------------
FROM built AS dev

WORKDIR /work
CMD ["bash", "-l"]
