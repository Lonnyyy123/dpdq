ARG BASE_IMAGE=ubuntu:16.04
FROM ${BASE_IMAGE}

ARG APT_MIRROR=mirrors.aliyun.com
ENV DEBIAN_FRONTEND=noninteractive

SHELL ["/bin/bash", "-lc"]

RUN set -euo pipefail \
    && set_sources_list() { \
      local base_url="$1"; \
      printf "%s\n" \
        "deb ${base_url} xenial main restricted universe multiverse" \
        "deb ${base_url} xenial-updates main restricted universe multiverse" \
        "deb ${base_url} xenial-backports main restricted universe multiverse" \
        "deb ${base_url} xenial-security main restricted universe multiverse" \
        >/etc/apt/sources.list; \
    } \
    && try_apt_update() { \
      local mirror="$1"; \
      local base_url; \
      case "${mirror}" in \
        http://*|https://*) base_url="${mirror}" ;; \
        *) base_url="http://${mirror}/ubuntu" ;; \
      esac; \
      echo "[build-env] Trying apt mirror: ${base_url}"; \
      set_sources_list "${base_url}"; \
      apt-get -o Acquire::Retries=2 -o Acquire::http::Timeout=20 update; \
    } \
    && MIRRORS=() \
    && if [[ -n "${APT_MIRROR}" ]]; then MIRRORS+=("${APT_MIRROR}"); fi \
    && MIRRORS+=( \
      "mirrors.aliyun.com" \
      "mirrors.ustc.edu.cn/ubuntu-old-releases" \
      "archive.ubuntu.com" \
      "old-releases.ubuntu.com" \
    ) \
    && APT_READY=0 \
    && for mirror in "${MIRRORS[@]}"; do \
      if try_apt_update "${mirror}"; then \
        APT_READY=1; \
        break; \
      fi; \
      rm -rf /var/lib/apt/lists/*; \
    done \
    && if [[ "${APT_READY}" -ne 1 ]]; then \
      echo "[build-env] All configured apt mirrors failed." >&2; \
      exit 100; \
    fi \
    && apt-get install -y --no-install-recommends \
      python gcc-5 g++-5 make cmake pkg-config libc6-dev ca-certificates \
    && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-5 50 \
    && update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-5 50 \
    && rm -rf /var/lib/apt/lists/*
