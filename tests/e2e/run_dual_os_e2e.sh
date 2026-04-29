#!/usr/bin/env bash
# agentOS dual-OS E2E proof
#
# Builds Linux/Ubuntu and FreeBSD agentOS images, starts both under QEMU at the
# same time, then proves SSH connectivity to each guest by running `df /` while
# both QEMU instances are still alive.
#
# Exit codes:
#   0 - PASS
#   1 - FAIL
#   2 - SKIP only when E2E_DUAL_ALLOW_SKIP is set
#
# Environment:
#   E2E_TIMEOUT              seconds to wait for each guest SSH (default: 600)
#   E2E_BOARD                board to build/run (default: qemu_virt_aarch64)
#   E2E_QEMU                 QEMU binary (default: qemu-system-aarch64)
#   E2E_DUAL_NO_BUILD        set to 1 to reuse provided agentOS image paths
#   E2E_LINUX_AGENTOS_IMAGE  Linux/Ubuntu agentOS image when NO_BUILD=1
#   E2E_FREEBSD_AGENTOS_IMAGE FreeBSD agentOS image when NO_BUILD=1
#   E2E_DUAL_UBUNTU_IMG      Ubuntu guest disk image override
#   E2E_DUAL_FREEBSD_IMG     FreeBSD guest disk image override
#   E2E_DUAL_FREEBSD_PROVISION set to 0 to use FreeBSD disk as-is
#   E2E_FREEBSD_KERNEL_IMAGE FreeBSD kernel.bin for provisioning with NO_BUILD
#   E2E_UBUNTU_SSH_PORT      host port for Ubuntu SSH (default: 12222)
#   E2E_FREEBSD_SSH_PORT     host port for FreeBSD SSH (default: 12223)
#   E2E_SEED_PORT            Ubuntu NoCloud-Net seed port (must match DTS: 18790)
#   E2E_QEMU_IMG             qemu-img binary (default: qemu-img)
#   E2E_SSH_KEY              SSH key (default: tests/e2e/id_ed25519)
#   E2E_DUAL_KEEP_TMP        keep temp images/logs on exit
#   E2E_DUAL_ALLOW_SKIP      exit 2 instead of 1 for missing prerequisites

set -uo pipefail

if [ -t 1 ]; then
    RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[0;33m'
    BOLD='\033[1m'; RESET='\033[0m'; CYAN='\033[0;36m'
else
    RED=''; GREEN=''; YELLOW=''; BOLD=''; RESET=''; CYAN=''
fi

pass() { printf "${GREEN}[PASS]${RESET} %s\n" "$*"; }
fail() { printf "${RED}[FAIL]${RESET} %s\n" "$*"; }
skip() { printf "${YELLOW}[SKIP]${RESET} %s\n" "$*"; }
info() { printf "${BOLD}[INFO]${RESET} %s\n" "$*"; }
warn() { printf "${YELLOW}[WARN]${RESET} %s\n" "$*"; }
section() { printf "\n${CYAN}--- %s ---${RESET}\n" "$*"; }

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

E2E_TIMEOUT="${E2E_TIMEOUT:-600}"
E2E_BOARD="${E2E_BOARD:-qemu_virt_aarch64}"
E2E_QEMU="${E2E_QEMU:-qemu-system-aarch64}"
E2E_QEMU_IMG="${E2E_QEMU_IMG:-qemu-img}"
E2E_UBUNTU_SSH_PORT="${E2E_UBUNTU_SSH_PORT:-12222}"
E2E_FREEBSD_SSH_PORT="${E2E_FREEBSD_SSH_PORT:-12223}"
E2E_SEED_PORT="${E2E_SEED_PORT:-18790}"
E2E_SSH_KEY="${E2E_SSH_KEY:-${SCRIPT_DIR}/id_ed25519}"

DEFAULT_BUILD_DIR="${REPO_ROOT}/build/${E2E_BOARD}"
LOADER_ELF="${E2E_LOADER_ELF:-}"
FREEBSD_KERNEL_IMAGE="${E2E_FREEBSD_KERNEL_IMAGE:-}"
FREEBSD_DISK_FORMAT="raw"
FREEBSD_DISK_SNAPSHOT="1"

TMP_DIR=""
UBUNTU_QEMU_PID=""
FREEBSD_QEMU_PID=""
SEED_HTTP_PID=""
SEED_DIR=""

finish_with_missing_prereq() {
    if [ -n "${E2E_DUAL_ALLOW_SKIP:-}" ]; then
        skip "$*"
        exit 2
    fi
    fail "$*"
    exit 1
}

cleanup() {
    local code=$?
    if [ -n "${SEED_HTTP_PID}" ]; then
        kill "${SEED_HTTP_PID}" 2>/dev/null || true
        wait "${SEED_HTTP_PID}" 2>/dev/null || true
    fi
    if [ -n "${UBUNTU_QEMU_PID}" ]; then
        kill "${UBUNTU_QEMU_PID}" 2>/dev/null || true
        wait "${UBUNTU_QEMU_PID}" 2>/dev/null || true
    fi
    if [ -n "${FREEBSD_QEMU_PID}" ]; then
        kill "${FREEBSD_QEMU_PID}" 2>/dev/null || true
        wait "${FREEBSD_QEMU_PID}" 2>/dev/null || true
    fi
    if [ -n "${SEED_DIR}" ]; then
        rm -rf "${SEED_DIR}"
    fi
    if [ -n "${TMP_DIR}" ] && [ -z "${E2E_DUAL_KEEP_TMP:-}" ]; then
        rm -rf "${TMP_DIR}"
    elif [ -n "${TMP_DIR}" ]; then
        info "Keeping temp dir: ${TMP_DIR}"
    fi
    exit "${code}"
}
trap cleanup EXIT INT TERM

require_command() {
    local cmd="$1"
    if ! command -v "${cmd}" >/dev/null 2>&1; then
        finish_with_missing_prereq "required command not found: ${cmd}"
    fi
}

require_file() {
    local path="$1"
    local label="$2"
    if [ ! -f "${path}" ]; then
        finish_with_missing_prereq "${label} not found: ${path}"
    fi
}

require_port_free() {
    local port="$1"
    python3 - "${port}" <<'PY'
import socket
import sys

port = int(sys.argv[1])
sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
try:
    sock.bind(("127.0.0.1", port))
except OSError:
    sys.exit(1)
finally:
    sock.close()
PY
    if [ $? -ne 0 ]; then
        fail "host TCP port ${port} is already in use"
        exit 1
    fi
}

default_ubuntu_img() {
    local cached="${HOME}/.local/agentos-images/ubuntu-24.04-aarch64.img"
    if [ -f "${cached}" ]; then
        printf "%s\n" "${cached}"
    else
        printf "%s\n" "${REPO_ROOT}/guest-images/ubuntu-24.04-aarch64.img"
    fi
}

default_freebsd_img() {
    if [ -f "${REPO_ROOT}/guest-images/freebsd.img" ]; then
        printf "%s\n" "${REPO_ROOT}/guest-images/freebsd.img"
    elif [ -f "${REPO_ROOT}/guest-images/freebsd-14.4-aarch64.img" ]; then
        printf "%s\n" "${REPO_ROOT}/guest-images/freebsd-14.4-aarch64.img"
    else
        printf "%s\n" "${HOME}/.local/agentos-images/freebsd-14.4-aarch64.img"
    fi
}

abs_path() {
    case "$1" in
        /*) printf "%s\n" "$1" ;;
        *) printf "%s/%s\n" "${REPO_ROOT}" "$1" ;;
    esac
}

ensure_ssh_key() {
    if [ ! -f "${E2E_SSH_KEY}" ]; then
        info "Generating test SSH key: ${E2E_SSH_KEY}"
        ssh-keygen -t ed25519 -N "" -f "${E2E_SSH_KEY}" -C "agentos-e2e-test" >/dev/null 2>&1
        if [ $? -ne 0 ]; then
            fail "ssh-keygen failed"
            exit 1
        fi
        chmod 600 "${E2E_SSH_KEY}"
    fi
    require_file "${E2E_SSH_KEY}.pub" "SSH public key"
}

start_ubuntu_seed_server() {
    local pubkey
    pubkey="$(cat "${E2E_SSH_KEY}.pub")"
    SEED_DIR="$(mktemp -d "/tmp/agentos-dual-nocloud.XXXXXX")"
    {
        printf "instance-id: agentos-dual-ubuntu-%s-%s\n" "$$" "$(date +%s)"
        printf "local-hostname: agentos-linux\n"
    } > "${SEED_DIR}/meta-data"
    : > "${SEED_DIR}/vendor-data"
    {
        printf "%s\n" "#cloud-config"
        printf "%s\n" "disable_root: false"
        printf "%s\n" "ssh_pwauth: true"
        printf "%s\n" "ssh_authorized_keys:"
        printf "  - %s\n" "${pubkey}"
        printf "%s\n" "users:"
        printf "%s\n" "  - default"
        printf "%s\n" "  - name: ubuntu"
        printf "%s\n" "    lock_passwd: false"
        printf "%s\n" "    groups: [adm, sudo]"
        printf "%s\n" "    shell: /bin/bash"
        printf "%s\n" "    ssh_authorized_keys:"
        printf "      - %s\n" "${pubkey}"
        printf "%s\n" "  - name: root"
        printf "%s\n" "    lock_passwd: false"
        printf "%s\n" "    ssh_authorized_keys:"
        printf "      - %s\n" "${pubkey}"
        printf "%s\n" "chpasswd:"
        printf "%s\n" "  expire: false"
        printf "%s\n" "  users:"
        printf "%s\n" "    - name: root"
        printf "%s\n" "      password: agentos"
        printf "%s\n" "      type: text"
        printf "%s\n" "write_files:"
        printf "%s\n" "  - path: /root/.ssh/authorized_keys"
        printf "%s\n" "    owner: root:root"
        printf "%s\n" "    permissions: '0600'"
        printf "%s\n" "    content: |"
        printf "      %s\n" "${pubkey}"
    } > "${SEED_DIR}/user-data"

    python3 -m http.server "${E2E_SEED_PORT}" --bind 127.0.0.1 \
        --directory "${SEED_DIR}" > "${TMP_DIR}/ubuntu-seed.log" 2>&1 &
    SEED_HTTP_PID=$!
    sleep 1
    if ! kill -0 "${SEED_HTTP_PID}" 2>/dev/null; then
        fail "Ubuntu NoCloud-Net seed server failed to start on port ${E2E_SEED_PORT}"
        exit 1
    fi
    info "Ubuntu NoCloud-Net seed: http://127.0.0.1:${E2E_SEED_PORT}/"
}

build_or_select_images() {
    if [ "${E2E_DUAL_NO_BUILD:-0}" = "1" ]; then
        LINUX_AGENTOS_IMAGE="${E2E_LINUX_AGENTOS_IMAGE:-}"
        FREEBSD_AGENTOS_IMAGE="${E2E_FREEBSD_AGENTOS_IMAGE:-}"
        if [ -z "${LOADER_ELF}" ]; then
            LOADER_ELF="${DEFAULT_BUILD_DIR}/loader.elf"
        fi
        [ -n "${LINUX_AGENTOS_IMAGE}" ] || finish_with_missing_prereq "E2E_LINUX_AGENTOS_IMAGE is required with E2E_DUAL_NO_BUILD=1"
        [ -n "${FREEBSD_AGENTOS_IMAGE}" ] || finish_with_missing_prereq "E2E_FREEBSD_AGENTOS_IMAGE is required with E2E_DUAL_NO_BUILD=1"
        require_file "${LINUX_AGENTOS_IMAGE}" "Linux agentOS image"
        require_file "${FREEBSD_AGENTOS_IMAGE}" "FreeBSD agentOS image"
        return
    fi

    local linux_build_dir="${TMP_DIR}/build-ubuntu"
    local freebsd_build_dir="${TMP_DIR}/build-freebsd"

    section "Build Linux/Ubuntu agentOS image"
    (cd "${REPO_ROOT}" && make build BOARD="${E2E_BOARD}" TARGET_ARCH=aarch64 GUEST_OS=ubuntu BUILD_DIR="${linux_build_dir}")
    if [ $? -ne 0 ]; then
        fail "Linux/Ubuntu image build failed"
        exit 1
    fi
    require_file "${linux_build_dir}/agentos.img" "built Linux/Ubuntu agentOS image"
    LINUX_AGENTOS_IMAGE="${linux_build_dir}/agentos.img"
    if [ -z "${LOADER_ELF}" ]; then
        LOADER_ELF="${linux_build_dir}/loader.elf"
    fi

    section "Build FreeBSD agentOS image"
    (cd "${REPO_ROOT}" && make build BOARD="${E2E_BOARD}" TARGET_ARCH=aarch64 GUEST_OS=freebsd BUILD_DIR="${freebsd_build_dir}")
    if [ $? -ne 0 ]; then
        fail "FreeBSD image build failed"
        exit 1
    fi
    require_file "${freebsd_build_dir}/agentos.img" "built FreeBSD agentOS image"
    FREEBSD_AGENTOS_IMAGE="${freebsd_build_dir}/agentos.img"
    FREEBSD_KERNEL_IMAGE="${freebsd_build_dir}/freebsd-kernel.bin"
}

prepare_freebsd_disk() {
    if [ "${E2E_DUAL_FREEBSD_PROVISION:-1}" = "0" ]; then
        FREEBSD_DISK_FORMAT="raw"
        FREEBSD_DISK_SNAPSHOT="1"
        return
    fi

    require_command "${E2E_QEMU_IMG}"
    require_command expect
    require_file "${SCRIPT_DIR}/provision_freebsd_ssh.expect" "FreeBSD SSH provisioning helper"

    if [ -z "${FREEBSD_KERNEL_IMAGE}" ]; then
        FREEBSD_KERNEL_IMAGE="${TMP_DIR}/freebsd-kernel.bin"
        require_file "${REPO_ROOT}/tools/extract_freebsd_file.py" "FreeBSD UFS extractor"
        info "Extracting FreeBSD kernel for provisioning"
        python3 "${REPO_ROOT}/tools/extract_freebsd_file.py" \
            "${FREEBSD_GUEST_IMG}" /boot/kernel/kernel.bin "${FREEBSD_KERNEL_IMAGE}"
        if [ $? -ne 0 ]; then
            fail "failed to extract FreeBSD kernel from ${FREEBSD_GUEST_IMG}"
            exit 1
        fi
    fi
    require_file "${FREEBSD_KERNEL_IMAGE}" "FreeBSD kernel image for provisioning"

    local backing
    backing="$(abs_path "${FREEBSD_GUEST_IMG}")"
    local provisioned="${TMP_DIR}/freebsd-provisioned.qcow2"

    section "Provision FreeBSD SSH overlay"
    "${E2E_QEMU_IMG}" create -f qcow2 -F raw -b "${backing}" "${provisioned}" >/dev/null
    if [ $? -ne 0 ]; then
        fail "failed to create FreeBSD qcow2 overlay"
        exit 1
    fi

    expect "${SCRIPT_DIR}/provision_freebsd_ssh.expect" \
        "${FREEBSD_KERNEL_IMAGE}" \
        "${provisioned}" \
        "${E2E_FREEBSD_SSH_PORT}" \
        "$(cat "${E2E_SSH_KEY}.pub")" \
        > "${TMP_DIR}/freebsd-provision.log" 2>&1
    if [ $? -ne 0 ]; then
        fail "failed to provision FreeBSD SSH overlay"
        tail -80 "${TMP_DIR}/freebsd-provision.log" 2>/dev/null || true
        exit 1
    fi

    FREEBSD_GUEST_IMG="${provisioned}"
    FREEBSD_DISK_FORMAT="qcow2"
    FREEBSD_DISK_SNAPSHOT="0"
    pass "FreeBSD SSH overlay provisioned"
}

start_qemu() {
    local name="$1"
    local image="$2"
    local disk="$3"
    local disk_bus="$4"
    local ssh_port="$5"
    local serial_log="$6"
    local qemu_log="$7"
    local disk_format="${8:-raw}"
    local disk_snapshot="${9:-1}"

    local netdev="user,id=net0,hostfwd=tcp:127.0.0.1:${ssh_port}-:22"
    local cc_sock="${TMP_DIR}/${name}-cc_pd.sock"
    local snapshot_opt=",snapshot=on"
    if [ "${disk_snapshot}" = "0" ]; then
        snapshot_opt=""
    fi
    rm -f "${cc_sock}"
    : > "${serial_log}"
    : > "${qemu_log}"

    "${E2E_QEMU}" \
        -machine "virt,virtualization=on,highmem=off,secure=off" \
        -cpu cortex-a57 \
        -m 2G \
        -display none \
        -monitor none \
        -global "virtio-mmio.force-legacy=off" \
        -serial "file:${serial_log}" \
        -chardev "socket,id=cc_pd_char,path=${cc_sock},server=on,wait=off" \
        -device "virtio-serial-device,bus=virtio-mmio-bus.2,id=vser0" \
        -device "virtserialport,bus=vser0.0,chardev=cc_pd_char,name=cc.0" \
        -netdev "${netdev}" \
        -device "virtio-net-device,netdev=net0,bus=virtio-mmio-bus.0" \
        -device "loader,file=${LOADER_ELF},cpu-num=0" \
        -device "loader,file=${image},addr=0x48000000" \
        -device "virtio-blk-device,drive=${name}0,bus=virtio-mmio-bus.${disk_bus}" \
        -drive "file=${disk},format=${disk_format},id=${name}0,if=none${snapshot_opt}" \
        > "${qemu_log}" 2>&1 &

    printf "%s\n" "$!"
}

wait_for_marker() {
    local name="$1"
    local pid="$2"
    local log="$3"
    local marker="$4"
    local timeout="$5"
    local elapsed=0
    while [ "${elapsed}" -lt "${timeout}" ]; do
        if grep -qF "${marker}" "${log}" 2>/dev/null; then
            pass "${name}: saw '${marker}'"
            return 0
        fi
        if ! kill -0 "${pid}" 2>/dev/null; then
            fail "${name}: QEMU exited before '${marker}'"
            tail -40 "${log}" 2>/dev/null || true
            return 1
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done
    fail "${name}: timed out waiting for '${marker}'"
    tail -40 "${log}" 2>/dev/null || true
    return 1
}

ssh_cmd() {
    local user="$1"
    local port="$2"
    shift 2
    ssh -i "${E2E_SSH_KEY}" \
        -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null \
        -o NumberOfPasswordPrompts=0 \
        -o ConnectTimeout=2 \
        -o BatchMode=yes \
        -p "${port}" \
        "${user}@127.0.0.1" "$@"
}

wait_for_ssh_probe() {
    local name="$1"
    local user="$2"
    local port="$3"
    local pid="$4"
    local timeout="$5"
    shift 5
    local elapsed=0
    local last_err="${TMP_DIR}/${name}-ssh-last.err"
    : > "${last_err}"

    while [ "${elapsed}" -lt "${timeout}" ]; do
        if ! kill -0 "${pid}" 2>/dev/null; then
            fail "${name}: QEMU exited before SSH became ready"
            tail -40 "${TMP_DIR}/${name}-serial.log" 2>/dev/null || true
            return 1
        fi
        if ssh_cmd "${user}" "${port}" "$@" >/dev/null 2>"${last_err}"; then
            pass "${name}: SSH ready on port ${port}"
            return 0
        fi
        sleep 2
        elapsed=$((elapsed + 2))
    done

    fail "${name}: SSH probe timed out after ${timeout}s on port ${port}"
    if [ -s "${last_err}" ]; then
        sed -n '1,5p' "${last_err}" 2>/dev/null || true
    fi
    return 1
}

run_df_probe() {
    local name="$1"
    local user="$2"
    local port="$3"
    local output
    if ! output="$(ssh_cmd "${user}" "${port}" df / 2>/dev/null)"; then
        fail "${name}: df / failed over SSH"
        return 1
    fi
    if ! printf "%s\n" "${output}" | grep -qE '/[[:space:]]*$|/$'; then
        fail "${name}: df / output did not identify the root filesystem"
        printf "%s\n" "${output}"
        return 1
    fi
    printf "\n${BOLD}%s df / output:${RESET}\n%s\n" "${name}" "${output}"
    pass "${name}: df / completed over SSH"
    return 0
}

main() {
    cd "${REPO_ROOT}" || exit 1

    if [ "${E2E_BOARD}" != "qemu_virt_aarch64" ]; then
        finish_with_missing_prereq "dual-OS E2E currently supports qemu_virt_aarch64 only"
    fi
    if [ "${E2E_SEED_PORT}" != "18790" ]; then
        fail "E2E_SEED_PORT must be 18790 because the Ubuntu guest DTS is built with that NoCloud URL"
        exit 1
    fi

    require_command "${E2E_QEMU}"
    require_command make
    require_command ssh
    require_command ssh-keygen
    require_command python3

    require_port_free "${E2E_UBUNTU_SSH_PORT}"
    require_port_free "${E2E_FREEBSD_SSH_PORT}"
    require_port_free "${E2E_SEED_PORT}"

    TMP_DIR="$(mktemp -d "/tmp/agentos-dual-os.XXXXXX")"
    info "Temp dir: ${TMP_DIR}"

    ensure_ssh_key

    UBUNTU_GUEST_IMG="${E2E_DUAL_UBUNTU_IMG:-$(default_ubuntu_img)}"
    FREEBSD_GUEST_IMG="${E2E_DUAL_FREEBSD_IMG:-$(default_freebsd_img)}"
    require_file "${UBUNTU_GUEST_IMG}" "Ubuntu guest disk"
    require_file "${FREEBSD_GUEST_IMG}" "FreeBSD guest disk"

    build_or_select_images
    require_file "${LOADER_ELF}" "AArch64 loader"
    prepare_freebsd_disk

    start_ubuntu_seed_server

    section "Start both agentOS guests"
    UBUNTU_SERIAL_LOG="${TMP_DIR}/ubuntu-serial.log"
    FREEBSD_SERIAL_LOG="${TMP_DIR}/freebsd-serial.log"
    UBUNTU_QEMU_LOG="${TMP_DIR}/ubuntu-qemu.log"
    FREEBSD_QEMU_LOG="${TMP_DIR}/freebsd-qemu.log"

    UBUNTU_QEMU_PID="$(start_qemu ubuntu "${LINUX_AGENTOS_IMAGE}" "${UBUNTU_GUEST_IMG}" 1 "${E2E_UBUNTU_SSH_PORT}" "${UBUNTU_SERIAL_LOG}" "${UBUNTU_QEMU_LOG}" raw 1)"
    info "Ubuntu agentOS QEMU PID: ${UBUNTU_QEMU_PID}"

    FREEBSD_QEMU_PID="$(start_qemu freebsd "${FREEBSD_AGENTOS_IMAGE}" "${FREEBSD_GUEST_IMG}" 31 "${E2E_FREEBSD_SSH_PORT}" "${FREEBSD_SERIAL_LOG}" "${FREEBSD_QEMU_LOG}" "${FREEBSD_DISK_FORMAT}" "${FREEBSD_DISK_SNAPSHOT}")"
    info "FreeBSD agentOS QEMU PID: ${FREEBSD_QEMU_PID}"

    section "Wait for both agentOS instances"
    wait_for_marker ubuntu "${UBUNTU_QEMU_PID}" "${UBUNTU_SERIAL_LOG}" "agentOS boot complete" 90 || exit 1
    wait_for_marker freebsd "${FREEBSD_QEMU_PID}" "${FREEBSD_SERIAL_LOG}" "agentOS boot complete" 90 || exit 1

    section "Wait for guest SSH"
    wait_for_ssh_probe ubuntu ubuntu "${E2E_UBUNTU_SSH_PORT}" "${UBUNTU_QEMU_PID}" "${E2E_TIMEOUT}" \
        systemctl is-active --quiet multi-user.target || exit 1
    wait_for_ssh_probe freebsd root "${E2E_FREEBSD_SSH_PORT}" "${FREEBSD_QEMU_PID}" "${E2E_TIMEOUT}" \
        sh -c "uname -s | grep -qx FreeBSD && service sshd onestatus >/dev/null 2>&1" || exit 1

    section "Run df while both guests are alive"
    if ! kill -0 "${UBUNTU_QEMU_PID}" 2>/dev/null || ! kill -0 "${FREEBSD_QEMU_PID}" 2>/dev/null; then
        fail "one QEMU exited before dual df probe"
        exit 1
    fi

    run_df_probe ubuntu ubuntu "${E2E_UBUNTU_SSH_PORT}" || exit 1

    if ! kill -0 "${UBUNTU_QEMU_PID}" 2>/dev/null || ! kill -0 "${FREEBSD_QEMU_PID}" 2>/dev/null; then
        fail "one QEMU exited before FreeBSD df probe completed"
        exit 1
    fi

    run_df_probe freebsd root "${E2E_FREEBSD_SSH_PORT}" || exit 1

    if ! kill -0 "${UBUNTU_QEMU_PID}" 2>/dev/null || ! kill -0 "${FREEBSD_QEMU_PID}" 2>/dev/null; then
        fail "one QEMU exited before final liveness check"
        exit 1
    fi

    pass "dual-OS E2E: Linux/Ubuntu and FreeBSD were simultaneously running with SSH and df output"
}

main "$@"
