#!/usr/bin/env bash
#
# zOS development-environment bootstrap for Ubuntu 22.04 under WSL2.
#
# This script:
#   1. Installs a small set of Ubuntu host packages
#   2. Downloads and verifies the official LLVM 22.1.8 Linux x64 release.
#   3. Creates a repo-local Python environment with SCons 4.10.1.
#   4. Creates a stable .zos/Toolchain/bin facade.
#   5. Discovers OVMF firmware and records the resulting environment.
#   6. Runs smoke tests and writes a machine-readable manifest.
#
# Run from anywhere:
#   ./Tools/SetupDevEnv.sh
#
# Optional:
#   ./Tools/SetupDevEnv.sh --verify-only
#   ./Tools/SetupDevEnv.sh --force-llvm
#

set -Eeuo pipefail
IFS=$'\n\t'
umask 022

LLVM_VERSION="22.1.8"
SCONS_VERSION="4.10.1"
LLVM_ARCHIVE_NAME="LLVM-${LLVM_VERSION}-Linux-X64.tar.xz"
LLVM_RELEASE_BASE="https://github.com/llvm/llvm-project/releases/download/llvmorg-${LLVM_VERSION}"
LLVM_ARCHIVE_URL="${LLVM_RELEASE_BASE}/${LLVM_ARCHIVE_NAME}"
LLVM_SIGNATURE_URL="${LLVM_ARCHIVE_URL}.sig"
LLVM_RELEASE_KEYS_URL="https://releases.llvm.org/release-keys.asc"

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPOSITORY_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
ZOS_STATE_DIR="${REPOSITORY_ROOT}/.zos"
DOWNLOAD_DIR="${ZOS_STATE_DIR}/Downloads"
TOOLCHAIN_DIR="${ZOS_STATE_DIR}/Toolchain"
LLVM_DIR="${TOOLCHAIN_DIR}/LLVM-${LLVM_VERSION}"
PYTHON_DIR="${TOOLCHAIN_DIR}/Python"
TOOL_BIN_DIR="${TOOLCHAIN_DIR}/bin"
ENVIRONMENT_FILE="${TOOLCHAIN_DIR}/Environment.sh"
MANIFEST_FILE="${TOOLCHAIN_DIR}/Manifest.json"

FORCE_LLVM=0
VERIFY_ONLY=0

log() {
    printf '\033[1;34m[zOS]\033[0m %s\n' "$*"
}

warn() {
    printf '\033[1;33m[zOS warning]\033[0m %s\n' "$*" >&2
}

fail() {
    printf '\033[1;31m[zOS error]\033[0m %s\n' "$*" >&2
    exit 1
}

usage() {
    cat <<'EOF'
Usage: SetupDevEnv.sh [options]

Options:
  --verify-only  Do not install or download anything; verify the existing setup.
  --force-llvm   Re-download and replace the repository-local LLVM installation.
  -h, --help     Show this help text.
EOF
}

while (($# > 0)); do
    case "$1" in
        --verify-only)
            VERIFY_ONLY=1
            ;;
        --force-llvm)
            FORCE_LLVM=1
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            fail "Unknown argument: $1"
            ;;
    esac
    shift
done

if [[ "$(uname -s)" != "Linux" ]]; then
    fail "This bootstrap currently supports Linux hosts only."
fi

if [[ "$(uname -m)" != "x86_64" ]]; then
    fail "This bootstrap expects an x86-64 development host."
fi

if [[ -r /etc/os-release ]]; then
    # shellcheck disable=SC1091
    source /etc/os-release
    if [[ "${ID:-}" != "ubuntu" || "${VERSION_ID:-}" != "22.04" ]]; then
        warn "The tested host is Ubuntu 22.04; detected ${PRETTY_NAME:-unknown Linux}."
    fi
fi

if [[ -n "${WSL_DISTRO_NAME:-}" || "$(uname -r)" == *[Mm]icrosoft* ]]; then
    log "Detected WSL environment: ${WSL_DISTRO_NAME:-unknown distribution}."
else
    warn "WSL was not detected. The script may still work on native Ubuntu 22.04."
fi

case "${REPOSITORY_ROOT}" in
    /mnt/*)
        warn "The repository is stored on a Windows-mounted filesystem (${REPOSITORY_ROOT})."
        warn "Move it under the WSL Linux filesystem, such as ~/Projects/zOS, for faster and more reliable builds."
        ;;
esac

require_command() {
    command -v "$1" >/dev/null 2>&1 || fail "Required command is unavailable: $1"
}

version_line() {
    "$@" 2>/dev/null | head -n 1
}

find_ovmf_file() {
    local file_name
    local candidate

    for file_name in "$@"; do
        for candidate in \
            "/usr/share/OVMF/${file_name}" \
            "/usr/share/qemu/${file_name}" \
            "/usr/share/edk2/ovmf/${file_name}"
        do
            if [[ -f "${candidate}" ]]; then
                printf '%s\n' "${candidate}"
                return 0
            fi
        done
    done

    if command -v dpkg >/dev/null 2>&1 && dpkg-query -W -f='${Status}' ovmf 2>/dev/null | grep -q "install ok installed"; then
        while IFS= read -r candidate; do
            for file_name in "$@"; do
                if [[ "$(basename -- "${candidate}")" == "${file_name}" && -f "${candidate}" ]]; then
                    printf '%s\n' "${candidate}"
                    return 0
                fi
            done
        done < <(dpkg -L ovmf)
    fi

    return 1
}

verify_environment() {
    log "Verifying the zOS development environment."

    [[ -x "${LLVM_DIR}/bin/clang" ]] || fail "LLVM is missing: ${LLVM_DIR}/bin/clang"
    [[ -x "${LLVM_DIR}/bin/clang++" ]] || fail "Clang++ is missing."
    [[ -x "${LLVM_DIR}/bin/ld.lld" ]] || fail "LLD is missing."
    [[ -x "${LLVM_DIR}/bin/lld-link" ]] || fail "lld-link is missing."
    [[ -x "${PYTHON_DIR}/bin/scons" ]] || fail "Repository-local SCons is missing."
    [[ -r "${ENVIRONMENT_FILE}" ]] || fail "Environment file is missing: ${ENVIRONMENT_FILE}"

    require_command qemu-system-x86_64
    require_command qemu-img
    require_command sgdisk
    require_command mkfs.fat
    require_command mcopy
    require_command gdb

    local clang_version
    local scons_version
    local ovmf_code
    local ovmf_vars
    local smoke_dir

    clang_version="$("${LLVM_DIR}/bin/clang" --version | head -n 1)"
    [[ "${clang_version}" == *"${LLVM_VERSION}"* ]] ||
        fail "Expected LLVM ${LLVM_VERSION}, but found: ${clang_version}"

    scons_version="$("${PYTHON_DIR}/bin/python" -c 'import SCons; print(SCons.__version__)')"
    [[ "${scons_version}" == "${SCONS_VERSION}" ]] ||
        fail "Expected SCons ${SCONS_VERSION}, but found ${scons_version}."

    ovmf_code="$(find_ovmf_file \
        OVMF_CODE_4M.fd \
        OVMF_CODE.fd \
        OVMF_CODE_4M.secboot.fd \
        OVMF_CODE.secboot.fd)" ||
        fail "Could not locate OVMF code firmware."

    ovmf_vars="$(find_ovmf_file \
        OVMF_VARS_4M.fd \
        OVMF_VARS.fd \
        OVMF_VARS_4M.ms.fd \
        OVMF_VARS.ms.fd)" ||
        fail "Could not locate an OVMF variable-store template."

    smoke_dir="$(mktemp -d)"
    trap 'rm -rf "${smoke_dir:-}"' RETURN

    cat > "${smoke_dir}/Language.cpp" <<'EOF'
#include <expected>
#include <optional>
#include <span>
#include <variant>

using Value = std::variant<int, long>;

constexpr bool Test()
{
    Value value = 42;
    return std::get_if<int>(&value) != nullptr;
}

static_assert(Test());

int main()
{
    std::optional<int> optional = 1;
    std::expected<int, int> expected = 2;
    int storage[] = { 1, 2, 3 };
    std::span<int> span(storage);

    return *optional + *expected + span[0] == 4 ? 0 : 1;
}
EOF

    "${LLVM_DIR}/bin/clang++" \
        -std=c++23 \
        -stdlib=libc++ \
        -Wall \
        -Wextra \
        -Werror \
        -c "${smoke_dir}/Language.cpp" \
        -o "${smoke_dir}/Language.o"

    cat > "${smoke_dir}/Freestanding.cpp" <<'EOF'
extern "C" void KernelEntry()
{
    for (;;) {
        __asm__ volatile("pause");
    }
}
EOF

    "${LLVM_DIR}/bin/clang++" \
        --target=x86_64-unknown-none-elf \
        -std=c++23 \
        -ffreestanding \
        -fno-exceptions \
        -fno-rtti \
        -fno-stack-protector \
        -mno-red-zone \
        -Wall \
        -Wextra \
        -Werror \
        -c "${smoke_dir}/Freestanding.cpp" \
        -o "${smoke_dir}/Freestanding.o"

    "${LLVM_DIR}/bin/llvm-readobj" --file-headers "${smoke_dir}/Freestanding.o" |
        grep -q "Format: elf64-x86-64" ||
        fail "The freestanding smoke test did not produce an ELF64 x86-64 object."

    rm -rf "${smoke_dir}"
    trap - RETURN

    log "LLVM:  ${clang_version}"
    log "SCons: ${scons_version}"
    log "QEMU:  $(version_line qemu-system-x86_64 --version)"
    log "OVMF:   ${ovmf_code}"
    log "All development-environment checks passed."
}

if ((VERIFY_ONLY)); then
    verify_environment
    exit 0
fi

mkdir -p "${DOWNLOAD_DIR}" "${TOOLCHAIN_DIR}" "${TOOL_BIN_DIR}"

if ((EUID == 0)); then
    SUDO=()
else
    require_command sudo
    SUDO=(sudo)
fi

log "Installing Ubuntu host prerequisites."
"${SUDO[@]}" apt-get update
"${SUDO[@]}" env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    curl \
    dosfstools \
    file \
    gdb \
    gdisk \
    git \
    gnupg \
    libtinfo6 \
    libxml2 \
    mtools \
    ovmf \
    pkg-config \
    python3 \
    python3-pip \
    python3-venv \
    qemu-system-x86 \
    qemu-utils \
    rsync \
    unzip \
    uuid-runtime \
    xz-utils \
    zip

require_command curl
require_command gpg
require_command tar
require_command python3

download_file() {
    local url="$1"
    local destination="$2"
    local temporary="${destination}.part"

    if [[ -f "${destination}" ]]; then
        log "Using cached download: $(basename -- "${destination}")"
        return
    fi

    log "Downloading $(basename -- "${destination}")."
    rm -f "${temporary}"
    curl \
        --fail \
        --location \
        --retry 5 \
        --retry-delay 2 \
        --retry-all-errors \
        --output "${temporary}" \
        "${url}"
    mv "${temporary}" "${destination}"
}

install_llvm() {
    local archive="${DOWNLOAD_DIR}/${LLVM_ARCHIVE_NAME}"
    local signature="${archive}.sig"
    local keys="${DOWNLOAD_DIR}/llvm-release-keys.asc"
    local gnupg_home
    local extraction_dir="${LLVM_DIR}.extracting"

    if ((FORCE_LLVM)); then
        rm -rf "${LLVM_DIR}"
    fi

    if [[ -x "${LLVM_DIR}/bin/clang" ]] && "${LLVM_DIR}/bin/clang" --version | head -n 1 | grep -q "${LLVM_VERSION}"
    then
        log "LLVM ${LLVM_VERSION} is already installed."
        return
    fi

    download_file "${LLVM_ARCHIVE_URL}" "${archive}"
    download_file "${LLVM_SIGNATURE_URL}" "${signature}"
    download_file "${LLVM_RELEASE_KEYS_URL}" "${keys}"

    log "Verifying the LLVM release signature."
    gnupg_home="$(mktemp -d)"
    chmod 700 "${gnupg_home}"
    gpg --batch --homedir "${gnupg_home}" --import "${keys}" >/dev/null 2>&1
    gpg --batch --homedir "${gnupg_home}" --verify "${signature}" "${archive}"
    rm -rf "${gnupg_home}"

    log "Extracting LLVM ${LLVM_VERSION}."
    rm -rf "${extraction_dir}"
    mkdir -p "${extraction_dir}"
    tar -xJf "${archive}" -C "${extraction_dir}" --strip-components=1

    [[ -x "${extraction_dir}/bin/clang" ]] ||
        fail "The LLVM archive did not contain bin/clang."

    "${extraction_dir}/bin/clang" --version | head -n 1 | grep -q "${LLVM_VERSION}" ||
        fail "The downloaded LLVM archive reports an unexpected version."

    rm -rf "${LLVM_DIR}"
    mv "${extraction_dir}" "${LLVM_DIR}"
}


install_scons() {
    if [[ ! -x "${PYTHON_DIR}/bin/python" ]]; then
        log "Creating repository-local Python environment."
        python3 -m venv "${PYTHON_DIR}"
    fi

    log "Installing SCons ${SCONS_VERSION} into the repository-local Python environment."
    "${PYTHON_DIR}/bin/python" -m pip install \
        --disable-pip-version-check \
        --no-input \
        --upgrade \
        "SCons==${SCONS_VERSION}"
}

create_tool_facade() {
    local tool
    local source

    log "Creating stable toolchain command links."
    mkdir -p "${TOOL_BIN_DIR}"

    for tool in \
        clang \
        clang++ \
        clang-format \
        clang-tidy \
        clangd \
        ld.lld \
        lld \
        lld-link \
        llvm-addr2line \
        llvm-ar \
        llvm-cxxfilt \
        llvm-dwarfdump \
        llvm-nm \
        llvm-objcopy \
        llvm-objdump \
        llvm-ranlib \
        llvm-readelf \
        llvm-readobj \
        llvm-size \
        llvm-strip \
        llvm-symbolizer
    do
        source="${LLVM_DIR}/bin/${tool}"
        if [[ -x "${source}" ]]; then
            ln -sfn "${source}" "${TOOL_BIN_DIR}/${tool}"
        fi
    done

    ln -sfn "${PYTHON_DIR}/bin/python" "${TOOL_BIN_DIR}/python"
    ln -sfn "${PYTHON_DIR}/bin/scons" "${TOOL_BIN_DIR}/scons"

    for tool in \
        gdb \
        mcopy \
        mmd \
        mformat \
        mkfs.fat \
        qemu-img \
        qemu-system-x86_64 \
        sgdisk
    do
        source="$(command -v "${tool}")"
        ln -sfn "${source}" "${TOOL_BIN_DIR}/${tool}"
    done
}

write_environment() {
    local ovmf_code
    local ovmf_vars
    local qemu_accel

    ovmf_code="$(find_ovmf_file \
        OVMF_CODE_4M.fd \
        OVMF_CODE.fd \
        OVMF_CODE_4M.secboot.fd \
        OVMF_CODE.secboot.fd)" ||
        fail "Could not locate OVMF code firmware."

    ovmf_vars="$(find_ovmf_file \
        OVMF_VARS_4M.fd \
        OVMF_VARS.fd \
        OVMF_VARS_4M.ms.fd \
        OVMF_VARS.ms.fd)" ||
        fail "Could not locate an OVMF variable-store template."

    if [[ -w /dev/kvm ]]; then
        qemu_accel="kvm"
    else
        qemu_accel="tcg,thread=multi"
    fi

    log "Writing ${ENVIRONMENT_FILE}."
    cat > "${ENVIRONMENT_FILE}" <<EOF
# Generated by Tools/SetupDevelopmentEnvironment.sh.
# Source this file from development shells and build wrappers.

export ZOS_REPOSITORY_ROOT='${REPOSITORY_ROOT}'
export ZOS_TOOLCHAIN_ROOT='${TOOLCHAIN_DIR}'
export ZOS_LLVM_ROOT='${LLVM_DIR}'
export ZOS_PYTHON='${PYTHON_DIR}/bin/python'
export ZOS_SCONS='${PYTHON_DIR}/bin/scons'
export ZOS_OVMF_CODE='${ovmf_code}'
export ZOS_OVMF_VARS_TEMPLATE='${ovmf_vars}'
export ZOS_QEMU_ACCEL='${qemu_accel}'
export PATH='${TOOL_BIN_DIR}':"\${PATH}"
EOF
}

write_manifest() {
    log "Writing ${MANIFEST_FILE}."

    REPOSITORY_ROOT="${REPOSITORY_ROOT}" \
    MANIFEST_FILE="${MANIFEST_FILE}" \
    LLVM_VERSION="${LLVM_VERSION}" \
    LLVM_DIR="${LLVM_DIR}" \
    SCONS_VERSION="${SCONS_VERSION}" \
    PYTHON_DIR="${PYTHON_DIR}" \
    ENVIRONMENT_FILE="${ENVIRONMENT_FILE}" \
    "${PYTHON_DIR}/bin/python" <<'PY'
import json
import os
import platform
import subprocess
from pathlib import Path

def output(*command: str) -> str:
    return subprocess.check_output(command, text=True, stderr=subprocess.STDOUT).strip()

def first_line(*command: str) -> str:
    return output(*command).splitlines()[0]

def package_version(name: str):
    try:
        return output("dpkg-query", "-W", "-f=${Version}", name)
    except (FileNotFoundError, subprocess.CalledProcessError):
        return None

environment = {}
for line in Path(os.environ["ENVIRONMENT_FILE"]).read_text(encoding="utf-8").splitlines():
    if line.startswith("export ") and "=" in line:
        key, value = line[7:].split("=", 1)
        environment[key] = value.strip("'\"")

llvm_dir = Path(os.environ["LLVM_DIR"])
python_dir = Path(os.environ["PYTHON_DIR"])

manifest = {
    "schema": 1,
    "host": {
        "platform": platform.platform(),
        "machine": platform.machine(),
        "pythonSystem": platform.python_version(),
        "wslDistribution": os.environ.get("WSL_DISTRO_NAME"),
    },
    "toolchain": {
        "llvm": {
            "requestedVersion": os.environ["LLVM_VERSION"],
            "reportedVersion": first_line(str(llvm_dir / "bin" / "clang"), "--version"),
            "root": str(llvm_dir),
        },
        "scons": {
            "requestedVersion": os.environ["SCONS_VERSION"],
            "reportedVersion": output(
                str(python_dir / "bin" / "python"),
                "-c",
                "import SCons; print(SCons.__version__)",
            ),
        },
        "python": {
            "reportedVersion": first_line(str(python_dir / "bin" / "python"), "--version"),
            "root": str(python_dir),
        },
        "qemu": {
            "reportedVersion": first_line("qemu-system-x86_64", "--version"),
            "packageVersion": package_version("qemu-system-x86"),
            "accelerator": environment.get("ZOS_QEMU_ACCEL"),
        },
        "ovmf": {
            "packageVersion": package_version("ovmf"),
            "code": environment.get("ZOS_OVMF_CODE"),
            "variablesTemplate": environment.get("ZOS_OVMF_VARS_TEMPLATE"),
        },
    },
    "hostPackages": {
        name: package_version(name)
        for name in (
            "build-essential",
            "dosfstools",
            "gdb",
            "gdisk",
            "git",
            "mtools",
            "ovmf",
            "python3-venv",
            "qemu-system-x86",
            "qemu-utils",
        )
    },
}

Path(os.environ["MANIFEST_FILE"]).write_text(
    json.dumps(manifest, indent=2, sort_keys=True) + "\n",
    encoding="utf-8",
)
PY
}

install_llvm
install_scons
create_tool_facade
write_environment
write_manifest
verify_environment

cat <<EOF

zOS development environment prepared successfully.

Repository:
  ${REPOSITORY_ROOT}

Activate the environment:
  source .zos/Toolchain/Environment.sh

Verify it later:
  ./Tools/SetupDevEnv.sh --verify-only

Important WSL note:
  QEMU acceleration selected: $(grep '^export ZOS_QEMU_ACCEL=' "${ENVIRONMENT_FILE}" | cut -d= -f2-)
  TCG is expected when /dev/kvm is unavailable under WSL.
EOF
