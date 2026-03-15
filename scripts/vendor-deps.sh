#!/usr/bin/env bash
# =============================================================================
# vendor-deps.sh
#
# 在联网机器上运行，将所有第三方依赖以 .zip 形式下载到 third_party/。
# 下载完成后将 .zip 文件提交到仓库，内网机器 cmake 时自动解压编译。
#
# 用法：
#   bash scripts/vendor-deps.sh
#
# 依赖：curl 或 wget（二选一即可）
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
THIRD_PARTY="${REPO_ROOT}/third_party"

mkdir -p "${THIRD_PARTY}"

# -----------------------------------------------------------------------------
# 工具函数：download_zip <url> <dest_file>
# 若文件已存在则跳过。
# -----------------------------------------------------------------------------
download_zip() {
  local url="$1"
  local dest="$2"

  if [[ -f "${dest}" ]]; then
    echo "[skip] $(basename "${dest}") already exists"
    return 0
  fi

  echo "[download] $(basename "${dest}") ← ${url}"

  if command -v curl &>/dev/null; then
    curl -fsSL --retry 3 --retry-delay 2 -o "${dest}" "${url}"
  elif command -v wget &>/dev/null; then
    wget -q --tries=3 -O "${dest}" "${url}"
  else
    echo "ERROR: curl or wget is required" >&2
    exit 1
  fi

  echo "[done] $(basename "${dest}"): $(du -sh "${dest}" | cut -f1)"
}

# -----------------------------------------------------------------------------
# 各依赖下载
# -----------------------------------------------------------------------------

# 1. oneTBB v2022.0.0
download_zip \
  "https://github.com/oneapi-src/oneTBB/archive/refs/tags/v2022.0.0.zip" \
  "${THIRD_PARTY}/oneTBB.zip"

# 2. libuv v1.48.0
download_zip \
  "https://github.com/libuv/libuv/archive/refs/tags/v1.48.0.zip" \
  "${THIRD_PARTY}/libuv.zip"

# 3. uvw v3.4.0 (匹配 libuv v1.48)
download_zip \
  "https://github.com/skypjack/uvw/archive/refs/tags/v3.4.0_libuv_v1.48.zip" \
  "${THIRD_PARTY}/uvw.zip"

# 4. BehaviorTree.CPP 4.9.0
download_zip \
  "https://github.com/BehaviorTree/BehaviorTree.CPP/archive/refs/tags/4.9.0.zip" \
  "${THIRD_PARTY}/BehaviorTree.CPP.zip"

# 5. GoogleTest v1.16.0
download_zip \
  "https://github.com/google/googletest/archive/refs/tags/v1.16.0.zip" \
  "${THIRD_PARTY}/googletest.zip"

# -----------------------------------------------------------------------------
# 完成
# -----------------------------------------------------------------------------
echo ""
echo "All deps downloaded to ${THIRD_PARTY}/"
echo ""
echo "Next steps:"
echo "  git add third_party/*.zip"
echo "  git commit -m 'vendor: add third-party dependency zips'"
echo ""
echo "On intranet machine:"
echo "  cmake -B build && cmake --build build -j\$(nproc)"
