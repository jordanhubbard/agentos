#!/usr/bin/env bash
# tools/bootstrap-guest.sh — Create agentOS E2E guest disk images from ISO files
#
# Automates guest OS installation from installer ISOs, producing raw disk images
# suitable for use as agentOS VMM guest disks in E2E tests.
#
# Usage:
#   tools/bootstrap-guest.sh <os>
#   tools/bootstrap-guest.sh <os> <output-image>
#
# Supported OS targets:
#   ubuntu-amd64    Ubuntu 24.04 LTS x86_64 server  (qemu_virt x86_64 guests)
#   ubuntu-arm64    Ubuntu 26.04 ARM64 desktop       (qemu_virt_aarch64 guests)
#   nixos           NixOS 25.11 x86_64 minimal       (qemu_virt x86_64 guests)
#   freebsd15       FreeBSD 15.0 AMD64               (qemu_virt x86_64 guests)
#
# Environment:
#   ISO_DIR         directory containing ISO files (default: /Volumes/ISOs)
#   GUEST_IMG_DIR   output directory (default: guest-images/)
#   E2E_SSH_PUBKEY  path to test SSH public key (default: tests/e2e/id_ed25519.pub)
#   DISK_SIZE_GB    guest disk image size in GB (default: 20)
#   QEMU_MEM_MB     RAM to give installer VM in MB (default: 2048)
#
# ISO filenames expected in ISO_DIR:
#   ubuntu-amd64:  ubuntu-24.04-live-server-amd64.iso
#   ubuntu-arm64:  ubuntu-26.04-desktop-arm64.iso
#   nixos:         nixos-minimal-25.11-x86_64-linux.iso
#   freebsd15:     FreeBSD-15.0-RELEASE-amd64-bootonly.iso  (or disc1)
#
# Prerequisites:
#   All:           qemu-system-{x86_64,aarch64}
#   ubuntu-*:      mkisofs OR hdiutil (macOS built-in)
#   nixos:         /usr/bin/expect (macOS built-in; brew install expect on Linux)
#   freebsd15:     /usr/bin/expect  (or: curl to download VM image instead)
#
# Notes:
#   - The NixOS and FreeBSD installers require ~15-30 minutes in QEMU without KVM.
#   - Pass E2E_SKIP_ISO_INSTALL=1 to download pre-built cloud images instead
#     (faster; requires internet; bypasses the local ISOs).

set -euo pipefail

# ── Helpers ────────────────────────────────────────────────────────────────────

BOLD='\033[1m'; GREEN='\033[0;32m'; RED='\033[0;31m'; YELLOW='\033[0;33m'; RESET='\033[0m'
info()  { printf "${BOLD}[bootstrap]${RESET} %s\n" "$*"; }
ok()    { printf "${GREEN}[ok]${RESET} %s\n" "$*"; }
warn()  { printf "${YELLOW}[warn]${RESET} %s\n" "$*"; }
die()   { printf "${RED}[error]${RESET} %s\n" "$*" >&2; exit 1; }

# ── Configuration ──────────────────────────────────────────────────────────────

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

ISO_DIR="${ISO_DIR:-/Volumes/ISOs}"
GUEST_IMG_DIR="${GUEST_IMG_DIR:-${REPO_ROOT}/guest-images}"
E2E_SSH_PUBKEY="${E2E_SSH_PUBKEY:-${REPO_ROOT}/tests/e2e/id_ed25519.pub}"
DISK_SIZE_GB="${DISK_SIZE_GB:-20}"
QEMU_MEM_MB="${QEMU_MEM_MB:-2048}"
E2E_SKIP_ISO_INSTALL="${E2E_SKIP_ISO_INSTALL:-0}"

OS="${1:-}"
[ -z "${OS}" ] && die "Usage: bootstrap-guest.sh <ubuntu-amd64|ubuntu-arm64|nixos|freebsd15> [output-image]"

OUTPUT_IMG="${2:-}"

mkdir -p "${GUEST_IMG_DIR}"

# ── SSH key ────────────────────────────────────────────────────────────────────

ensure_ssh_key() {
    local key_path="${REPO_ROOT}/tests/e2e/id_ed25519"
    if [ ! -f "${key_path}" ]; then
        info "Generating test SSH key..."
        ssh-keygen -t ed25519 -N "" -f "${key_path}" -C "agentos-e2e-test" >/dev/null
        chmod 600 "${key_path}"
    fi
    E2E_SSH_PUBKEY="${key_path}.pub"
    [ -f "${E2E_SSH_PUBKEY}" ] || die "SSH public key not found: ${E2E_SSH_PUBKEY}"
    SSH_PUBKEY_CONTENT="$(cat "${E2E_SSH_PUBKEY}")"
}

# ── Seed ISO creation (Ubuntu cloud-init CIDATA) ───────────────────────────────

# Creates a CIDATA seed ISO from a directory of cloud-init files.
# On macOS uses hdiutil; on Linux uses mkisofs/genisoimage.
make_cidata_iso() {
    local seed_dir="$1"
    local out_iso="$2"

    if command -v hdiutil >/dev/null 2>&1; then
        # macOS
        hdiutil makehybrid \
            -o "${out_iso}" \
            -joliet \
            -iso \
            -default-volume-name CIDATA \
            "${seed_dir}" >/dev/null 2>&1
    elif command -v mkisofs >/dev/null 2>&1; then
        mkisofs -output "${out_iso}" \
            -volid CIDATA -joliet -rock "${seed_dir}" >/dev/null 2>&1
    elif command -v genisoimage >/dev/null 2>&1; then
        genisoimage -output "${out_iso}" \
            -volid CIDATA -joliet -rock "${seed_dir}" >/dev/null 2>&1
    else
        die "Cannot create seed ISO: install hdiutil (macOS) or mkisofs/genisoimage (Linux)"
    fi
}

# ── Download fallback (cloud images) ──────────────────────────────────────────

download_nixos_cloud_image() {
    local out="$1"
    local ver="25.11"
    # NixOS provides minimal QCOW2 images via the hydra build system
    local base="https://channels.nixos.org/nixos-${ver}"
    info "Fetching NixOS ${ver} QCOW2 cloud image from nixos.org..."
    local fname="nixos-${ver}-x86_64-linux.qcow2.zst"
    local url="${base}/nixos-${ver}-x86_64-linux.qcow2.zst"
    local tmp="${out}.qcow2.zst"
    curl -L --progress-bar -o "${tmp}" "${url}" || die "Download failed: ${url}"
    if command -v zstd >/dev/null 2>&1; then
        zstd -d "${tmp}" -o "${out}.qcow2"
    else
        die "zstd not found — install it or use ISO mode (unset E2E_SKIP_ISO_INSTALL)"
    fi
    qemu-img convert -f qcow2 -O raw "${out}.qcow2" "${out}"
    rm -f "${tmp}" "${out}.qcow2"
}

download_freebsd15_vm_image() {
    local out="$1"
    local base="https://download.freebsd.org/releases/VM-IMAGES/15.0-RELEASE/amd64/Latest"
    local fname="FreeBSD-15.0-RELEASE-amd64.raw.xz"
    info "Fetching FreeBSD 15.0 VM image from freebsd.org..."
    local tmp="${out}.raw.xz"
    curl -L --progress-bar -o "${tmp}" "${base}/${fname}" || die "Download failed"
    xz -d "${tmp}"   # produces ${out%.raw.xz}.raw — rename
    mv "${tmp%.xz}" "${out}"
}

# ── Expect runner ─────────────────────────────────────────────────────────────

EXPECT_BIN=""
find_expect() {
    for p in /usr/bin/expect /usr/local/bin/expect /opt/homebrew/bin/expect; do
        [ -x "${p}" ] && EXPECT_BIN="${p}" && return 0
    done
    command -v expect >/dev/null 2>&1 && EXPECT_BIN="$(command -v expect)" && return 0
    return 1
}

# ── Ubuntu AMD64 bootstrap ─────────────────────────────────────────────────────
#
# Ubuntu 24.04 server ISO supports cloud-init "autoinstall" natively.
# We create a CIDATA seed ISO with user-data (autoinstall.yaml) and mount it
# alongside the Ubuntu installer ISO.  The installer runs completely unattended.

bootstrap_ubuntu_amd64() {
    local iso="${ISO_DIR}/ubuntu-24.04-live-server-amd64.iso"
    local out="${OUTPUT_IMG:-${GUEST_IMG_DIR}/ubuntu-amd64.img}"
    local qemu="qemu-system-x86_64"

    command -v "${qemu}" >/dev/null 2>&1 || die "qemu-system-x86_64 not found"
    [ -f "${iso}" ] || die "Ubuntu ISO not found: ${iso}"

    ensure_ssh_key

    info "Bootstrapping Ubuntu 24.04 amd64 → ${out}"
    qemu-img create -f raw "${out}" "${DISK_SIZE_GB}G"

    # Build cloud-init seed directory
    local seed_dir
    seed_dir="$(mktemp -d)"
    trap 'rm -rf "${seed_dir}"' EXIT INT TERM

    # meta-data (minimal)
    cat > "${seed_dir}/meta-data" << 'META'
instance-id: agentos-e2e
local-hostname: agentos-guest
META

    # user-data (Ubuntu autoinstall — subiquity format)
    cat > "${seed_dir}/user-data" << AUTOINSTALL
#cloud-config
autoinstall:
  version: 1
  identity:
    hostname: agentos-guest
    username: root
    password: '!'
  ssh:
    install-server: true
    allow-pw: false
    authorized-keys:
      - '${SSH_PUBKEY_CONTENT}'
  storage:
    layout:
      name: direct
  late-commands:
    - 'echo "PermitRootLogin yes" >> /target/etc/ssh/sshd_config'
    - 'mkdir -p /target/root/.ssh'
    - 'echo "${SSH_PUBKEY_CONTENT}" > /target/root/.ssh/authorized_keys'
    - 'chmod 700 /target/root/.ssh && chmod 600 /target/root/.ssh/authorized_keys'
  shutdown: poweroff
AUTOINSTALL

    local seed_iso="${seed_dir}/seed.iso"
    make_cidata_iso "${seed_dir}" "${seed_iso}"

    info "Launching Ubuntu installer in QEMU (headless — this takes ~10-20 min)..."
    info "Serial log: /tmp/ubuntu-install-$$.log"

    # Ubuntu autoinstall: pass ds=nocloud;seedfrom via kernel cmdline
    "${qemu}" \
        -machine q35 -m "${QEMU_MEM_MB}M" \
        -cpu host 2>/dev/null || true
    "${qemu}" \
        -machine q35 -m "${QEMU_MEM_MB}M" \
        -drive "file=${iso},readonly=on,media=cdrom,format=raw" \
        -drive "file=${seed_iso},readonly=on,media=cdrom,format=raw,index=1" \
        -drive "file=${out},if=virtio,format=raw" \
        -nographic \
        -serial "file:/tmp/ubuntu-install-$$.log" \
        -no-reboot 2>/dev/null &
    local qemu_pid=$!

    info "Waiting for Ubuntu autoinstall to complete (poweroff signal)..."
    local waited=0
    while kill -0 "${qemu_pid}" 2>/dev/null; do
        sleep 10
        waited=$(( waited + 10 ))
        if grep -qF "Installation complete" /tmp/ubuntu-install-$$.log 2>/dev/null; then
            info "Autoinstall complete (${waited}s)"
        fi
        if [ "${waited}" -ge 2400 ]; then
            kill "${qemu_pid}" 2>/dev/null || true
            die "Ubuntu install did not complete within 40 min"
        fi
    done

    rm -f "/tmp/ubuntu-install-$$.log"
    ok "Ubuntu amd64 image ready: ${out}"
}

# ── Ubuntu ARM64 bootstrap ─────────────────────────────────────────────────────

bootstrap_ubuntu_arm64() {
    local iso="${ISO_DIR}/ubuntu-26.04-desktop-arm64.iso"
    local out="${OUTPUT_IMG:-${GUEST_IMG_DIR}/ubuntu-arm64.img}"
    local qemu="qemu-system-aarch64"
    local firmware="${GUEST_IMG_DIR}/edk2-aarch64-code.fd"

    command -v "${qemu}" >/dev/null 2>&1 || die "qemu-system-aarch64 not found"
    [ -f "${iso}" ] || die "Ubuntu ARM64 ISO not found: ${iso}"
    [ -f "${firmware}" ] || die "UEFI firmware not found: ${firmware} (run: make fetch-guest)"

    ensure_ssh_key

    info "Bootstrapping Ubuntu 26.04 arm64 → ${out}"
    qemu-img create -f raw "${out}" "${DISK_SIZE_GB}G"

    local seed_dir
    seed_dir="$(mktemp -d)"
    trap 'rm -rf "${seed_dir}"' EXIT INT TERM

    cat > "${seed_dir}/meta-data" << 'META'
instance-id: agentos-e2e-arm64
local-hostname: agentos-guest-arm64
META

    cat > "${seed_dir}/user-data" << AUTOINSTALL
#cloud-config
autoinstall:
  version: 1
  identity:
    hostname: agentos-guest-arm64
    username: root
    password: '!'
  ssh:
    install-server: true
    allow-pw: false
    authorized-keys:
      - '${SSH_PUBKEY_CONTENT}'
  storage:
    layout:
      name: direct
  late-commands:
    - 'echo "PermitRootLogin yes" >> /target/etc/ssh/sshd_config'
    - 'mkdir -p /target/root/.ssh'
    - 'echo "${SSH_PUBKEY_CONTENT}" > /target/root/.ssh/authorized_keys'
    - 'chmod 700 /target/root/.ssh && chmod 600 /target/root/.ssh/authorized_keys'
  shutdown: poweroff
AUTOINSTALL

    local seed_iso="${seed_dir}/seed.iso"
    make_cidata_iso "${seed_dir}" "${seed_iso}"

    info "Launching Ubuntu ARM64 installer in QEMU (headless)..."
    "${qemu}" \
        -machine virt,virtualization=on \
        -cpu cortex-a57 -m "${QEMU_MEM_MB}M" \
        -drive "if=pflash,format=raw,file=${firmware},readonly=on" \
        -drive "file=${iso},readonly=on,media=cdrom,format=raw" \
        -drive "file=${seed_iso},readonly=on,media=cdrom,format=raw,index=1" \
        -drive "file=${out},if=virtio,format=raw" \
        -nographic \
        -serial "file:/tmp/ubuntu-arm64-install-$$.log" \
        -no-reboot 2>/dev/null &
    local qemu_pid=$!

    info "Waiting for Ubuntu ARM64 autoinstall to complete..."
    local waited=0
    while kill -0 "${qemu_pid}" 2>/dev/null; do
        sleep 15
        waited=$(( waited + 15 ))
        [ "${waited}" -ge 3600 ] && kill "${qemu_pid}" 2>/dev/null || true && \
            die "Ubuntu ARM64 install did not complete within 60 min"
    done

    rm -f "/tmp/ubuntu-arm64-install-$$.log"
    ok "Ubuntu arm64 image ready: ${out}"
}

# ── NixOS bootstrap ────────────────────────────────────────────────────────────
#
# NixOS minimal ISO boots directly to a root shell on the serial console
# (no login prompt — it auto-logs in as root).  We drive the installation
# with expect: partition, format, mount, write configuration.nix, install.

bootstrap_nixos() {
    local iso="${ISO_DIR}/nixos-minimal-25.11-x86_64-linux.iso"
    local out="${OUTPUT_IMG:-${GUEST_IMG_DIR}/nixos.img}"
    local qemu="qemu-system-x86_64"

    if [ "${E2E_SKIP_ISO_INSTALL}" = "1" ]; then
        download_nixos_cloud_image "${out}"
        return 0
    fi

    command -v "${qemu}" >/dev/null 2>&1 || die "qemu-system-x86_64 not found"
    [ -f "${iso}" ] || die "NixOS ISO not found: ${iso}"
    find_expect || die "expect not found — install it (brew install expect) or set E2E_SKIP_ISO_INSTALL=1"

    ensure_ssh_key

    info "Bootstrapping NixOS 25.11 → ${out}"
    qemu-img create -f raw "${out}" "${DISK_SIZE_GB}G"

    local serial_sock="/tmp/nixos-install-$$.sock"

    # Write the expect script to a temp file
    local expect_script
    expect_script="$(mktemp /tmp/nixos-install-XXXXXX.exp)"
    cat > "${expect_script}" << EXPECT_SCRIPT
#!/usr/bin/expect -f

set timeout 600
set out  [lindex \$argv 0]
set iso  [lindex \$argv 1]
set key  [lindex \$argv 2]

# Launch QEMU
spawn qemu-system-x86_64 \\
    -machine q35 -m ${QEMU_MEM_MB} \\
    -drive "file=\$iso,readonly=on,media=cdrom,format=raw" \\
    -drive "file=\$out,if=virtio,format=raw" \\
    -nographic

# NixOS minimal boots to a root shell automatically — wait for prompt
# The prompt looks like: [root@nixos:~]#
puts "Waiting for NixOS to boot (up to 10 min)..."
expect {
    timeout         { puts "ERROR: NixOS did not boot"; exit 1 }
    "root@nixos"    { }
}
# Wait for the shell to be fully ready
sleep 3

# Partition the disk (GPT: EFI + root)
send "parted -s /dev/vda mklabel gpt \
    mkpart ESP fat32 1MiB 512MiB set 1 esp on \
    mkpart primary ext4 512MiB 100%\r"
expect "# "

send "mkfs.fat -F 32 -n boot /dev/vda1 && mkfs.ext4 -L nixos /dev/vda2\r"
expect "# "

send "mount /dev/disk/by-label/nixos /mnt && mkdir -p /mnt/boot && mount /dev/vda1 /mnt/boot\r"
expect "# "

send "nixos-generate-config --root /mnt\r"
expect "# "

# Write configuration.nix — use printf to avoid heredoc quoting issues in expect
send "printf '%s\\n' '{ config, pkgs, ... }:' > /mnt/etc/nixos/configuration.nix\r"
expect "# "
send "printf '%s\\n' '{ imports = \[ ./hardware-configuration.nix \];' >> /mnt/etc/nixos/configuration.nix\r"
expect "# "
send "printf '%s\\n' '  boot.loader.systemd-boot.enable = true;' >> /mnt/etc/nixos/configuration.nix\r"
expect "# "
send "printf '%s\\n' '  boot.loader.efi.canTouchEfiVariables = true;' >> /mnt/etc/nixos/configuration.nix\r"
expect "# "
send "printf '%s\\n' '  services.openssh.enable = true;' >> /mnt/etc/nixos/configuration.nix\r"
expect "# "
send "printf '%s\\n' '  services.openssh.settings.PermitRootLogin = \"yes\";' >> /mnt/etc/nixos/configuration.nix\r"
expect "# "
send "printf '%s\\n' '  services.openssh.settings.PasswordAuthentication = false;' >> /mnt/etc/nixos/configuration.nix\r"
expect "# "
send "printf '%s\\n' '  users.users.root.openssh.authorizedKeys.keys = \[ \"\$key\" \];' >> /mnt/etc/nixos/configuration.nix\r"
expect "# "
send "printf '%s\\n' '  networking.firewall.enable = false;' >> /mnt/etc/nixos/configuration.nix\r"
expect "# "
send "printf '%s\\n' '  environment.systemPackages = with pkgs; \[ vim curl wget \];' >> /mnt/etc/nixos/configuration.nix\r"
expect "# "
send "printf '%s\\n' '  system.stateVersion = \"25.11\";' >> /mnt/etc/nixos/configuration.nix\r"
expect "# "
send "printf '%s\\n' '}' >> /mnt/etc/nixos/configuration.nix\r"
expect "# "

puts "Running nixos-install (this takes 10-20 min)..."
send "nixos-install --no-root-passwd 2>&1 | tee /tmp/nixos-install.log\r"
set timeout 1800
expect {
    timeout              { puts "ERROR: nixos-install timed out"; exit 1 }
    "Installation finished" { }
    "installation finished" { }
    "error:"             { puts "ERROR: nixos-install failed"; exit 1 }
}
expect "# "

send "poweroff\r"
puts "NixOS installation complete."
expect eof
exit 0
EXPECT_SCRIPT
    chmod +x "${expect_script}"

    info "Running NixOS automated install via expect (15-25 min)..."
    "${EXPECT_BIN}" "${expect_script}" "${out}" "${iso}" "${SSH_PUBKEY_CONTENT}" || {
        rm -f "${expect_script}"
        die "NixOS installation failed — check QEMU output above"
    }
    rm -f "${expect_script}"

    ok "NixOS image ready: ${out}"
}

# ── FreeBSD 15 bootstrap ───────────────────────────────────────────────────────
#
# FreeBSD 15.0 bootonly ISO requires network access to fetch packages.
# Default: download the official VM image directly from freebsd.org.
# With E2E_SKIP_ISO_INSTALL=0 and a disc1 ISO: use expect + bsdinstall.

bootstrap_freebsd15() {
    local out="${OUTPUT_IMG:-${GUEST_IMG_DIR}/freebsd15-amd64.img}"
    local disc1_iso="${ISO_DIR}/FreeBSD-15.0-RELEASE-amd64-disc1.iso"
    local bootonly_iso="${ISO_DIR}/FreeBSD-15.0-RELEASE-amd64-bootonly.iso"
    local qemu="qemu-system-x86_64"

    # Prefer disc1 for offline install; fall back to download if only bootonly available
    local iso=""
    if [ "${E2E_SKIP_ISO_INSTALL}" = "0" ] && [ -f "${disc1_iso}" ]; then
        iso="${disc1_iso}"
    elif [ "${E2E_SKIP_ISO_INSTALL}" = "0" ] && [ -f "${bootonly_iso}" ]; then
        warn "Only bootonly ISO found — FreeBSD install requires internet access during install"
        iso="${bootonly_iso}"
    else
        info "Downloading FreeBSD 15.0 VM image (disc1 ISO not found at ${disc1_iso})"
        download_freebsd15_vm_image "${out}"
        return 0
    fi

    command -v "${qemu}" >/dev/null 2>&1 || die "qemu-system-x86_64 not found"
    find_expect || die "expect not found — install it or set E2E_SKIP_ISO_INSTALL=1 to download instead"

    ensure_ssh_key

    info "Bootstrapping FreeBSD 15.0 amd64 → ${out}"
    qemu-img create -f raw "${out}" "${DISK_SIZE_GB}G"

    local expect_script
    expect_script="$(mktemp /tmp/freebsd-install-XXXXXX.exp)"
    cat > "${expect_script}" << EXPECT_SCRIPT
#!/usr/bin/expect -f

set timeout 600
set out  [lindex \$argv 0]
set iso  [lindex \$argv 1]
set key  [lindex \$argv 2]

spawn qemu-system-x86_64 \\
    -machine q35 -m ${QEMU_MEM_MB} \\
    -drive "file=\$iso,readonly=on,media=cdrom,format=raw" \\
    -drive "file=\$out,if=virtio,format=raw" \\
    -nographic

# Wait for the FreeBSD boot menu
expect {
    timeout                     { puts "ERROR: FreeBSD did not boot"; exit 1 }
    "Welcome to FreeBSD"        { }
    "Booting FreeBSD"           { }
}
# Let autoboot timer expire (10s) or press Enter to boot immediately
sleep 12

# Wait for bsdinstall main menu
expect {
    timeout                     { puts "ERROR: bsdinstall did not appear"; exit 1 }
    "Install"                   { }
    "Shell"                     { }
}

# Select "Shell" to drive installation programmatically
send "\033\[B"   ;# Down arrow
sleep 0.5
# Navigate to Shell option and select it
# The exact menu position varies — use keyboard shortcut
send "s"
expect {
    "# "    { }
    timeout { puts "ERROR: Shell prompt not reached"; exit 1 }
}

# Run bsdinstall in scripted mode
send "set -x\r"
expect "# "

# Partition with gpart
send "gpart create -s gpt vtbd0\r"
expect "# "
send "gpart add -t efi -s 100m vtbd0\r"
expect "# "
send "gpart add -t freebsd-swap -s 512m vtbd0\r"
expect "# "
send "gpart add -t freebsd-ufs vtbd0\r"
expect "# "

send "newfs_msdos -F 32 -c 1 /dev/vtbd0p1\r"
expect "# "
send "newfs -U /dev/vtbd0p3\r"
expect "# "

send "mount /dev/vtbd0p3 /mnt\r"
expect "# "
send "mkdir -p /mnt/boot/efi && mount_msdosfs /dev/vtbd0p1 /mnt/boot/efi\r"
expect "# "

# Extract distributions
puts "Extracting FreeBSD base and kernel..."
send "BSDINSTALL_DISTDIR=/usr/freebsd-dist DISTRIBUTIONS='base.txz kernel.txz' bsdinstall distextract\r"
set timeout 1200
expect {
    timeout { puts "ERROR: distextract timed out"; exit 1 }
    "# "    { }
}
set timeout 600

# Configure the installed system
send "echo 'hostname=\"freebsd-guest\"' > /mnt/etc/rc.conf\r"
expect "# "
send "echo 'sshd_enable=\"YES\"' >> /mnt/etc/rc.conf\r"
expect "# "
send "echo 'ifconfig_vtnet0=\"DHCP\"' >> /mnt/etc/rc.conf\r"
expect "# "

# SSH configuration
send "echo 'PermitRootLogin yes' >> /mnt/etc/ssh/sshd_config\r"
expect "# "
send "echo 'PasswordAuthentication no' >> /mnt/etc/ssh/sshd_config\r"
expect "# "

# Inject SSH public key
send "mkdir -p /mnt/root/.ssh && chmod 700 /mnt/root/.ssh\r"
expect "# "
send "echo '\$key' > /mnt/root/.ssh/authorized_keys && chmod 600 /mnt/root/.ssh/authorized_keys\r"
expect "# "

# Install bootloader
send "bsdinstall bootconfig\r"
expect "# "
send "mount -t devfs devfs /mnt/dev\r"
expect "# "
send "chroot /mnt efibootmgr --verbose 2>/dev/null || true\r"
expect "# "

# fstab
send "echo '/dev/vtbd0p3 / ufs rw 1 1' > /mnt/etc/fstab\r"
expect "# "
send "echo '/dev/vtbd0p2 none swap sw 0 0' >> /mnt/etc/fstab\r"
expect "# "
send "echo 'proc /proc procfs rw 0 0' >> /mnt/etc/fstab\r"
expect "# "

send "sync && umount /mnt/dev /mnt/boot/efi /mnt\r"
expect "# "

send "poweroff\r"
puts "FreeBSD installation complete."
expect eof
exit 0
EXPECT_SCRIPT
    chmod +x "${expect_script}"

    info "Running FreeBSD automated install via expect (10-20 min)..."
    "${EXPECT_BIN}" "${expect_script}" "${out}" "${iso}" "${SSH_PUBKEY_CONTENT}" || {
        rm -f "${expect_script}"
        die "FreeBSD installation failed"
    }
    rm -f "${expect_script}"

    ok "FreeBSD 15 image ready: ${out}"
}

# ── Dispatch ───────────────────────────────────────────────────────────────────

case "${OS}" in
    ubuntu-amd64)   bootstrap_ubuntu_amd64 ;;
    ubuntu-arm64)   bootstrap_ubuntu_arm64 ;;
    nixos)          bootstrap_nixos ;;
    freebsd15)      bootstrap_freebsd15 ;;
    *)
        die "Unknown OS target '${OS}'. Valid: ubuntu-amd64 ubuntu-arm64 nixos freebsd15"
        ;;
esac
