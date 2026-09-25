use crate::cmd_guest_profile::{self, DesktopPlan, HostProfilePlan};
use crate::guest_scenario::{self, HostScenarioPlan, ScenarioGuestPlan};
use crate::{rfb, QemuLaunchArgs, TestArgs};
use anyhow::Context;
use sha2::{Digest, Sha256};
use std::io::{IsTerminal, Read, Seek, SeekFrom, Write};
use std::net::{SocketAddr, TcpStream};
use std::ops::{Deref, DerefMut};
use std::os::unix::net::UnixStream;
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Child, Stdio};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

pub(crate) const SSH_AUTH_OPTIONS: &[&str] = &[
    "-F",
    "/dev/null",
    "-o",
    "BatchMode=yes",
    "-o",
    "PreferredAuthentications=publickey",
    "-o",
    "PasswordAuthentication=no",
    "-o",
    "KbdInteractiveAuthentication=no",
    "-o",
    "IdentitiesOnly=yes",
    "-o",
    "ConnectTimeout=30",
    "-o",
    "ConnectionAttempts=1",
    "-o",
    "StrictHostKeyChecking=no",
    "-o",
    "UserKnownHostsFile=/dev/null",
    "-o",
    "LogLevel=ERROR",
];
pub(crate) const SSH_PROBE_LIVENESS_OPTIONS: &[&str] =
    &["-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=1"];
const SSH_SESSION_LIVENESS_OPTIONS: &[&str] = &[
    "-o",
    "ServerAliveInterval=30",
    "-o",
    "ServerAliveCountMax=20",
];
const CC_WIRE_SHMEM_SIZE: usize = 4096;
const CC_INPUT_TEXT: u32 = 0x05;
/*
 * A text event crosses CC -> VibeEngine -> VM manager before reaching a
 * dynamic guest. The 24-byte event and 4-byte VM slot fit the 48-byte seL4
 * payload. Retain the existing bounded 16-byte host chunks for compatibility.
 */
const CC_INPUT_TEXT_CHUNK: usize = 16;
const CC_REQ_SIZE: usize = 4 + 12 + CC_WIRE_SHMEM_SIZE;
const CC_REPLY_SIZE: usize = 16 + CC_WIRE_SHMEM_SIZE;
const CC_IO_TIMEOUT: Duration = Duration::from_secs(5);
/*
 * A console drain crosses the host virtconsole, CC-PD,
 * vm_manager, and a running VMM. Those target components now have a strictly
 * ascending priority chain, so a full minute without frame progress means the
 * QEMU chardev lost the request or reply. Reconnect and replay the identical
 * request; CC-PD's retry cache makes completed state changes exactly-once.
 */
const CC_FRAME_DEADLINE: Duration = Duration::from_secs(60);
const CC_INPUT_RETRY_DEADLINE: Duration = Duration::from_secs(120);
const CC_OK: u32 = 0;
const CC_ERR_RELAY_FAULT: u32 = 8;
const CC_ERR_BAD_HANDLE: u32 = 6;
#[cfg(test)]
const VMM_RELAY_PAYLOAD_BYTES: usize = 48;
const MSG_CC_LOG_STREAM: u32 = 0x2610;
const MSG_CC_CREATE_GUEST: u32 = 0x2611;
const MSG_CC_LIST_GUESTS: u32 = 0x2607;
const MSG_CC_GUEST_STATUS: u32 = 0x260a;
const MSG_CC_SEND_INPUT: u32 = 0x260d;
const MSG_CC_SUSPEND_GUEST: u32 = 0x2613;
const MSG_CC_RESUME_GUEST: u32 = 0x2614;
const MSG_CC_DESTROY_GUEST: u32 = 0x2615;
const CC_INPUT_KEY_DOWN: u32 = 0x01;
const CC_INPUT_RAW_BYTE_BASE: u32 = 0x100;
const GUEST_DESTROY_NORMAL: u32 = 0;
const VIBEOS_ARCH_AARCH64: u8 = 0x01;
const VIBEOS_ARCH_X86_64: u8 = 0x02;
const VIBEOS_DEV_SERIAL: u32 = 1 << 0;
const VIBEOS_DEV_NET: u32 = 1 << 1;
const VIBEOS_DEV_BLOCK: u32 = 1 << 2;
const TRACE_PD_GUEST_VMM_PRIMARY: u32 = 41;
const TRACE_PD_GUEST_VMM_SECONDARY: u32 = 42;
const FOCUSED_NET_STIMULUS_PORT: u16 = 12224;

#[derive(Clone, Debug)]
struct VirtioAssertion {
    devices: Vec<String>,
    host_backed: bool,
    bidirectional_console: bool,
}

fn requested_virtio_assertion(
    args: &TestArgs,
    profile: Option<&HostProfilePlan>,
) -> Option<VirtioAssertion> {
    if args.block_isolation_probe.is_some()
        || args.virtualizer_authority_probe.is_some()
        || args.network_isolation_probe.is_some()
        || args.serial_isolation_probe.is_some()
    {
        return None;
    }
    if args.assert_agentos_virtio {
        return Some(VirtioAssertion {
            devices: vec!["net".into(), "block".into(), "console".into()],
            host_backed: true,
            bidirectional_console: true,
        });
    }
    if args.assert_emulated_console {
        return Some(VirtioAssertion {
            devices: vec!["console".into()],
            host_backed: false,
            bidirectional_console: true,
        });
    }
    if args.assert_emulated_net {
        return Some(VirtioAssertion {
            devices: vec!["net".into()],
            host_backed: false,
            bidirectional_console: false,
        });
    }
    if args.assert_emulated_blk {
        return Some(VirtioAssertion {
            devices: vec!["block".into()],
            host_backed: false,
            bidirectional_console: false,
        });
    }
    profile.and_then(|profile| {
        profile
            .test
            .iter()
            .find(|step| step.action == "assert-virtio")
            .map(|step| VirtioAssertion {
                devices: step.args["devices"].split(',').map(str::to_owned).collect(),
                host_backed: step
                    .args
                    .get("scope")
                    .is_some_and(|scope| scope == "host-backed"),
                bidirectional_console: step
                    .args
                    .get("console_io")
                    .is_some_and(|value| value == "bidirectional"),
            })
    })
}

fn virtio_markers(assertion: &VirtioAssertion) -> Vec<&'static str> {
    let mut required = Vec::new();
    for device in &assertion.devices {
        match device.as_str() {
            "net" => {
                required.extend_from_slice(&[
                    "emulated virtio-net: guest probed",
                    "emulated virtio-net: guest DRIVER_OK",
                    "emulated virtio-net: pumped",
                ]);
                if assertion.host_backed {
                    required.extend_from_slice(&[
                        "[net_virt] TX accepted by net_pd",
                        "[net_virt] RX delivered from net_pd",
                        "via net_virt",
                        "[net_pd] HOST_READY: virtio-net bus.16",
                        "[net_pd] HOST_TX: QEMU bus.16 completion observed",
                    ]);
                }
            }
            "block" => {
                required.extend_from_slice(&[
                    "emulated virtio-blk: guest probed",
                    "emulated virtio-blk: guest DRIVER_OK",
                    "emulated virtio-blk: pumped",
                ]);
                if assertion.host_backed {
                    required.extend_from_slice(&[
                        "[blk_virt] host media",
                        "[blk_virt] host-media read",
                    ]);
                }
            }
            "console" => {
                required.extend_from_slice(&[
                    "emulated virtio-console: guest probed",
                    "emulated virtio-console: guest DRIVER_OK",
                    "emulated virtio-console: pumped",
                ]);
                if assertion.bidirectional_console {
                    required.push("emulated virtio-console: pumped input serial_virt->guest");
                    required.push("[serial_virt] frontend input delivered to VMM queue");
                    required.push("[serial_virt] VMM output delivered to frontend queue");
                }
            }
            _ => {}
        }
    }
    required
}

fn persistence_script(token: &str, second_boot: bool) -> anyhow::Result<String> {
    anyhow::ensure!(
        !token.is_empty()
            && token.len() <= 128
            && token
                .bytes()
                .all(|c| c.is_ascii_alphanumeric() || c == b'-'),
        "invalid persistence witness token"
    );
    let file = "/var/lib/agentos/persistence-proof";
    let write = if second_boot {
        String::new()
    } else {
        format!("test ! -e {file}\nmkdir -p /var/lib/agentos\numask 077\nprintf '%s\\n' '{token}' >{file}\nsync\n")
    };
    Ok(format!(
        "set -eu\n{write}test \"$(cat {file})\" = '{token}'\nprintf '%s\\n' '{token}'\n"
    ))
}

fn run_persistent_boots(args: &TestArgs) -> anyhow::Result<()> {
    anyhow::ensure!(
        args.assert_live
            && !args.no_build
            && !args.keep_running
            && !args.assert_desktop
            && args.guest_os != "both"
            && args.guest_os != "none",
        "persistent proof requires one freshly built live guest profile"
    );
    let root = repo_root()?;
    let evidence = root.join("build/evidence");
    std::fs::create_dir_all(&evidence)?;
    let directory = tempfile::Builder::new()
        .prefix("persistent-boot-")
        .tempdir_in(&evidence)?
        .keep();
    println!(
        "[xtask:test] Persistent boot evidence: {}",
        directory.display()
    );
    let mut round = args.clone();
    round.assert_persistent_boots = false;
    round.persistent_directory = Some(directory.clone());
    round.persistent_token = directory
        .file_name()
        .unwrap()
        .to_string_lossy()
        .into_owned();
    let mut status = serde_json::json!({"schema":"agentos.persistent_boot.v1", "profile":args.guest_os,
        "status":"running", "first_boot":false, "second_boot":false,
        "scope":"two QEMU cold boots; not guest-slot recreation or orderly shutdown"});
    let receipt = directory.join("result.json");
    std::fs::write(&receipt, serde_json::to_vec_pretty(&status)?)?;
    for second in [false, true] {
        round.persistent_second_boot = second;
        round.no_build = second;
        if let Err(error) = run(&round) {
            status["status"] = serde_json::json!("failed");
            status["error"] = serde_json::json!(format!("{error:#}"));
            std::fs::write(&receipt, serde_json::to_vec_pretty(&status)?)?;
            return Err(error);
        }
        status[if second { "second_boot" } else { "first_boot" }] = serde_json::json!(true);
        std::fs::write(&receipt, serde_json::to_vec_pretty(&status)?)?;
    }
    status["status"] = serde_json::json!("pass");
    std::fs::write(receipt, serde_json::to_vec_pretty(&status)?)?;
    println!("PASS: persistent disk witness survived two authenticated guest cold boots");
    Ok(())
}

pub fn run_x86_storage(timeout_secs: u64) -> anyhow::Result<()> {
    anyhow::ensure!(
        host_kvm_available("x86_64_generic_vtx"),
        "Intel storage qualification requires nested VMX/KVM"
    );
    let root = repo_root()?;
    crate::cmd_fetch_guest::build_x86_initramfs()?;
    let evidence = tempfile::Builder::new()
        .prefix("x86-storage-")
        .tempdir_in(root.join("build/tmp"))?
        .keep();
    let disk = evidence.join("disk.img");
    let mut expected = vec![0u8; 32 * 1024 * 1024];
    let boot = b"agentos-host-block-qualification-v1\n";
    let guest = b"agentos-guest-block-qualification-v1\n";
    expected[..boot.len()].copy_from_slice(boot);
    expected[4096..4096 + guest.len()].copy_from_slice(guest);
    let mut file = std::fs::OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&disk)?;
    file.write_all(&expected)?;
    file.sync_all()?;
    drop(file);
    println!(
        "[x86-storage] Retaining disk and boot artifacts in {}",
        evidence.display()
    );
    expected[8192..12288].fill(0x5a);
    let mut phases = Vec::new();
    for phase in ["write", "verify"] {
        let initrd = root.join(format!("build/x86-userspace/initrd-{phase}.bin"));
        let initrd_sha = format!("{:x}", Sha256::digest(std::fs::read(&initrd)?));
        let mut command = std::process::Command::new(std::env::current_exe()?);
        command
            .current_dir(&root)
            .args([
                "qemu-test",
                "--board",
                "x86_64_generic_vtx",
                "--guest-os",
                "none",
                "--assert-vmx-exit",
                "--assert-firmware-reset",
                "--assert-x86-userspace",
                "--timeout-secs",
                &timeout_secs.to_string(),
                "--x86-block-image",
            ])
            .arg(&disk)
            .env("X86_BOOT_INITRD", &initrd)
            .env("X86_BOOT_INITRD_SHA256", &initrd_sha);
        if phase == "write" {
            command.arg("--x86-block-write");
        }
        println!("[x86-storage] Starting {phase} cold boot; initrd SHA256={initrd_sha}");
        let status = command.status()?;
        anyhow::ensure!(
            status.success(),
            "{phase} cold boot failed; retained {}",
            evidence.display()
        );
        let actual = std::fs::read(&disk)?;
        anyhow::ensure!(
            actual == expected,
            "{phase} disk contents differ from the exact expected image"
        );
        let directory = evidence.join(phase);
        std::fs::create_dir(&directory)?;
        let mut artifacts = serde_json::Map::new();
        for name in ["root_task.elf", "agentos.img"] {
            let source = root.join("build/x86_64_generic_vtx").join(name);
            let destination = directory.join(name);
            std::fs::copy(&source, &destination)?;
            artifacts.insert(name.into(), serde_json::json!({
                "path": destination, "sha256": format!("{:x}", Sha256::digest(std::fs::read(source)?))
            }));
        }
        phases.push(
            serde_json::json!({"phase":phase, "result":"PASS", "initrd_sha256":initrd_sha,
            "disk_sha256":format!("{:x}", Sha256::digest(&actual)), "artifacts":artifacts}),
        );
        std::fs::write(
            evidence.join("receipt.json"),
            serde_json::to_vec_pretty(&serde_json::json!({
                "disk":disk, "phases":phases,
                "scope":"Single guest, two complete platform cold boots; no concurrent isolation or guest lifecycle reset claim"
            }))?,
        )?;
    }
    println!("PASS: Intel guest write and fsync survived a fresh platform cold boot; entire disk verified");
    Ok(())
}

fn seeded_boot_guard(
    directory: &Path,
    plan: &str,
    image: &Path,
    second: bool,
) -> anyhow::Result<()> {
    let receipt = directory.join("seeded-profile.txt");
    let retained_image = directory.join("seeded-agentos.img");
    if second {
        anyhow::ensure!(
            std::fs::read_to_string(&receipt)? == plan,
            "seeded profile changed between cold boots"
        );
        crate::persistent_media::require_same_image(&retained_image, image)?;
    } else {
        let mut record = std::fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(receipt)?;
        record.write_all(plan.as_bytes())?;
        record.sync_all()?;
        let mut output = std::fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(retained_image)?;
        std::io::copy(&mut std::fs::File::open(image)?, &mut output)?;
        output.sync_all()?;
    }
    Ok(())
}

fn run_seeded_cold_boots(args: &TestArgs) -> anyhow::Result<()> {
    anyhow::ensure!(
        args.seed_profile && args.seeded_directory.is_none(),
        "two seeded cold boots require a fresh automatic seed"
    );
    let parent = repo_root()?.join("build/evidence");
    std::fs::create_dir_all(&parent)?;
    let directory = tempfile::Builder::new()
        .prefix("seeded-cold-boots-")
        .tempdir_in(parent)?
        .keep();
    println!(
        "[xtask:test] Seeded cold-boot evidence: {}",
        directory.display()
    );
    let receipt = directory.join("cold-boots.json");
    let mut status = serde_json::json!({"schema":"agentos.seeded_cold_boots.v1",
        "profile":args.guest_os, "status":"running", "first_boot":false, "second_boot":false,
        "scope":"guest-written and synced file across two authenticated QEMU cold boots with original host identity; not orderly shutdown, concurrent isolation or guest-slot recreation"});
    std::fs::write(&receipt, serde_json::to_vec_pretty(&status)?)?;
    let mut round = args.clone();
    round.assert_seeded_cold_boots = false;
    round.seeded_directory = Some(directory.clone());
    round.seeded_witness = Some(
        directory
            .file_name()
            .unwrap()
            .to_string_lossy()
            .into_owned(),
    );
    status["witness"] = serde_json::json!(round.seeded_witness);
    for second in [false, true] {
        if second {
            round.seed_profile = false;
            round.no_build = true;
            round.seeded_ssh_key = Some(directory.join("identity"));
            round.seeded_ssh_known_hosts = Some(directory.join("first-known_hosts"));
            round.seeded_source = Some(directory.join("seeded.raw"));
        }
        if let Err(error) = run(&round) {
            status["status"] = serde_json::json!("failed");
            status["error"] = serde_json::json!(format!("{error:#}"));
            std::fs::write(&receipt, serde_json::to_vec_pretty(&status)?)?;
            return Err(error);
        }
        status[if second { "second_boot" } else { "first_boot" }] = serde_json::json!(true);
        std::fs::write(&receipt, serde_json::to_vec_pretty(&status)?)?;
    }
    status["status"] = serde_json::json!("passed");
    std::fs::write(receipt, serde_json::to_vec_pretty(&status)?)?;
    println!("PASS: two seeded cold boots retained the guest-written synced file and original SSH host identity");
    Ok(())
}

pub fn run(args: &TestArgs) -> anyhow::Result<()> {
    anyhow::ensure!(
        !args.retain_failed_guest || (args.keep_running && std::io::stdin().is_terminal()),
        "--retain-failed-guest requires --keep-running and an interactive stdin"
    );
    if args.assert_seeded_cold_boots {
        return run_seeded_cold_boots(args);
    }
    let mut effective_args = args.clone();
    let args = &mut effective_args;
    anyhow::ensure!(
        !args.seed_profile
            || (args.board == "qemu_virt_aarch64"
                && args.ssh_port != 0
                && !args.no_build
                && args.seeded_ssh_key.is_none()
                && args.seeded_ssh_known_hosts.is_none()
                && !args.assert_live
                && !args.assert_desktop
                && !args.assert_persistent_boots),
        "automatic seed requires a fresh ARM profile boot and nonzero SSH port"
    );
    anyhow::ensure!(
        args.seeded_ssh_key.is_none()
            || (args.board == "qemu_virt_aarch64"
                && args.ssh_port != 0
                && args
                    .seeded_ssh_key
                    .as_ref()
                    .is_some_and(|path| path.is_file())),
        "seeded SSH proof requires an ARM profile, private-key file and nonzero --ssh-port"
    );
    anyhow::ensure!(
        args.x86_ssh_key.is_none()
            || (args.ssh_port != 0 && args.x86_ssh_key.as_ref().is_some_and(|p| p.is_file())),
        "Intel SSH proof requires a private-key file and nonzero --ssh-port"
    );
    anyhow::ensure!(
        args.x86_smp_probe.is_none()
            || (args.assert_x86_cc
                && args.x86_ssh_key.is_some()
                && args.x86_smp_probe.as_ref().is_some_and(|p| p.is_file())),
        "Intel SMP probe requires managed CC, pinned SSH and a payload file"
    );
    anyhow::ensure!(
        args.x86_block_image.is_none() || args.board == "x86_64_generic_vtx",
        "qualification block image requires the Intel VMX board"
    );
    if let Some(secondary) = &args.x86_secondary_block_image {
        anyhow::ensure!(
            args.board == "x86_64_generic_vtx" && args.assert_firmware_reset && !args.no_build,
            "second Intel disk requires a freshly built firmware composition"
        );
        validate_x86_distinct_media(
            args.x86_block_image
                .as_deref()
                .context("second disk requires primary media")?,
            secondary,
        )?;
    }
    if args.assert_persistent_boots {
        return run_persistent_boots(args);
    }
    anyhow::ensure!(
        args.guest_gic_failure_probe.is_none()
            || (args.board == "qemu_virt_aarch64"
                && args.guest_os == "none"
                && !args.no_build
                && !args.keep_running),
        "guest GIC failure qualification requires a fresh ARM boot-only image"
    );
    anyhow::ensure!(
        !(args.assert_inspect
            || args.inspect_write_probe
            || args.assert_operator_session
            || args.operator_isolation_probe.is_some()
            || args.assert_log_rings
            || args.log_isolation_probe.is_some())
            || (args.board == "qemu_virt_aarch64" && args.guest_os == "none"),
        "inspect qualification requires AArch64 with guest-os none"
    );
    anyhow::ensure!(
        !args.assert_native_guest
            || (args.board == "qemu_virt_aarch64"
                && args.guest_os == "ubuntu-live"
                && args.assert_live
                && !args.no_build
                && !args.assert_desktop),
        "native/guest qualification requires a fresh Ubuntu live userspace image"
    );
    anyhow::ensure!(
        !(args.assert_native_rust || args.assert_framebuffer)
            || (args.board == "qemu_virt_aarch64" && args.guest_os == "none" && !args.no_build),
        "native Rust/framebuffer qualification requires a fresh qemu_virt_aarch64 GUEST_OS=none image"
    );
    anyhow::ensure!(
        !args.assert_vmx_exit
            || (args.board == "x86_64_generic_vtx"
                && args.guest_os == "none"
                && !args.no_build
                && !args.keep_running),
        "--assert-vmx-exit requires a fresh x86_64_generic_vtx GUEST_OS=none image"
    );
    anyhow::ensure!(
        !(args.assert_guest_ram_recycle || args.assert_guest_block_drain)
            || (args.board == "qemu_virt_aarch64"
                && args.guest_os == "buildroot"
                && !args.no_build
                && !args.keep_running),
        "guest RAM recycle/block drain requires a fresh AArch64 buildroot image"
    );
    anyhow::ensure!(
        !args.assert_console_backpressure
            || (args.board == "qemu_virt_aarch64"
                && args.guest_os == "ubuntu"
                && !args.assert_live
                && !args.assert_desktop
                && !args.no_build
                && args.serial_isolation_probe.is_none()
                && args.network_isolation_probe.is_none()
                && args.block_isolation_probe.is_none()
                && args.virtualizer_authority_probe.is_none()),
        "console backpressure requires a freshly built Ubuntu deterministic probe image"
    );
    anyhow::ensure!(
        !args.assert_guest_teardown
            || (args.board == "qemu_virt_aarch64"
                && !args.no_build
                && !args.keep_running
                && (args.assert_emulated_console
                    || args.seed_profile
                    || args.seeded_ssh_key.is_some())),
        "guest teardown requires a fresh ARM console proof or authenticated seeded profile"
    );
    let repo_root = repo_root()?;
    anyhow::ensure!(
        !args.assert_managed_guest
            || (args.board == "qemu_virt_aarch64"
                && args.guest_os == "ubuntu"
                && !args.no_build
                && !args.keep_running
                && !args.assert_guest_teardown
                && !args.seed_profile
                && args.seeded_ssh_key.is_none()
                && !args.assert_console_backpressure
                && !args.assert_guest_display
                && !args.assert_live
                && !args.assert_desktop),
        "managed guest qualification requires a fresh standalone ARM Ubuntu console proof"
    );
    let initial_agentos_revision = agentos_revision(&repo_root)?;
    let timing_source_tree_clean = agentos_worktree_clean(&repo_root)?;
    let profile_root = repo_root.join("guest-profiles");
    anyhow::ensure!(
        args.scenario.is_none() || args.guest_os == "both",
        "a test scenario requires --guest-os both"
    );
    let mut scenario_plan = if args.guest_os == "both" {
        Some(guest_scenario::resolve_alias(
            &repo_root.join("guest-scenarios"),
            &profile_root,
            args.scenario.as_deref().unwrap_or(&args.guest_os),
        )?)
    } else {
        None
    };
    let mut profile_plan = if matches!(args.guest_os.as_str(), "none" | "both") {
        None
    } else {
        let path = cmd_guest_profile::resolve_alias(&profile_root, &args.guest_os)?;
        Some(cmd_guest_profile::host_profile_plan(&profile_root, &path)?)
    };
    anyhow::ensure!(
        !args.assert_scenario_recreation
            || (args.board == "qemu_virt_aarch64" && args.guest_os == "both"),
        "scenario recreation requires the ARM dual-guest scenario"
    );
    if let Some(profile) = &mut profile_plan {
        apply_profile_ssh_port(profile, args.ssh_port);
    }
    anyhow::ensure!(
        !args.seed_profile || profile_plan.as_ref().is_some_and(|p| p.seed.is_some()),
        "automatic seed requires a host.seed profile contract"
    );
    anyhow::ensure!(
        !args.assert_seeded_recreation || args.board == "qemu_virt_aarch64",
        "seeded recreation requires ARM"
    );
    anyhow::ensure!(
        !profile_plan
            .as_ref()
            .is_some_and(|p| p.test.iter().any(|s| s.action == "assert-ssh-output"))
            || args.seed_profile
            || args.seeded_ssh_key.is_some(),
        "profile SSH output assertions require seeded authentication"
    );
    anyhow::ensure!(
        !args.assert_guest_display || (args.board == "qemu_virt_aarch64"
            && (args.assert_live || args.seed_profile || args.seeded_ssh_key.is_some())
            && !args.no_build && profile_plan.as_ref().is_some_and(|p|
                p.devices.iter().any(|d| d == "gpu")
                && p.test.iter().any(|s| s.action == "assert-frame-pixels"))),
        "guest display qualification requires a fresh AArch64 graphics profile with pixel assertions"
    );
    if let Some(profile) = &profile_plan {
        println!(
            "[xtask:test] resolved alias {:?} to {} ({}, architecture={}, control_type={}, guest_id={}, provision_steps={}, test_steps={})",
            args.guest_os,
            profile.id,
            profile.path.display(),
            profile.architecture,
            profile.control_type,
            profile.guest_id,
            profile.provision.len(),
            profile.test.len()
        );
    }
    if let Some(mode) = args
        .block_isolation_probe
        .or(args.network_isolation_probe)
        .or(args.serial_isolation_probe)
        .or(args
            .virtualizer_authority_probe
            .map(|slot| if slot == 1 { 1 } else { 5 }))
    {
        anyhow::ensure!(
            args.board == "qemu_virt_aarch64"
                && args.guest_os == if mode <= 4 { "buildroot" } else { "freebsd" }
                && !args.no_build
                && !args.assert_emulated_net
                && !args.assert_emulated_blk
                && !args.assert_emulated_console
                && !args.assert_agentos_virtio
                && !args.assert_live
                && !args.assert_desktop,
            "block isolation probe requires a fresh AArch64 image: Buildroot for primary, FreeBSD for secondary"
        );
    }
    let large_guest = profile_plan
        .as_ref()
        .is_some_and(|profile| profile.media_initrd_path.is_some())
        || args.assert_live
        || args.assert_desktop
        || args.guest_os == "both";
    let virtio_assertion = requested_virtio_assertion(args, profile_plan.as_ref());
    if let (Some(profile), Some(assertion)) = (&profile_plan, &virtio_assertion) {
        anyhow::ensure!(
            assertion
                .devices
                .iter()
                .all(|device| profile.devices.contains(device)),
            "profile {} test requests a VirtIO device absent from target.devices",
            profile.id
        );
    }

    anyhow::ensure!(
        !args.keep_running
            || args.guest_os == "both"
            || args.assert_desktop
            || args.assert_live
            || args.seed_profile
            || args.seeded_ssh_key.is_some(),
        "--keep-running requires a dual guest, desktop or authenticated live profile"
    );
    if args.assert_emulated_net
        || args.assert_emulated_blk
        || args.assert_emulated_console
        || args.assert_agentos_virtio
        || args.assert_live
        || args.assert_desktop
    {
        anyhow::ensure!(
            args.board == "qemu_virt_aarch64",
            "emulated VirtIO assertions require --board qemu_virt_aarch64"
        );
        anyhow::ensure!(
            args.guest_os != "none",
            "emulated VirtIO assertions need a real guest; GUEST_OS=none is a stub VMM"
        );
    }
    if args.assert_emulated_console || args.assert_agentos_virtio {
        anyhow::ensure!(
            profile_plan.is_some(),
            "VirtIO console assertions require one runtime guest profile"
        );
    }
    if args.assert_live || args.assert_desktop {
        anyhow::ensure!(
            profile_plan.is_some(),
            "full-userspace assertions require one runtime guest profile"
        );
    }
    if args.assert_desktop {
        anyhow::ensure!(
            profile_plan
                .as_ref()
                .is_some_and(|profile| profile.desktop.is_some()),
            "desktop assertion requires a profile with host.desktop policy"
        );
    }

    if !args.no_build {
        println!(
            "[xtask:test] Building BOARD={} selection={}...",
            args.board, args.guest_os
        );
        let mut make_args = vec![
            String::from("build"),
            format!("BOARD={}", args.board),
            String::from("GUEST_OS=none"),
        ];
        if args.board == "x86_64_generic" {
            make_args.push(String::from("TARGET_ARCH=x86_64"));
            make_args.push(String::from("BOARD_NAME=qemu-x86_64"));
        } else if args.board == "x86_64_generic_vtx" {
            make_args.push(String::from("TARGET_ARCH=x86_64"));
            make_args.push(String::from("BOARD_NAME=qemu-x86_64-vtx"));
        }
        if scenario_plan.is_some() {
            make_args.push(format!(
                "GUEST_SCENARIO={}",
                args.scenario.as_deref().unwrap_or(&args.guest_os)
            ));
        } else if let Some(profile) = &profile_plan {
            make_args.push(format!("GUEST_PROFILE={}", profile.path.display()));
        }
        if let Some(mode) = args.block_isolation_probe {
            make_args.push(format!("BLK_ISOLATION_PROBE={mode}"));
        }
        if let Some(mode) = args.guest_gic_failure_probe {
            make_args.push(format!("GUEST_GIC_FAILURE_PROBE={mode}"));
        }
        if args.inspect_write_probe {
            make_args.push(String::from("INSPECT_WRITE_PROBE=1"));
        }
        if args.assert_operator_session || args.operator_isolation_probe.is_some() {
            make_args.push(String::from("OPERATOR_TEST=1"));
        }
        if args.assert_log_rings || args.log_isolation_probe.is_some() {
            make_args.push(String::from("LOG_RING_TEST=1"));
        }
        if let Some(mode) = args.log_isolation_probe {
            make_args.push(format!("LOG_ISOLATION_PROBE={mode}"));
        }
        if let Some(mode) = args.operator_isolation_probe {
            make_args.push(format!("OPERATOR_ISOLATION_PROBE={mode}"));
        }
        if args.assert_native_rust || args.assert_native_guest {
            make_args.push(String::from("NATIVE_RUST_TEST=1"));
        }
        if args.assert_framebuffer {
            make_args.push(String::from("FRAMEBUFFER_TEST=1"));
        }
        if args.assert_display || args.assert_guest_display {
            make_args.push(String::from("DISPLAY_RAMFB=1"));
        }
        make_args.extend(profile_device_build_args(
            profile_plan.as_ref(),
            scenario_plan.as_ref(),
        ));
        if args.assert_guest_ram_recycle {
            make_args.push(String::from("GUEST_RAM_RECYCLE_TEST=1"));
        }
        if args.assert_guest_queue_recycle {
            make_args.push(String::from("GUEST_QUEUE_RECYCLE_TEST=1"));
        }
        if args.assert_managed_guest || args.assert_seeded_recreation {
            make_args.push(String::from("GUEST_MANAGED_BOOT=1"));
        }
        if args.assert_guest_block_drain {
            make_args.push(String::from("GUEST_BLOCK_DRAIN_TEST=1"));
        }
        if let Some(mode) = args.framebuffer_isolation_probe {
            make_args.push(format!("FRAMEBUFFER_ISOLATION_PROBE={mode}"));
        }
        if let Some(mode) = args.native_network_isolation_probe {
            make_args.push(format!("NATIVE_NET_ISOLATION_PROBE={mode}"));
        }
        if let Some(mode) = args.network_isolation_probe {
            make_args.push(format!("NET_ISOLATION_PROBE={mode}"));
        }
        if let Some(mode) = args.serial_isolation_probe {
            make_args.push(format!("SERIAL_ISOLATION_PROBE={mode}"));
        }
        if let Some(slot) = args.virtualizer_authority_probe {
            make_args.push(format!("VIRT_AUTHORITY_PROBE={slot}"));
        }
        if args.assert_firmware_modes {
            make_args.push(String::from("X86_FIRMWARE_MODES=1"));
        } else if args.assert_vmx_exit {
            make_args.push(String::from("X86_FIRMWARE_MODES=0"));
        }
        if args.assert_vmx_exit {
            make_args.push(format!(
                "X86_GUEST_FAULT_PROOF={}",
                u8::from(args.assert_guest_faults)
            ));
            make_args.push(format!(
                "X86_USERSPACE_PROOF={}",
                u8::from(args.assert_x86_userspace)
            ));
            make_args.push(format!(
                "X86_LINUX_LOGIN={}",
                u8::from(args.assert_x86_linux_login)
            ));
            make_args.push(format!("X86_CC_PCI={}", u8::from(args.assert_x86_cc)));
            make_args.push(format!(
                "X86_SECONDARY_BLOCK={}",
                u8::from(args.x86_secondary_block_image.is_some())
            ));
            make_args.push(format!(
                "X86_FIRMWARE_RESET={}",
                u8::from(args.assert_firmware_reset)
            ));
        }
        if let Some(path) = &args.x86_boot_profile {
            make_args.extend(cmd_guest_profile::prepare_x86_boot_profile(
                &repo_root, path,
            )?);
        }
        let make_arg_refs = make_args.iter().map(String::as_str).collect::<Vec<_>>();
        run_make(&make_arg_refs, &repo_root).context("profile-driven build step failed")?;
    }

    let seeded_plan = format!("{profile_plan:#?}\n");
    if args.seed_profile {
        let profile = profile_plan.as_mut().unwrap();
        let seed = profile.seed.as_ref().unwrap();
        let parent = repo_root.join("build/evidence");
        std::fs::create_dir_all(&parent)?;
        let directory = if let Some(directory) = &args.seeded_directory {
            directory.clone()
        } else {
            tempfile::Builder::new()
                .prefix("profile-seed-")
                .tempdir_in(parent)?
                .keep()
        };
        println!(
            "[xtask:test] Automatic seed evidence: {}",
            directory.display()
        );
        let key = directory.join("identity");
        let status = std::process::Command::new("ssh-keygen")
            .args(["-q", "-t", "ed25519", "-N", "", "-f"])
            .arg(&key)
            .status()?;
        anyhow::ensure!(
            status.success(),
            "automatic seed ssh-keygen failed: {status}"
        );
        let output = directory.join("seeded.raw");
        crate::cmd_seed_guest::run(&crate::cmd_seed_guest::SeedGuestArgs {
            root_ext4: repo_root.join(&seed.root_ext4),
            public_key: key.with_extension("pub"),
            output: output.clone(),
            guest_address: profile
                .qemu
                .as_ref()
                .context("seed requires QEMU")?
                .ssh
                .as_ref()
                .context("seed requires SSH")?
                .guest_address
                .as_deref()
                .context("seed requires a guest address")?
                .parse()?,
            instance_id: directory
                .file_name()
                .unwrap()
                .to_str()
                .context("seed directory is not UTF-8")?
                .into(),
            disk_raw: Some(repo_root.join(&seed.disk_raw)),
            partition_offset: Some(seed.partition_offset),
        })?;
        let disk = profile
            .qemu
            .as_mut()
            .unwrap()
            .media
            .iter_mut()
            .find(|disk| disk.writable)
            .unwrap();
        disk.path = output.to_str().context("seed path is not UTF-8")?.into();
        args.seeded_ssh_key = Some(key);
        args.seeded_directory = Some(directory);
    }

    if args.seeded_ssh_key.is_some() {
        let directory = if let Some(directory) = &args.seeded_directory {
            std::fs::create_dir_all(directory)?;
            directory.clone()
        } else {
            let parent = repo_root.join("build/evidence");
            std::fs::create_dir_all(&parent)?;
            tempfile::Builder::new()
                .prefix("seeded-boot-")
                .tempdir_in(parent)?
                .keep()
        };
        let profile = profile_plan
            .as_mut()
            .context("seeded SSH requires a runtime profile")?;
        let media = &mut profile
            .qemu
            .as_mut()
            .context("seeded profile has no QEMU plan")?
            .media;
        anyhow::ensure!(
            media.iter().filter(|disk| disk.writable).count() == 1,
            "seeded proof requires exactly one writable disk"
        );
        let disk = media.iter_mut().find(|disk| disk.writable).unwrap();
        anyhow::ensure!(
            disk.override_env
                .iter()
                .all(|key| std::env::var_os(key).is_none()),
            "seeded proof rejects media overrides"
        );
        let source = args
            .seeded_source
            .clone()
            .unwrap_or_else(|| repo_root.join(&disk.path));
        let copy = crate::persistent_media::prepare(
            &source,
            &directory,
            args.seeded_ssh_known_hosts.is_some(),
        )?;
        seeded_boot_guard(
            &directory,
            &seeded_plan,
            &repo_root
                .join("build")
                .join(&args.board)
                .join("agentos.img"),
            args.seeded_ssh_known_hosts.is_some(),
        )?;
        disk.path = copy
            .to_str()
            .context("seeded disk path is not UTF-8")?
            .to_owned();
        disk.override_env.clear();
        disk.managed_persistent = true;
        println!(
            "[xtask:test] Seeded persistent evidence: {}",
            directory.display()
        );
    }

    if let Some(directory) = &args.persistent_directory {
        let profile = profile_plan
            .as_mut()
            .context("persistent proof needs one profile")?;
        let resolved = format!("{profile:#?}\n");
        let plan_receipt = directory.join("profile-plan.txt");
        if args.persistent_second_boot {
            anyhow::ensure!(
                std::fs::read_to_string(&plan_receipt)? == resolved,
                "resolved guest profile changed between cold boots"
            );
        } else {
            std::fs::write(plan_receipt, resolved)?;
        }
        let media = &mut profile
            .qemu
            .as_mut()
            .context("profile has no QEMU plan")?
            .media;
        anyhow::ensure!(
            media.iter().filter(|disk| disk.writable).count() == 1,
            "persistent proof requires exactly one writable disk"
        );
        let disk = media.iter_mut().find(|disk| disk.writable).unwrap();
        anyhow::ensure!(
            disk.override_env
                .iter()
                .all(|key| std::env::var_os(key).is_none()),
            "persistent proof requires the pinned profile media, without overrides"
        );
        let copy = crate::persistent_media::prepare(
            &repo_root.join(&disk.path),
            directory,
            args.persistent_second_boot,
        )?;
        disk.path = copy
            .to_str()
            .context("persistent copy path is not UTF-8")?
            .to_owned();
        disk.override_env.clear();
        disk.managed_persistent = true;
        if !args.persistent_second_boot {
            std::fs::copy(
                repo_root
                    .join("build")
                    .join(&args.board)
                    .join("agentos.img"),
                directory.join("agentos.img"),
            )?;
        } else {
            crate::persistent_media::require_same_image(
                &directory.join("agentos.img"),
                &repo_root
                    .join("build")
                    .join(&args.board)
                    .join("agentos.img"),
            )?;
        }
    }
    let input_helper = if (args.assert_live || args.seeded_ssh_key.is_some())
        && profile_plan
            .as_ref()
            .is_some_and(|p| p.devices.iter().any(|d| d == "input"))
    {
        anyhow::ensure!(
            args.board == "qemu_virt_aarch64",
            "input probe requires AArch64 Linux"
        );
        run_make(&["guest-input-probe"], &repo_root)?;
        run_make(&["-C", "tools/agentctl"], &repo_root)?;
        let path = repo_root.join("build/tmp/guest-input-probe-aarch64");
        let bytes = std::fs::read(&path)?;
        anyhow::ensure!(
            bytes.len() >= 20 && &bytes[..6] == b"\x7fELF\x02\x01" && bytes[18..20] == [183, 0],
            "input probe is not little-endian AArch64 ELF64"
        );
        Some(path)
    } else {
        None
    };
    let tmp_dir = qemu_tmp_dir(&repo_root);
    std::fs::create_dir_all(&tmp_dir)
        .with_context(|| format!("failed to create {}", tmp_dir.display()))?;
    let log_file = tempfile::Builder::new()
        .prefix("agentos-qemu-")
        .suffix(".log")
        .tempfile_in(&tmp_dir)
        .context("failed to create build/tmp QEMU log file")?;
    let (_, log_path) = log_file
        .keep()
        .context("failed to persist build/tmp QEMU log file")?;
    if let Some(directory) = &args.persistent_directory {
        let name = if args.persistent_second_boot {
            "second-log-path.txt"
        } else {
            "first-log-path.txt"
        };
        std::fs::write(directory.join(name), format!("{}\n", log_path.display()))?;
    }
    let mut ssh_key = if let Some(key) = &args.seeded_ssh_key {
        Some(SshTestKey {
            _temporary_dir: None,
            private_key: key.clone(),
            public_key: String::new(),
            known_hosts: Some(log_path.with_extension("known_hosts")),
        })
    } else if scenario_plan.is_some() || args.assert_live || args.assert_desktop {
        Some(generate_ssh_test_key(&repo_root, args.keep_running)?)
    } else {
        None
    };

    if let Some(scenario) = &mut scenario_plan {
        let seeded = scenario
            .guests
            .iter()
            .filter(|guest| guest.profile.seed.is_some())
            .count();
        anyhow::ensure!(
            seeded <= 1,
            "scenario executor supports one seeded guest identity"
        );
        if seeded == 1 {
            let key = ssh_key
                .as_mut()
                .context("scenario seed requires SSH identity")?;
            key.known_hosts = Some(log_path.with_extension("scenario.known_hosts"));
            for guest in &mut scenario.guests {
                if guest.profile.seed.is_some() {
                    prepare_scenario_seed(&repo_root, guest, key)?;
                }
            }
        }
    }

    // Every runtime profile reaches its emulated NIC through net_virt and
    // net_pd. Stimulate RX even for the focused "emulated" assertion;
    // otherwise a quiet guest can negotiate the device correctly and then
    // wait forever without exercising a queue.
    let needs_host_net_stimulus = virtio_assertion
        .as_ref()
        .is_some_and(|assertion| assertion.devices.iter().any(|device| device == "net"));
    let ssh_port = if needs_host_net_stimulus
        || args.x86_ssh_key.is_some()
        || args.seeded_ssh_key.is_some()
        || args.assert_live
        || args.assert_desktop
        || scenario_plan.is_some()
    {
        let configured = effective_ssh_port(args, profile_plan.as_ref(), scenario_plan.as_ref());
        if needs_host_net_stimulus && configured == 0 {
            FOCUSED_NET_STIMULUS_PORT
        } else {
            configured
        }
    } else {
        0
    };
    println!("[xtask:test] Launching QEMU for board={}...", args.board);
    let cc_sock = log_path.with_extension("cc_pd.sock");
    let timing_artifact_digests =
        if (args.assert_live && !args.assert_desktop) || args.seeded_ssh_key.is_some() {
            Some((
                sha256_file(
                    &repo_root
                        .join("build")
                        .join(&args.board)
                        .join("agentos.img"),
                )?,
                guest_bundle_sha256(&repo_root, &args.board)?,
            ))
        } else {
            None
        };
    // Measure the host-observed launch-to-authentication interval. Acquisition,
    // compilation and persistent-disk preparation have already completed.
    let boot_clock = Instant::now();
    let mut qemu = ChildGuard::new(spawn_qemu_with_guest(
        &args.board,
        &repo_root,
        &log_path,
        &cc_sock,
        profile_plan.as_ref(),
        scenario_plan.as_ref(),
        ssh_port,
        large_guest,
        (args.guest_os == "both" || args.assert_desktop) && !args.keep_running,
        false,
        false,
        args.x86_block_image.as_deref(),
        args.x86_block_write,
        args.x86_secondary_block_image
            .as_deref()
            .map(|p| (p, args.x86_secondary_block_write)),
        args.assert_display || args.assert_guest_display,
        args.assert_x86_cc,
    )?);
    if needs_host_net_stimulus
        && !args.assert_live
        && !args.assert_desktop
        && args.seeded_ssh_key.is_none()
    {
        wait_for_all_markers(
            &log_path,
            &["emulated virtio-net: guest DRIVER_OK"],
            Duration::from_secs(args.timeout_secs),
            &mut qemu,
        )
        .context("guest net driver did not become ready for host RX stimulus")?;
        /* The SYN is sufficient evidence and RX stimulus. Holding this
         * pre-sshd connection can consume the first daemon accept slot. */
        drop(connect_host_net_stimulus(ssh_port, &mut qemu));
    }

    let mut timing_receipt = None;
    let mut result = if let Some(mode) = args.guest_gic_failure_probe {
        let probe = match mode {
            1 => "[rt] GIC failure probe: missing frame",
            2 => "[rt] GIC failure probe: page mapping",
            _ => "[rt] GIC failure probe: capability copy",
        };
        let refusal = if mode == 1 {
            "[rt] missing guest GIC vCPU frame; refusing boot"
        } else {
            "[rt] guest GIC vCPU mapping failed; refusing boot"
        };
        wait_for_all_markers(
            &log_path,
            &[probe, refusal],
            Duration::from_secs(args.timeout_secs),
            &mut qemu,
        )
        .and_then(|proof| {
            std::thread::sleep(Duration::from_millis(500));
            let text = std::fs::read_to_string(&log_path)?;
            anyhow::ensure!(
                !text.contains("[rt] VMM guest caps installed")
                    && !text.contains("agentOS boot complete"),
                "root continued guest startup after GIC failure"
            );
            Ok(proof)
        })
    } else if args.log_isolation_probe.is_some() {
        wait_for_all_markers(
            &log_path,
            &["[rt] log isolation: expected client data fault verified"],
            Duration::from_secs(args.timeout_secs),
            &mut qemu,
        )
    } else if args.operator_isolation_probe.is_some() {
        wait_for_all_markers(
            &log_path,
            &["[rt] operator isolation: expected client data fault verified"],
            Duration::from_secs(args.timeout_secs),
            &mut qemu,
        )
    } else if args.inspect_write_probe {
        wait_for_all_markers(
            &log_path,
            &[
                "[cc_pd] inspect: valid boot page read before write probe",
                "[rt] inspect: expected read-only page write fault verified",
            ],
            Duration::from_secs(args.timeout_secs),
            &mut qemu,
        )
    } else if args.native_network_isolation_probe.is_some() {
        wait_for_all_markers(
            &log_path,
            &[
                "[rt] native network isolation: expected client data fault verified",
                "[net_virt] TX accepted by net_pd",
                "[net_virt] RX delivered from net_pd",
            ],
            Duration::from_secs(args.timeout_secs),
            &mut qemu,
        )
    } else if args.assert_framebuffer {
        let mut markers = vec![
            "[framebuffer] PASS: client 0 create/write/flip/status/read/destroy exact pixels",
            "[framebuffer] PASS: client 1 create/write/flip/status/read/destroy exact pixels",
        ];
        if args.framebuffer_isolation_probe.is_some() {
            markers.push("[rt] framebuffer isolation: expected client data fault verified");
        }
        if args.assert_display {
            markers.push("[display] private DMA and scanout banks ready");
            markers.push("[display] first frame configured");
        }
        wait_for_all_markers(
            &log_path,
            &markers,
            Duration::from_secs(args.timeout_secs),
            &mut qemu,
        )
    } else if args.assert_native_rust {
        wait_for_all_markers(
            &log_path,
            &[
                "[native-rust] PASS: IPC version, all 120 MRs, invalid requests, recovery",
                "[native-rust] PASS: alloc Vec, alignment, exhaustion, heap reuse",
                "[native-rust] PASS: async tasks, poll budgets, capacity, cancellation",
                "[native-rust] PASS: isolated network queues, NIC ARP replies, persistent wakeups",
                "[net_virt] TX accepted by net_pd",
                "[net_virt] RX delivered from net_pd",
            ],
            Duration::from_secs(args.timeout_secs),
            &mut qemu,
        )
    } else if args.virtualizer_authority_probe.is_some() {
        wait_for_all_markers(&log_path,
            &["[authority-test] spoofed attachments rejected; assigned net/block clients accepted"],
            Duration::from_secs(args.timeout_secs), &mut qemu)
    } else if args.serial_isolation_probe.is_some() {
        wait_for_all_markers(
            &log_path,
            &["[rt] serial isolation: expected VMM data fault verified"],
            Duration::from_secs(args.timeout_secs),
            &mut qemu,
        )
    } else if args.network_isolation_probe.is_some() {
        wait_for_all_markers(
            &log_path,
            &["[rt] network isolation: expected VMM data fault verified"],
            Duration::from_secs(args.timeout_secs),
            &mut qemu,
        )
    } else if args.block_isolation_probe.is_some() {
        wait_for_all_markers(
            &log_path,
            &["[rt] block isolation: expected VMM data fault verified"],
            Duration::from_secs(args.timeout_secs),
            &mut qemu,
        )
    } else if args.assert_emulated_net {
        let start = Instant::now();
        let timeout = Duration::from_secs(args.timeout_secs);
        let profile = profile_plan
            .as_ref()
            .context("network proof requires a guest profile")?;
        anyhow::ensure!(
            profile.console.probe_line.is_some() && profile.console.probe_marker.is_some(),
            "network proof requires an explicit guest console probe"
        );
        wait_for_guest_console_login_via_cc(
            &cc_sock,
            0,
            &profile.id,
            Some(profile),
            timeout,
            &mut qemu,
            None,
        )?;
        println!(
            "[xtask:test] Waiting for emulated virtio-net guest proof in {}...",
            log_path.display()
        );
        wait_for_emulated_net(
            &log_path,
            timeout.saturating_sub(start.elapsed()),
            &mut qemu,
        )
    } else if args.assert_emulated_blk {
        println!(
            "[xtask:test] Waiting for emulated virtio-blk guest proof in {}...",
            log_path.display()
        );
        wait_for_emulated_blk(&log_path, Duration::from_secs(args.timeout_secs), &mut qemu)
    } else {
        if args.guest_os == "both" {
            println!(
                "[xtask:test] Creating both configured profiles through CC-PD/vm_manager ({})...",
                cc_sock.display()
            );
            wait_for_all_markers(
                &log_path,
                &["[cc_pd] VirtIO serial ready"],
                Duration::from_secs(args.timeout_secs),
                &mut qemu,
            )
            .context("CC-PD did not become ready before dual guest creation")?;
            wait_for_dual_guest_consoles_via_cc(
                &cc_sock,
                scenario_plan.as_ref().unwrap(),
                Duration::from_secs(args.timeout_secs),
                &mut qemu,
                ssh_key.as_ref().context("dual SSH key was not generated")?,
                args.keep_running,
                args.assert_scenario_recreation,
            )
        } else if let Some(key) = &args.seeded_ssh_key {
            wait_for_all_markers(
                &log_path,
                &["[cc_pd] VirtIO serial ready"],
                Duration::from_secs(args.timeout_secs),
                &mut qemu,
            )?;
            if args.assert_seeded_recreation {
                seeded_recreation_via_cc(
                    &repo_root,
                    input_helper.as_deref(),
                    &cc_sock,
                    &log_path,
                    profile_plan.as_ref().context("seeded profile missing")?,
                    key,
                    ssh_port,
                    Duration::from_secs(args.timeout_secs),
                    &mut qemu,
                )
            } else {
                seeded_ssh_via_cc(
                    &cc_sock,
                    &log_path,
                    profile_plan
                        .as_ref()
                        .context("seeded SSH requires a runtime profile")?,
                    key,
                    args.seeded_ssh_known_hosts.as_deref(),
                    args.seeded_witness.as_deref(),
                    ssh_port,
                    Duration::from_secs(args.timeout_secs),
                    &mut qemu,
                    0,
                )
            }
        } else if args.assert_emulated_console
            || profile_plan
                .as_ref()
                .is_some_and(|profile| !profile_console_markers(profile).is_empty())
        {
            let profile = profile_plan.as_ref().unwrap();
            println!(
                "[xtask:test] Waiting for profile {} console evidence via CC-PD API ({})...",
                profile.id,
                cc_sock.display()
            );
            wait_for_all_markers(
                &log_path,
                &["[cc_pd] VirtIO serial ready"],
                Duration::from_secs(args.timeout_secs),
                &mut qemu,
            )
            .context("CC-PD did not become ready before guest console probing")?;
            if args.assert_managed_guest {
                let timeout = Duration::from_secs(args.timeout_secs);
                let mut cc =
                    connect_cc_client(&cc_sock, timeout.min(Duration::from_secs(30)), &mut qemu)?;
                let absent = cc.call(MSG_CC_GUEST_STATUS, 0, 0, 0, &[])?;
                anyhow::ensure!(
                    absent.mr[0] == CC_ERR_BAD_HANDLE,
                    "managed image exposed an automatic boot guest"
                );
                let handle = create_guest_via_cc_wait(
                    &mut cc,
                    profile.control_type as u8,
                    VIBEOS_ARCH_AARCH64,
                    64,
                    &profile.id,
                    timeout,
                    &mut qemu,
                )?;
                anyhow::ensure!(handle != 0, "managed CREATE returned reserved boot handle");
                let proof = wait_for_guest_console_login_on_cc(
                    &cc_sock,
                    &mut cc,
                    handle,
                    args.guest_os.as_str(),
                    Some(profile),
                    timeout,
                    &mut qemu,
                    None,
                )?;
                destroy_guest_via_cc(&mut cc, handle, Some(profile))?;
                for opcode in [
                    MSG_CC_GUEST_STATUS,
                    MSG_CC_RESUME_GUEST,
                    MSG_CC_SUSPEND_GUEST,
                ] {
                    let reply = cc.call(opcode, handle, 0, 0, &[])?;
                    anyhow::ensure!(
                        reply.mr[0] == CC_ERR_BAD_HANDLE,
                        "destroyed managed guest opcode {opcode:#x} returned {}, expected bad handle", reply.mr[0]
                    );
                }
                let recreated = create_guest_via_cc_wait(
                    &mut cc,
                    profile.control_type as u8,
                    VIBEOS_ARCH_AARCH64,
                    64,
                    &profile.id,
                    timeout,
                    &mut qemu,
                )?;
                anyhow::ensure!(
                    recreated != 0 && recreated != handle,
                    "recreation reused the retired public handle"
                );
                let second_proof = wait_for_guest_console_login_on_cc(
                    &cc_sock,
                    &mut cc,
                    recreated,
                    args.guest_os.as_str(),
                    Some(profile),
                    timeout,
                    &mut qemu,
                    None,
                )?;
                for opcode in [
                    MSG_CC_GUEST_STATUS,
                    MSG_CC_RESUME_GUEST,
                    MSG_CC_SUSPEND_GUEST,
                ] {
                    let reply = cc.call(opcode, handle, 0, 0, &[])?;
                    anyhow::ensure!(
                        reply.mr[0] == CC_ERR_BAD_HANDLE,
                        "retired handle became usable after recreation"
                    );
                }
                destroy_guest_via_cc(&mut cc, recreated, Some(profile))?;
                Ok(format!("{proof}; {second_proof}; explicit manager CREATE/BOOT, destroy/recreate, second console proof and stale handle rejection passed"))
            } else {
                wait_for_guest_console_login_via_cc(
                    &cc_sock,
                    0,
                    args.guest_os.as_str(),
                    Some(profile),
                    Duration::from_secs(args.timeout_secs),
                    &mut qemu,
                    args.assert_guest_display.then_some(log_path.as_path()),
                )
            }
        } else if args.assert_vmx_exit {
            if args.assert_firmware_reset {
                wait_for_all_markers(
                    &log_path,
                    &[
                        "[rt] x86 host network PCI discovery verified",
                        "[rt] x86 host network driver resources mapped",
                        "[rt] x86 host block PCI resources verified",
                    ],
                    Duration::from_secs(args.timeout_secs),
                    &mut qemu,
                )?;
            }
            if args.assert_x86_cc {
                x86_cc_linux_probe(
                    &cc_sock,
                    &log_path,
                    Duration::from_secs(args.timeout_secs),
                    args.x86_ssh_key
                        .as_deref()
                        .map(|key| (key, ssh_port, args.x86_ssh_known_hosts.as_deref())),
                    args.x86_smp_probe.as_deref(),
                    &mut qemu,
                )
            } else if args.assert_x86_linux_login {
                x86_linux_login_probe(
                    &cc_sock,
                    &log_path,
                    Duration::from_secs(args.timeout_secs),
                    args.x86_ssh_key
                        .as_deref()
                        .map(|key| (key, ssh_port, args.x86_ssh_known_hosts.as_deref())),
                )
            } else {
                if args.assert_x86_userspace {
                    x86_console_roundtrip(&cc_sock, Duration::from_secs(args.timeout_secs))?;
                }
                wait_for_x86_vtx_proof(
                    &log_path,
                    Duration::from_secs(args.timeout_secs),
                    &mut qemu,
                    args.assert_firmware_modes,
                    args.assert_firmware_reset,
                    args.assert_guest_faults,
                    args.assert_x86_userspace,
                )
                .and_then(|proof| {
                    if args.assert_firmware_reset {
                        let required: &[&str] = if args.assert_x86_userspace {
                            &[
                                "[rt] x86 host block queue read verified",
                                "[rt] x86 Linux guest block read verified",
                                "[rt] x86 Linux guest network packet roundtrip verified",
                                "[rt] x86 terminal teardown and zeroed pool reuse verified",
                                "[rt] x86 lifecycle client suspend resume destroy verified",
                            ]
                        } else {
                            &["[rt] x86 host block queue read verified"]
                        };
                        wait_for_all_markers(
                            &log_path,
                            required,
                            Duration::from_secs(args.timeout_secs),
                            &mut qemu,
                        )?;
                    }
                    Ok(proof)
                })
            }
        } else if args.board == "x86_64_generic" {
            wait_for_x86_reduced_smoke(&log_path, Duration::from_secs(args.timeout_secs))
        } else {
            wait_for_markers(
                &log_path,
                &["agentOS boot complete", "buildroot login:"],
                Duration::from_secs(args.timeout_secs),
            )
        }
    };

    if result.is_ok() && args.x86_secondary_block_image.is_some() {
        result = result.and_then(|previous| {
            wait_for_all_markers(
                &log_path,
                &["[virtio_blk] two PCI media initialized with independent queues"],
                Duration::from_secs(5),
                &mut qemu,
            )?;
            Ok(format!("{previous}; two host PCI block devices initialized (secondary guest I/O not qualified)"))
        });
    }

    if result.is_ok() && args.assert_framebuffer {
        result = verify_native_frame_observer(
            &cc_sock,
            &log_path,
            Duration::from_secs(args.timeout_secs),
            &mut qemu,
        );
    }

    if result.is_ok() && args.assert_display {
        result = verify_native_display(&log_path);
    }

    if result.is_ok() && args.assert_console_backpressure {
        result = verify_console_backpressure(
            &cc_sock,
            &log_path,
            profile_plan.as_ref(),
            Duration::from_secs(args.timeout_secs),
            &mut qemu,
        );
    }

    /*
     * Start the authenticated desktop path before checking host-backed network
     * markers.  Its SSH connection is the RX stimulus.  Connecting to the
     * forwarded port before sshd exists leaves a stale user-net flow that can
     * accept later host sockets without ever completing an SSH banner.
     */
    if result.is_ok()
        && ((args.assert_live && !args.assert_desktop) || args.seeded_ssh_key.is_some())
    {
        let proof = if args.seeded_ssh_key.is_some() {
            // The seeded result already includes authenticated SSH and sync.
            Ok(Instant::now())
        } else {
            let key = ssh_key
                .as_ref()
                .context("live-profile SSH key was not generated")?;
            prove_profile_ssh(
                &cc_sock,
                profile_plan
                    .as_ref()
                    .context("live test requires a resolved guest profile")?,
                key,
                Duration::from_secs(args.timeout_secs),
                &mut qemu,
            )
        };
        match proof {
            Ok(ssh_ready) => {
                // Preserve the v2 readiness timing boundary; functional session
                // acceptance is also required before publishing a passed receipt.
                let elapsed_ms = ssh_ready.duration_since(boot_clock).as_millis();
                let timing_path = log_path.with_extension("boot-timing.json");
                let timing_source_tree_clean = timing_source_tree_clean
                    && agentos_worktree_clean(&repo_root)?
                    && initial_agentos_revision == agentos_revision(&repo_root)?;
                let timing_qemu_config = timing_qemu_config(
                    args,
                    profile_plan
                        .as_ref()
                        .context("live test requires a resolved guest profile")?,
                )?;
                let (agentos_image_sha256, guest_bundle_sha256) = timing_artifact_digests
                    .as_ref()
                    .context("live test lost its pre-launch timing artifact digests")?;
                timing_receipt = Some((
                    timing_path,
                    serde_json::json!({
                        "schema": "agentos.guest_boot_timing.v2",
                        "status": "ssh_authenticated",
                        "qualification_status": "pending",
                        "boundary": "host QEMU launch request to completed authenticated SSH proof",
                        "elapsed_ms": elapsed_ms,
                        "board": args.board,
                        "profile": profile_plan.as_ref().unwrap().id,
                        "agentos_revision": initial_agentos_revision,
                        "source_tree_clean": timing_source_tree_clean,
                        "agentos_image_sha256": agentos_image_sha256,
                        "guest_bundle_sha256": guest_bundle_sha256,
                        "qemu_config_sha256": sha256_bytes(timing_qemu_config.as_bytes()),
                        "host_backed_virtio": args.assert_agentos_virtio,
                        "host_os": std::env::consts::OS,
                        "host_arch": std::env::consts::ARCH,
                        "persistent_second_boot": args.persistent_second_boot || args.seeded_ssh_known_hosts.is_some(),
                        "serial_log": log_path,
                        "excludes": ["artifact acquisition", "build", "persistent media preparation"],
                        "includes": ["host scheduling", "QEMU startup", "agentOS boot", "guest boot", "console provisioning", "SSH authentication"],
                    }),
                ));
                result = Ok(format!(
                    "{}; {}",
                    result.as_deref().unwrap_or("guest profile ready"),
                    if args.seeded_ssh_key.is_some() {
                        "profile seeded SSH proof verified"
                    } else {
                        "profile functional SSH session verified"
                    }
                ));
            }
            Err(error) => result = Err(error),
        }
    }

    if result.is_ok() && args.seeded_ssh_key.is_some() && !args.assert_seeded_recreation {
        result = prove_seeded_profile_steps(
            &cc_sock,
            &log_path,
            profile_plan.as_ref().context("seeded profile missing")?,
            ssh_key.as_ref().context("seeded SSH identity missing")?,
            args.assert_guest_display,
            &mut qemu,
            0,
        )
        .map(|()| result.as_ref().unwrap().clone());
    }
    if result.is_ok()
        && args.seeded_ssh_key.is_some()
        && !args.assert_seeded_recreation
        && virtio_assertion
            .as_ref()
            .is_some_and(|proof| proof.bidirectional_console)
    {
        // Submit an empty login line after timing authentication. The normal
        // VirtIO proof below requires actual queue delivery in both directions.
        let mut cc = connect_cc_client(&cc_sock, Duration::from_secs(30), &mut qemu)?;
        cc_send_raw_byte(&mut cc, 0, b'\r')?;
    }
    if result.is_ok() && !args.assert_seeded_recreation {
        if let Some(helper) = &input_helper {
            result = prove_profile_input(
                &repo_root,
                &cc_sock,
                &log_path,
                helper,
                profile_plan.as_ref().context("input profile missing")?,
                ssh_key.as_ref().context("input SSH key missing")?,
                &mut qemu,
                0,
            );
        }
    }

    // Live-profile provisioning above establishes the guest's network and
    // shell before interleaving fresh native exchanges with guest traffic.
    if result.is_ok() && args.assert_native_guest {
        result = verify_native_guest_network(
            &cc_sock,
            profile_plan.as_ref(),
            Duration::from_secs(args.timeout_secs),
            &mut qemu,
        );
    }

    if result.is_ok() && (args.assert_log_rings || args.log_isolation_probe.is_some()) {
        result = wait_for_all_markers(
            &log_path,
            &["[operator_session]\x1b[0m native log proof: first fragment + second\n"],
            Duration::from_secs(args.timeout_secs),
            &mut qemu,
        );
    }
    if result.is_ok() && args.assert_inspect {
        result = verify_inspect(&cc_sock, &repo_root);
    }
    if result.is_ok() && args.assert_operator_session {
        result =
            verify_operator_session(&cc_sock, &repo_root, Duration::from_secs(args.timeout_secs));
    }

    // Every AArch64 image includes log_drain and the serial driver. Require
    // actual driver-backed output even on release kernels with debug printing
    // disabled; PD load alone missed malformed serial requests in log_drain.
    if result.is_ok() && args.board == "qemu_virt_aarch64" && args.guest_gic_failure_probe.is_none()
    {
        if let Err(error) = wait_for_all_markers(
            &log_path,
            &["[log_drain] ready"],
            Duration::from_secs(10),
            &mut qemu,
        ) {
            result = Err(error.context("log_drain did not emit through serial_pd"));
        }
    }

    if result.is_ok() && args.assert_guest_ram_recycle {
        result = wait_for_all_markers(
            &log_path,
            &[
                "guest RAM recycle: PASS two full overwrite/revoke/rebuild/zero cycles",
                "guest image recycle: embedded artifacts restored byte for byte twice",
            ],
            Duration::from_secs(10),
            &mut qemu,
        );
    }

    if result.is_ok() && args.assert_guest_block_drain {
        result = wait_for_all_markers(
            &log_path,
            &[
                "guest block drain: pending response before admission stop",
                "guest block drain: PASS accepted requests complete and queues empty",
                "guest block drain: queue detach acknowledged",
            ],
            Duration::from_secs(10),
            &mut qemu,
        );
    }

    if result.is_ok() && args.assert_guest_teardown {
        let input_detach_required = profile_plan
            .as_ref()
            .is_some_and(|p| p.devices.iter().any(|d| d == "input"));
        let graphics_detach_required = profile_plan
            .as_ref()
            .is_some_and(|p| p.devices.iter().any(|d| d == "gpu"));
        result = verify_guest_teardown(
            &cc_sock,
            &log_path,
            &mut qemu,
            input_detach_required,
            graphics_detach_required,
        )
        .map(|proof| format!("{}; {proof}", result.as_deref().unwrap()));
    }
    if result.is_ok() && args.assert_guest_queue_recycle {
        let mut markers = vec![
            "guest queue recycle: zero pages and stale caps verified",
            "guest paging recycle: fresh VSpaces and page tables verified",
            "guest execution recycle: fresh stopped objects and registers verified",
        ];
        if profile_plan
            .as_ref()
            .is_some_and(|p| p.devices.iter().any(|d| d == "gpu"))
        {
            markers.push("guest graphics recycle: queue and arena pools verified");
        }
        result = wait_for_all_markers(&log_path, &markers, Duration::from_secs(10), &mut qemu);
    }

    let mut desktop_evidence = None;
    let mut desktop_tunnel = None;
    if result.is_ok() && args.assert_desktop {
        let key = ssh_key
            .as_ref()
            .context("desktop SSH key was not generated")?;
        match prove_profile_desktop(
            &cc_sock,
            profile_plan
                .as_ref()
                .context("desktop test requires a resolved guest profile")?,
            key,
            Duration::from_secs(args.timeout_secs),
            &mut qemu,
        ) {
            Ok((evidence, tunnel)) => {
                desktop_evidence = Some(evidence);
                desktop_tunnel = Some(tunnel);
            }
            Err(error) => result = Err(error),
        }
    }

    if result.is_ok() {
        if let Some(assertion) = &virtio_assertion {
            let required = virtio_markers(assertion);
            let proof =
                wait_for_all_markers(&log_path, &required, Duration::from_secs(10), &mut qemu);
            let forbidden_loopback = assertion.host_backed
                && assertion.devices.iter().any(|device| device == "net")
                && std::fs::read_to_string(&log_path)
                    .map(|log| log.contains("frame(s) TX->RX"))
                    .unwrap_or(false);
            result = match (result, proof) {
                (Ok(_), Ok(_)) if forbidden_loopback => anyhow::bail!(
                    "host-backed network proof used the forbidden VMM-local TX->RX loopback"
                ),
                (Ok(evidence), Ok(_)) => Ok(format!(
                    "{evidence}; profile VirtIO {:?} scope={} satisfied",
                    assertion.devices,
                    if assertion.host_backed {
                        "host-backed"
                    } else {
                        "emulated"
                    }
                )),
                (_, Err(err)) => Err(err.context("profile VirtIO proof was incomplete")),
                (Err(err), _) => Err(err),
            };
        }
    }

    if result.is_ok() {
        if let Some(evidence) = desktop_evidence {
            result = Ok(format!(
                "{}; profile desktop RFB {}x{} bytes={} fnv1a64={:016x} name={:?}",
                result.as_deref().unwrap_or("guest profile ready"),
                evidence.width,
                evidence.height,
                evidence.bytes_received,
                evidence.fnv1a64,
                evidence.desktop_name,
            ));
        }
    }

    if result.is_ok() && args.persistent_directory.is_some() {
        let profile = profile_plan
            .as_ref()
            .context("persistent proof lost its profile")?;
        let ssh = profile
            .qemu
            .as_ref()
            .and_then(|plan| plan.ssh.as_ref())
            .context("persistent proof requires profile SSH")?;
        let key = ssh_key
            .as_ref()
            .context("persistent proof requires authenticated SSH")?;
        let script = persistence_script(&args.persistent_token, args.persistent_second_boot)?;
        result = run_ssh_script(
            &key.private_key,
            ssh.host_port,
            &ssh.account,
            Duration::from_secs(120),
            &script,
        )
        .and_then(|output| {
            anyhow::ensure!(
                output.trim() == args.persistent_token,
                "persistent disk witness did not match"
            );
            Ok(format!(
                "{}; persistent disk witness {}",
                result.as_deref().unwrap_or("profile ready"),
                if args.persistent_second_boot {
                    "read after cold boot"
                } else {
                    "written and synced"
                }
            ))
        });
    }
    if args.retain_failed_guest {
        if let Err(failure) = &result {
            eprintln!("[xtask:test] Qualification FAILED; retaining for diagnosis: {failure:#}");
            if let Err(error) = wait_for_manual_cc_client(&cc_sock, &mut qemu, false) {
                eprintln!("[xtask:test] Failed-guest retention ended: {error:#}");
            }
            // Preserve the qualification error, even if the manual session
            // succeeds or subsequent inspection restores guest responsiveness.
        }
    }
    if args.keep_running && result.is_ok() {
        let key = ssh_key
            .as_ref()
            .context("persistent SSH key was not generated")?;
        result = if args.assert_desktop {
            wait_for_manual_desktop(
                key,
                profile_plan
                    .as_ref()
                    .context("desktop session requires a resolved profile")?,
                &mut qemu,
            )
            .map(|()| String::from("manual profile desktop session completed"))
        } else if let Some(scenario) = scenario_plan.as_ref() {
            wait_for_manual_dual_ssh(key, scenario, &mut qemu)
                .map(|()| String::from("manual dual SSH session completed"))
        } else {
            wait_for_manual_cc_client(&cc_sock, &mut qemu, true).map(|()| {
                String::from("qualified live guest retained for manual CC client session")
            })
        };
    }

    if result.is_ok() {
        if let Some((timing_path, mut timing)) = timing_receipt {
            let source_tree_clean = timing["source_tree_clean"].as_bool().unwrap_or(false)
                && agentos_worktree_clean(&repo_root)?
                && timing["agentos_revision"].as_str() == Some(&agentos_revision(&repo_root)?);
            timing["source_tree_clean"] = serde_json::json!(source_tree_clean);
            timing["qualification_status"] = serde_json::json!("passed");
            std::fs::write(&timing_path, serde_json::to_vec_pretty(&timing)?)?;
            println!(
                "[xtask:test] Authenticated boot timing: {} ms ({})",
                timing["elapsed_ms"],
                timing_path.display()
            );
        }
    }

    if let Some(mut tunnel) = desktop_tunnel {
        let _ = tunnel.kill();
        let _ = tunnel.wait();
    }
    let _ = qemu.kill();
    let _ = qemu.wait();

    if args.seeded_ssh_key.is_some() {
        if let Some(directory) = &args.seeded_directory {
            let phase = if args.seeded_ssh_known_hosts.is_some() {
                "second"
            } else {
                "first"
            };
            for (extension, name) in [
                ("log", "serial.log"),
                ("console.log", "console.log"),
                ("known_hosts", "known_hosts"),
                ("boot-timing.json", "boot-timing.json"),
            ] {
                let source = log_path.with_extension(extension);
                if source.is_file() {
                    std::fs::copy(source, directory.join(format!("{phase}-{name}")))?;
                }
            }
            std::fs::write(
                directory.join(format!("{phase}-result.txt")),
                match &result {
                    Ok(value) => format!("pass: {value}\n"),
                    Err(error) => format!("fail: {error:#}\n"),
                },
            )?;
        }
    }

    if let Some(directory) = &args.persistent_directory {
        let phase = if args.persistent_second_boot {
            "second"
        } else {
            "first"
        };
        std::fs::copy(&log_path, directory.join(format!("{phase}-serial.log")))?;
        let timing_path = log_path.with_extension("boot-timing.json");
        if timing_path.exists() {
            std::fs::copy(
                timing_path,
                directory.join(format!("{phase}-boot-timing.json")),
            )?;
        }
        std::fs::write(
            directory.join(format!("{phase}-result.txt")),
            match &result {
                Ok(value) => format!("pass: {value}\n"),
                Err(error) => format!("fail: {error:#}\n"),
            },
        )?;
    }

    // Print captured serial output
    println!("\n=== Serial output ===");
    if let Ok(mut f) = std::fs::File::open(&log_path) {
        let mut buf = String::new();
        let _ = f.read_to_string(&mut buf);
        print!("{}", buf);
    }
    println!("=====================\n");

    match result {
        Ok(marker) => {
            println!("PASS [board={}]: found marker \"{}\"", args.board, marker);
            Ok(())
        }
        Err(e) => {
            println!("FAIL [board={}]: {:#}", args.board, e);
            anyhow::bail!("test failed for board {}: {:#}", args.board, e);
        }
    }
}

fn profile_device_build_args(
    profile: Option<&HostProfilePlan>,
    scenario: Option<&HostScenarioPlan>,
) -> Vec<String> {
    let needs = |device: &str| {
        profile.is_some_and(|p| p.devices.iter().any(|d| d == device))
            || scenario.is_some_and(|s| {
                s.guests
                    .iter()
                    .any(|g| g.profile.devices.iter().any(|d| d == device))
            })
    };
    // Explicit empty values prevent inherited environment settings from silently
    // adding devices to a profile which does not request them.
    vec![
        format!("GUEST_GRAPHICS={}", if needs("gpu") { "1" } else { "" }),
        format!("GUEST_INPUT={}", if needs("input") { "1" } else { "" }),
    ]
}

pub fn launch(args: &QemuLaunchArgs) -> anyhow::Result<()> {
    anyhow::ensure!(
        !args.x86_cc || args.board == "x86_64_generic_vtx",
        "binary Intel CC launch requires --board x86_64_generic_vtx"
    );
    let repo_root = repo_root()?;
    let profile_root = repo_root.join("guest-profiles");
    let profile_plan = args
        .profile
        .as_ref()
        .map(|path| cmd_guest_profile::host_profile_plan(&profile_root, path))
        .transpose()?;
    let scenario_plan = args
        .scenario
        .as_ref()
        .map(|alias| {
            guest_scenario::resolve_alias(&repo_root.join("guest-scenarios"), &profile_root, alias)
        })
        .transpose()?;

    let selection = if let Some(path) = &args.x86_boot_profile {
        format!("managed Intel profile {}", path.display())
    } else if let Some(profile) = &profile_plan {
        format!("profile {}", profile.id)
    } else if let Some(scenario) = &scenario_plan {
        format!("scenario {}", scenario.id)
    } else {
        String::from("no guest profile")
    };
    println!(
        "[xtask:launch] Building BOARD={} from {}...",
        args.board, selection
    );
    let mut make_args = vec![
        String::from("build"),
        format!("BOARD={}", args.board),
        String::from("GUEST_OS=none"),
    ];
    if args.board == "x86_64_generic" {
        make_args.push(String::from("TARGET_ARCH=x86_64"));
        make_args.push(String::from("BOARD_NAME=qemu-x86_64"));
    } else if args.board == "x86_64_generic_vtx" {
        make_args.push(String::from("TARGET_ARCH=x86_64"));
        make_args.push(String::from("BOARD_NAME=qemu-x86_64-vtx"));
    }
    if let Some(profile) = &profile_plan {
        make_args.push(format!("GUEST_PROFILE={}", profile.path.display()));
    } else if let Some(alias) = &args.scenario {
        make_args.push(format!("GUEST_SCENARIO={alias}"));
    }
    make_args.extend(profile_device_build_args(
        profile_plan.as_ref(),
        scenario_plan.as_ref(),
    ));
    if args.x86_cc {
        make_args.extend([
            String::from("X86_CC_PCI=1"),
            String::from("X86_FIRMWARE_RESET=1"),
        ]);
    }
    if let Some(path) = &args.x86_boot_profile {
        make_args.extend(cmd_guest_profile::prepare_x86_boot_profile(
            &repo_root, path,
        )?);
        make_args.push(String::from("X86_LINUX_LOGIN=1"));
    }
    let make_arg_refs = make_args.iter().map(String::as_str).collect::<Vec<_>>();
    run_make(&make_arg_refs, &repo_root).context("profile-driven build step failed")?;

    let tmp_dir = qemu_tmp_dir(&repo_root);
    std::fs::create_dir_all(&tmp_dir)
        .with_context(|| format!("failed to create {}", tmp_dir.display()))?;
    let log_path = tmp_dir.join("agentos-run.log");
    let cc_sock = repo_root.join("build/cc_pd.sock");
    let ssh_port = args.ssh_port.unwrap_or_else(|| {
        profile_plan
            .as_ref()
            .and_then(|profile| profile.qemu.as_ref())
            .and_then(|qemu| qemu.ssh.as_ref())
            .map(|ssh| ssh.host_port)
            .unwrap_or(0)
    });
    let large_guest = profile_plan
        .as_ref()
        .is_some_and(|profile| profile.media_initrd_path.is_some())
        || scenario_plan.is_some();

    println!("\nagentOS interactive QEMU launch");
    println!("  board:     {}", args.board);
    println!("  selection: {selection}");
    println!("  CC-PD:     {}", cc_sock.display());
    println!(
        "  accel:     {}",
        if host_kvm_available(&args.board) {
            "KVM"
        } else if args.fast {
            "multi-threaded TCG"
        } else {
            "TCG"
        }
    );
    println!("  exit:      Ctrl-A X\n");

    let mut qemu = spawn_qemu_with_guest(
        &args.board,
        &repo_root,
        &log_path,
        &cc_sock,
        profile_plan.as_ref(),
        scenario_plan.as_ref(),
        ssh_port,
        large_guest,
        false,
        true,
        args.fast,
        args.x86_block_image.as_deref(),
        args.x86_block_write,
        None,
        false,
        args.x86_cc,
    )?;
    let status = qemu.wait().context("failed to wait for QEMU")?;
    anyhow::ensure!(status.success(), "QEMU exited with {status}");
    Ok(())
}

fn wait_for_manual_cc_client(
    socket: &Path,
    qemu: &mut Child,
    qualified: bool,
) -> anyhow::Result<()> {
    ensure_qemu_running(qemu, "entering manual CC client mode")?;
    if qualified {
        println!("\n[xtask:test] Qualified guest retained for native CC clients");
    } else {
        println!(
            "\n[xtask:test] FAILED guest retained for diagnosis; qualification remains failed"
        );
    }
    println!("CC_PD_SOCK={}", socket.display());
    println!("Close the external client, then press Enter here to stop QEMU.");
    let mut line = String::new();
    let bytes = std::io::stdin()
        .read_line(&mut line)
        .context("failed to wait for manual CC client shutdown input")?;
    anyhow::ensure!(
        bytes != 0,
        "manual CC client mode requires an interactive stdin"
    );
    ensure_qemu_running(qemu, "leaving manual CC client mode")
}

fn wait_for_manual_dual_ssh(
    ssh_key: &SshTestKey,
    scenario: &HostScenarioPlan,
    qemu: &mut Child,
) -> anyhow::Result<()> {
    ensure_qemu_running(qemu, "entering manual dual SSH mode")?;
    println!("\nDual guests are running with authenticated SSH:");
    for command in manual_ssh_commands(&ssh_key.private_key, scenario)? {
        println!("  {command}");
    }
    println!("Press Enter here to stop both guests and QEMU.");

    let mut line = String::new();
    let bytes = std::io::stdin()
        .read_line(&mut line)
        .context("failed to wait for manual SSH shutdown input")?;
    anyhow::ensure!(bytes != 0, "manual SSH mode requires an interactive stdin");
    ensure_qemu_running(qemu, "leaving manual dual SSH mode")
}

fn wait_for_manual_desktop(
    ssh_key: &SshTestKey,
    profile: &HostProfilePlan,
    qemu: &mut Child,
) -> anyhow::Result<()> {
    let desktop = profile
        .desktop
        .as_ref()
        .context("profile has no host.desktop plan")?;
    let ssh = profile
        .qemu
        .as_ref()
        .and_then(|qemu| qemu.ssh.as_ref())
        .context("desktop profile has no host.qemu.ssh plan")?;
    ensure_qemu_running(qemu, "entering manual profile desktop mode")?;
    println!("\n{} is running a tunnel-confined VNC desktop:", profile.id);
    println!("  vncviewer 127.0.0.1:{}", desktop.local_port);
    println!(
        "  ssh -i '{}' -p {} -o IdentitiesOnly=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null {}@127.0.0.1",
        ssh_key.private_key.display(),
        ssh.host_port,
        ssh.account,
    );
    println!("Press Enter here to stop the guest, SSH tunnel, and QEMU.");

    let mut line = String::new();
    let bytes = std::io::stdin()
        .read_line(&mut line)
        .context("failed to wait for manual desktop shutdown input")?;
    anyhow::ensure!(
        bytes != 0,
        "manual desktop mode requires an interactive stdin"
    );
    ensure_qemu_running(qemu, "leaving manual profile desktop mode")
}

fn manual_ssh_commands(
    private_key: &Path,
    scenario: &HostScenarioPlan,
) -> anyhow::Result<Vec<String>> {
    scenario
        .guests
        .iter()
        .map(|guest| {
            let (account, _) = profile_ssh_expectation(&guest.profile)?;
            Ok(format!(
                "ssh -i '{}' -p {} -o IdentitiesOnly=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null {}@127.0.0.1",
                private_key.display(),
                guest.ssh_host_port,
                account
            ))
        })
        .collect()
}

// Keep the resolved host plan consistent with QEMU's forwarding port. All
// later SSH probes, provisioning and manual instructions consume this plan.
// Scenario guests retain their explicitly assigned individual ports.
fn apply_profile_ssh_port(profile: &mut HostProfilePlan, port: u16) {
    if port != 0 {
        if let Some(ssh) = profile.qemu.as_mut().and_then(|qemu| qemu.ssh.as_mut()) {
            ssh.host_port = port;
        }
    }
}

fn effective_ssh_port(
    args: &TestArgs,
    profile: Option<&HostProfilePlan>,
    scenario: Option<&HostScenarioPlan>,
) -> u16 {
    if args.ssh_port != 0 {
        return args.ssh_port;
    }
    if let Some(scenario) = scenario {
        return scenario.guests[0].ssh_host_port;
    }
    profile
        .and_then(|value| value.qemu.as_ref())
        .and_then(|qemu| qemu.ssh.as_ref())
        .map(|ssh| ssh.host_port)
        .unwrap_or(0)
}

struct SshTestKey {
    _temporary_dir: Option<tempfile::TempDir>,
    private_key: PathBuf,
    public_key: String,
    known_hosts: Option<PathBuf>,
}

struct ChildGuard {
    child: Child,
}

impl ChildGuard {
    fn new(child: Child) -> Self {
        Self { child }
    }
}

impl Deref for ChildGuard {
    type Target = Child;

    fn deref(&self) -> &Self::Target {
        &self.child
    }
}

impl DerefMut for ChildGuard {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.child
    }
}

impl Drop for ChildGuard {
    fn drop(&mut self) {
        if matches!(self.child.try_wait(), Ok(None)) {
            let _ = self.child.kill();
            let _ = self.child.wait();
        }
    }
}

fn generate_ssh_test_key(repo_root: &Path, persistent: bool) -> anyhow::Result<SshTestKey> {
    let tmp_dir = qemu_tmp_dir(&repo_root);
    std::fs::create_dir_all(&tmp_dir)
        .with_context(|| format!("failed to create {}", tmp_dir.display()))?;
    let (temporary_dir, key_dir) = if persistent {
        let key_dir = tmp_dir.join("dual-ssh");
        std::fs::create_dir_all(&key_dir)
            .with_context(|| format!("failed to create {}", key_dir.display()))?;
        (None, key_dir)
    } else {
        let dir = tempfile::Builder::new()
            .prefix("agentos-dual-ssh-")
            .tempdir_in(&tmp_dir)
            .context("failed to create dual SSH key directory")?;
        let key_dir = dir.path().to_path_buf();
        (Some(dir), key_dir)
    };
    let private_key = key_dir.join("id_ed25519");
    let public_key_path = private_key.with_extension("pub");
    for path in [&private_key, &public_key_path] {
        if path.exists() {
            std::fs::remove_file(path)
                .with_context(|| format!("failed to replace {}", path.display()))?;
        }
    }
    let status = std::process::Command::new("ssh-keygen")
        .args(["-q", "-t", "ed25519", "-N", "", "-f"])
        .arg(&private_key)
        .status()
        .context("failed to run ssh-keygen for dual SSH proof")?;
    anyhow::ensure!(status.success(), "ssh-keygen failed with {status}");
    let public_key = std::fs::read_to_string(public_key_path)
        .context("failed to read generated dual SSH public key")?
        .trim()
        .to_string();
    Ok(SshTestKey {
        _temporary_dir: temporary_dir,
        private_key,
        public_key,
        known_hosts: None,
    })
}

/// Directory for QEMU logs and control sockets. Defaults to `build/tmp`
/// under the repo root; `AGENTOS_TMP_DIR` overrides it. The override exists
/// because QEMU binds Unix sockets here and macOS caps socket paths at 104
/// bytes, which a repo checked out under `.claude/worktrees/<name>/` exceeds.
pub fn qemu_tmp_dir(repo_root: &Path) -> PathBuf {
    std::env::var_os("AGENTOS_TMP_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| repo_root.join("build/tmp"))
}

pub fn run_make(args: &[&str], cwd: &Path) -> anyhow::Result<()> {
    let status = std::process::Command::new("make")
        .args(args)
        .current_dir(cwd)
        .status()?;
    anyhow::ensure!(status.success(), "make {} failed", args.join(" "));
    Ok(())
}

fn repo_root() -> anyhow::Result<std::path::PathBuf> {
    // Walk up from the xtask binary's manifest dir or use CARGO_MANIFEST_DIR
    // At runtime, resolve relative to the current working directory's git root.
    let output = std::process::Command::new("git")
        .args(["rev-parse", "--show-toplevel"])
        .output()
        .context("failed to run git rev-parse")?;
    anyhow::ensure!(output.status.success(), "not in a git repository");
    let root = String::from_utf8(output.stdout)
        .context("git output is not utf-8")?
        .trim()
        .to_string();
    Ok(std::path::PathBuf::from(root))
}

fn agentos_revision(repo_root: &Path) -> anyhow::Result<String> {
    let output = std::process::Command::new("git")
        .args(["rev-parse", "HEAD"])
        .current_dir(repo_root)
        .output()
        .context("failed to read agentOS revision")?;
    anyhow::ensure!(
        output.status.success(),
        "failed to resolve agentOS revision"
    );
    let revision = String::from_utf8(output.stdout)
        .context("agentOS revision is not UTF-8")?
        .trim()
        .to_owned();
    anyhow::ensure!(
        revision.len() == 40 && revision.bytes().all(|byte| byte.is_ascii_hexdigit()),
        "agentOS revision is not a full Git object ID"
    );
    Ok(revision)
}

fn agentos_worktree_clean(repo_root: &Path) -> anyhow::Result<bool> {
    let status = std::process::Command::new("git")
        .args(["status", "--porcelain=v1", "--untracked-files=all"])
        .current_dir(repo_root)
        .output()
        .context("failed to inspect agentOS worktree")?;
    anyhow::ensure!(
        status.status.success(),
        "failed to inspect agentOS worktree"
    );
    Ok(status.stdout.is_empty())
}

fn timing_qemu_config(args: &TestArgs, profile: &HostProfilePlan) -> anyhow::Result<String> {
    let qemu = profile
        .qemu
        .as_ref()
        .context("live timing receipt requires a QEMU profile")?;
    let sel4_profile = std::env::var("SEL4_PROFILE").unwrap_or_else(|_| String::from("release"));
    let smp = if sel4_profile.starts_with("smp-") || sel4_profile == "smp" {
        4
    } else {
        1
    };
    Ok(format!(
        "board={}\nmachine={}\nmemory={}\ncpu=cortex-a57\nsmp={smp}\nsel4_profile={sel4_profile}\naccel=tcg\nvirtio_mmio_force_legacy=off\nauthenticated_ssh={}\nassert_agentos_virtio={}\n",
        args.board, qemu.machine, qemu.memory, args.assert_live || args.seeded_ssh_key.is_some(), args.assert_agentos_virtio
    ))
}

fn sha256_file(path: &Path) -> anyhow::Result<String> {
    let mut file = std::fs::File::open(path)
        .with_context(|| format!("open {} for SHA-256", path.display()))?;
    let mut digest = Sha256::new();
    let mut buffer = [0u8; 64 * 1024];
    loop {
        let bytes = file
            .read(&mut buffer)
            .with_context(|| format!("read {} for SHA-256", path.display()))?;
        if bytes == 0 {
            break;
        }
        digest.update(&buffer[..bytes]);
    }
    Ok(format!("{:x}", digest.finalize()))
}

fn guest_bundle_sha256(repo_root: &Path, board: &str) -> anyhow::Result<String> {
    let bundle = repo_root
        .join("build")
        .join(board)
        .join("guest-bundle-primary");
    let mut digest = Sha256::new();
    for name in ["kernel.bin", "guest.dtb", "initrd.bin", "profile.bin"] {
        digest.update(name.as_bytes());
        digest.update([0]);
        let path = bundle.join(name);
        let mut file = std::fs::File::open(&path)
            .with_context(|| format!("open {} for guest bundle SHA-256", path.display()))?;
        let mut buffer = [0u8; 64 * 1024];
        loop {
            let bytes = file
                .read(&mut buffer)
                .with_context(|| format!("read {} for guest bundle SHA-256", path.display()))?;
            if bytes == 0 {
                break;
            }
            digest.update(&buffer[..bytes]);
        }
    }
    Ok(format!("{:x}", digest.finalize()))
}

fn sha256_bytes(bytes: &[u8]) -> String {
    format!("{:x}", Sha256::digest(bytes))
}

pub(crate) fn sel4_sdk_path() -> anyhow::Result<PathBuf> {
    if let Some(path) = std::env::var_os("SEL4_SDK") {
        return Ok(PathBuf::from(path));
    }

    let home = std::env::var_os("HOME").context(
        "SEL4_SDK is unset and HOME is unavailable; set SEL4_SDK to the external Microkit SDK",
    )?;
    let version = std::env::var("SEL4_SDK_VERSION").unwrap_or_else(|_| {
        include_str!("../../tools/sdk/default-version")
            .trim()
            .to_owned()
    });
    Ok(PathBuf::from(home).join(format!(".cache/agentos/microkit-sdk-{version}")))
}

fn attach_profile_media(
    command: &mut std::process::Command,
    repo_root: &Path,
    profile: &HostProfilePlan,
) {
    let Some(plan) = &profile.qemu else { return };
    for media in &plan.media {
        let mut media_path = repo_root.join(&media.path);
        for env_name in &media.override_env {
            if let Some(value) = std::env::var_os(env_name) {
                let candidate = PathBuf::from(value);
                if candidate.exists() {
                    media_path = candidate;
                    break;
                }
            }
        }
        if media_path.exists() {
            println!(
                "[xtask:test] profile {} host block media: {}",
                profile.id,
                media_path.display()
            );
            command.args([
                "-device",
                &format!(
                    "virtio-blk-device,drive={},bus=virtio-mmio-bus.{}",
                    media.drive_id, media.bus
                ),
                "-drive",
                &format!(
                    "file={},format=raw,id={},if=none,readonly={},snapshot={},file.locking=off",
                    media_path.display(),
                    media.drive_id,
                    if media.writable { "off" } else { "on" },
                    if media.writable && !media.managed_persistent {
                        "on"
                    } else {
                        "off"
                    }
                ),
            ]);
        }
    }
}

fn validate_x86_distinct_media(primary: &Path, secondary: &Path) -> anyhow::Result<()> {
    use std::os::unix::fs::MetadataExt;
    let mut identities = Vec::new();
    for path in [primary, secondary] {
        let text = path.to_str().context("Intel disk path must be UTF-8")?;
        anyhow::ensure!(
            !text.contains(','),
            "Intel disk path cannot contain a comma"
        );
        let metadata = std::fs::metadata(path)
            .with_context(|| format!("inspect Intel disk {}", path.display()))?;
        anyhow::ensure!(
            metadata.is_file() && metadata.len() > 0 && metadata.len() % 512 == 0,
            "Intel disk must be a nonempty sector-aligned regular file: {}",
            path.display()
        );
        identities.push((metadata.dev(), metadata.ino()));
    }
    anyhow::ensure!(
        identities[0] != identities[1],
        "Intel media must be distinct files, including through links"
    );
    Ok(())
}

pub(crate) fn spawn_qemu_with_guest(
    board: &str,
    repo_root: &Path,
    log_path: &Path,
    cc_sock: &Path,
    profile: Option<&HostProfilePlan>,
    scenario: Option<&HostScenarioPlan>,
    ssh_port: u16,
    large_guest: bool,
    capture_net: bool,
    interactive_serial: bool,
    fast: bool,
    x86_block_image: Option<&Path>,
    x86_block_write: bool,
    x86_secondary_block: Option<(&Path, bool)>,
    display: bool,
    x86_cc: bool,
) -> anyhow::Result<std::process::Child> {
    anyhow::ensure!(
        !display || board == "qemu_virt_aarch64",
        "ramfb requires AArch64"
    );
    let log_file = std::fs::File::create(log_path).context("failed to create QEMU log file")?;
    let netdev = qemu_netdev_arg(ssh_port, profile, scenario)?;

    let build_image = repo_root.join("build").join(board).join("agentos.img");

    let mut cmd = match board {
        "qemu_virt_aarch64" => {
            let build_dir = repo_root.join("build").join(board);
            let loader = build_dir.join("loader.elf");
            let _ = std::fs::remove_file(&cc_sock);
            let qemu_plan = profile.and_then(|value| value.qemu.as_ref());
            if let Some(plan) = qemu_plan {
                anyhow::ensure!(
                    plan.board == board,
                    "profile {} requires board {}, not {}",
                    profile.unwrap().id,
                    plan.board,
                    board
                );
            }
            if let Some(plan) = scenario {
                anyhow::ensure!(
                    plan.board == board,
                    "scenario {} requires board {}, not {}",
                    plan.id,
                    plan.board,
                    board
                );
            }
            let machine = scenario
                .map(|plan| plan.machine.as_str())
                .or_else(|| qemu_plan.map(|plan| plan.machine.as_str()))
                .unwrap_or("virt,virtualization=on,highmem=off,secure=off");
            let memory = scenario
                .map(|plan| plan.memory.as_str())
                .or_else(|| qemu_plan.map(|plan| plan.memory.as_str()))
                .unwrap_or(if large_guest { "3G" } else { "2G" });
            let sel4_profile =
                std::env::var("SEL4_PROFILE").unwrap_or_else(|_| String::from("release"));
            let smp = if sel4_profile.starts_with("smp-") || sel4_profile == "smp" {
                "4"
            } else {
                "1"
            };
            let use_kvm = interactive_serial && host_kvm_available(board);
            // Keep the SDK-qualified CPU model even in fast mode. QEMU's
            // evolving "max" feature set can leave this seL4 image in idle
            // before the root task starts. Fast mode changes TCG threading.
            let cpu = if use_kvm { "host" } else { "cortex-a57" };
            let serial = if interactive_serial {
                // Multiplex the monitor so the advertised Ctrl-A X exit works.
                String::from("mon:stdio")
            } else {
                format!("file:{}", log_path.display())
            };
            let mut c = std::process::Command::new("qemu-system-aarch64");
            if display {
                c.arg("-device").arg("ramfb,id=display0");
                c.arg("-qmp").arg(format!(
                    "unix:{},server=on,wait=off",
                    log_path.with_extension("display.qmp.sock").display()
                ));
            }
            c.arg("-machine")
                .arg(machine)
                .arg("-cpu")
                .arg(cpu)
                .arg("-m")
                .arg(memory)
                .arg("-smp")
                .arg(smp)
                .arg("-display")
                .arg("none")
                .arg("-monitor")
                .arg("none")
                .arg("-serial")
                .arg(serial)
                .arg("-global")
                .arg("virtio-mmio.force-legacy=off")
                .arg("-chardev")
                .arg(format!(
                    "socket,id=cc_pd_char,path={},server=on,wait=off",
                    cc_sock.display()
                ))
                .arg("-device")
                .arg("virtio-serial-device,bus=virtio-mmio-bus.2,id=vser0")
                .arg("-device")
                .arg("virtserialport,bus=vser0.0,chardev=cc_pd_char,name=cc.0,nr=1")
                .arg("-device")
                .arg(format!("loader,file={},cpu-num=0", loader.display()))
                .arg("-device")
                .arg(format!(
                    "loader,file={},addr=0x48000000",
                    build_image.display()
                ));
            if use_kvm {
                c.arg("-enable-kvm");
            } else if fast {
                c.args(["-accel", "tcg,thread=multi"]);
            }
            /*
             * Page-isolated host transport owned by net_pd. Every guest sees
             * only its separately emulated device at IPA 0x0a010000.
             */
            c.args([
                "-device",
                "virtio-net-device,netdev=net0,bus=virtio-mmio-bus.16,mac=02:00:00:00:00:01,ctrl_vq=off,mq=off",
                "-netdev",
                &netdev,
            ]);
            if let Some(plan) = profile {
                attach_profile_media(&mut c, repo_root, plan);
            }
            if let Some(plan) = scenario {
                for guest in &plan.guests {
                    attach_profile_media(&mut c, repo_root, &guest.profile);
                }
            }
            c
        }
        "qemu_virt_riscv64" => {
            let bios = find_opensbi_bios();
            let mut c = std::process::Command::new("qemu-system-riscv64");
            c.args([
                "-machine",
                "virt",
                "-cpu",
                "rv64",
                "-m",
                "2G",
                "-nographic",
                "-bios",
                &bios,
                "-kernel",
                build_image
                    .to_str()
                    .unwrap_or("build/qemu_virt_riscv64/agentos.img"),
                /* virtio-net (slot 0 → 0x10001000, IRQ 1) with SSH port forward */
                "-device",
                "virtio-net-device,netdev=net0",
                "-netdev",
                &netdev,
            ]);
            /* virtio-blk (slot 1 → 0x10002000, IRQ 2) — only if disk image exists */
            let disk = repo_root.join("build/qemu_virt_riscv64/disk.img");
            if disk.exists() {
                c.args([
                    "-device",
                    "virtio-blk-device,drive=hd0",
                    "-drive",
                    &format!(
                        "file={},format=raw,id=hd0,if=none",
                        disk.to_str().unwrap_or("build/qemu_virt_riscv64/disk.img")
                    ),
                ]);
            }
            c
        }
        "x86_64_generic" => {
            let kernel = sel4_sdk_path()?.join("board/x86_64_generic/release/elf/sel4_32.elf");
            let root_task = repo_root.join("build/x86_64_generic/root_task.elf");
            let mut c = std::process::Command::new("qemu-system-x86_64");
            c.arg("-machine")
                .arg("q35")
                .arg("-cpu")
                .arg("max")
                .arg("-m")
                .arg("2G")
                .arg("-display")
                .arg("none")
                .arg("-monitor")
                .arg("none")
                .arg("-serial")
                .arg("stdio")
                .arg("-kernel")
                .arg(kernel)
                .arg("-initrd")
                .arg(root_task)
                .arg("-netdev")
                .arg(netdev)
                .arg("-device")
                .arg("e1000,netdev=net0");
            c
        }
        "x86_64_generic_vtx" => {
            anyhow::ensure!(
                host_kvm_available(board),
                "x86_64_generic_vtx requires Linux x86_64 with accessible /dev/kvm"
            );
            let kernel = sel4_sdk_path()?.join("board/x86_64_generic_vtx/release/elf/sel4_32.elf");
            let root_task = repo_root.join("build/x86_64_generic_vtx/root_task.elf");
            // Default to a fresh read-only fixture. The storage gate passes
            // its own retained disk explicitly for its two cold boots.
            let block_path = x86_block_image
                .map(Path::to_path_buf)
                .unwrap_or_else(|| log_path.with_extension("block.img"));
            if x86_block_image.is_none() {
                let mut block = std::fs::OpenOptions::new()
                    .write(true)
                    .create_new(true)
                    .open(&block_path)
                    .context("create Intel block qualification medium")?;
                block.set_len(32 * 1024 * 1024)?;
                block.write_all(b"agentos-host-block-qualification-v1\n")?;
                block.seek(SeekFrom::Start(4096))?;
                block.write_all(b"agentos-guest-block-qualification-v1\n")?;
                block.sync_all()?;
            }
            println!("[xtask:test] Intel block medium: {}", block_path.display());
            let mut c = std::process::Command::new("qemu-system-x86_64");
            let _ = std::fs::remove_file(&cc_sock);
            c.arg("-machine")
                .arg("q35")
                .arg("-enable-kvm")
                .arg("-cpu")
                // Qualification runs on this host and is never migrated.
                // Keep invariant TSC visible for the VMM's virtual timers.
                .arg("host,migratable=off")
                .arg("-m")
                // Reserve room for the bounded 2 GiB guest plus native PDs.
                .arg("4G")
                .arg("-display")
                .arg("none")
                .arg("-monitor")
                .arg("none")
                .arg("-serial")
                .arg("stdio")
                .arg("-kernel")
                .arg(kernel)
                .arg("-initrd")
                .arg(root_task);
            c.arg("-drive")
                .arg(format!(
                    "file={},format=raw,id=agentos_blk,if=none,readonly={},cache=writeback",
                    block_path.display(),
                    if x86_block_write { "off" } else { "on" }
                ))
                .arg("-device")
                .arg("virtio-blk-pci,drive=agentos_blk,addr=05.0,disable-legacy=on");
            if let Some((secondary, writable)) = x86_secondary_block {
                validate_x86_distinct_media(&block_path, secondary)?;
                println!(
                    "[xtask:test] Intel secondary block medium: {}",
                    secondary.display()
                );
                c.arg("-drive")
                    .arg(format!(
                        "file={},format=raw,id=agentos_blk_secondary,if=none,readonly={},cache=writeback",
                        secondary.display(), if writable { "off" } else { "on" }
                    ))
                    .arg("-device")
                    .arg("virtio-blk-pci,drive=agentos_blk_secondary,addr=08.0,disable-legacy=on");
            }
            c.arg("-netdev")
                .arg(if ssh_port == 0 { "user,id=agentos_net,restrict=on".into() } else { format!("user,id=agentos_net,restrict=on,hostfwd=tcp:127.0.0.1:{ssh_port}-10.0.2.15:22") })
                .arg("-device")
                .arg("virtio-net-pci,netdev=agentos_net,addr=06.0,disable-legacy=on,mac=52:54:00:12:34:56");
            let capture = log_path.with_extension("pcap");
            println!("[xtask:test] Intel NIC capture: {}", capture.display());
            c.arg("-object").arg(format!(
                "filter-dump,id=agentos_net_capture,netdev=agentos_net,file={}",
                capture.display()
            ));
            if x86_cc {
                c.arg("-chardev")
                    .arg(format!(
                        "socket,id=cc_pd_char,path={},server=on,wait=off",
                        cc_sock.display()
                    ))
                    .arg("-device")
                    .arg("virtio-serial-pci,id=cc_serial,addr=07.0,disable-legacy=on")
                    .arg("-device")
                    .arg("virtserialport,bus=cc_serial.0,chardev=cc_pd_char,name=cc.0,nr=1");
            } else {
                c.arg("-chardev")
                    .arg(format!(
                        "socket,id=serial2,path={},server=on,wait=off",
                        cc_sock.display()
                    ))
                    .arg("-serial")
                    .arg("chardev:serial2");
            }
            c
        }
        other => {
            anyhow::bail!(
                "unknown board: {} — add QEMU invocation to cmd_test.rs",
                other
            );
        }
    };

    if capture_net {
        let capture_path = log_path.with_extension("net.pcap");
        println!(
            "[xtask:test] Capturing host-backed guest packets in {}",
            capture_path.display()
        );
        cmd.arg("-object").arg(format!(
            "filter-dump,id=agentos_net_capture,netdev=net0,file={}",
            capture_path.display()
        ));
    }

    let child = if interactive_serial {
        cmd.stdin(Stdio::inherit())
            .stdout(Stdio::inherit())
            .stderr(Stdio::inherit())
            // Keep the controlling terminal's foreground process group.
            // A new group receives SIGTTIN when QEMU reads inherited stdin.
            .spawn()
            .context("failed to spawn interactive QEMU")?
    } else if board == "qemu_virt_aarch64" {
        let stderr_path = log_path.with_extension("qemu.stderr");
        let stderr_file = std::fs::File::create(&stderr_path)
            .with_context(|| format!("failed to create {}", stderr_path.display()))?;
        println!("[xtask:test] QEMU stderr: {}", stderr_path.display());
        cmd.stdout(std::process::Stdio::null())
            .stderr(stderr_file)
            .process_group(0)
            .spawn()
            .context("failed to spawn QEMU")?
    } else {
        cmd.stdout(log_file.try_clone()?)
            .stderr(log_file)
            .process_group(0)
            .spawn()
            .context("failed to spawn QEMU")?
    };
    println!("[xtask:test] QEMU pid={}", child.id());
    Ok(child)
}

fn x86_console_host_key(text: &str) -> anyhow::Result<Option<String>> {
    let Some((_, keys)) = text.split_once("-----BEGIN SSH HOST KEY KEYS-----") else {
        return Ok(None);
    };
    let Some((keys, _)) = keys.split_once("-----END SSH HOST KEY KEYS-----") else {
        return Ok(None);
    };
    let mut found = None;
    for line in keys.lines() {
        let fields: Vec<_> = line.split_whitespace().collect();
        if fields.first() == Some(&"ssh-ed25519") {
            anyhow::ensure!(
                fields.len() >= 2
                    && fields[1].len() <= 128
                    && !fields[1].is_empty()
                    && fields[1]
                        .bytes()
                        .all(|b| b.is_ascii_alphanumeric() || b"+/=".contains(&b)),
                "invalid console host-key encoding"
            );
            anyhow::ensure!(found.is_none(), "multiple Ed25519 console host keys");
            found = Some(format!("ssh-ed25519 {}", fields[1]));
        }
    }
    anyhow::ensure!(
        found.is_some(),
        "console host-key report has no Ed25519 key"
    );
    Ok(found)
}

fn seeded_recreation_via_cc(
    repo: &Path,
    input_helper: Option<&Path>,
    socket: &Path,
    log: &Path,
    profile: &HostProfilePlan,
    key: &Path,
    port: u16,
    timeout: Duration,
    qemu: &mut Child,
) -> anyhow::Result<String> {
    let ssh = profile
        .qemu
        .as_ref()
        .and_then(|q| q.ssh.as_ref())
        .context("seeded recreation requires SSH")?;
    let mut cc = connect_cc_client(socket, timeout.min(Duration::from_secs(30)), qemu)?;
    anyhow::ensure!(
        cc.call(MSG_CC_GUEST_STATUS, 0, 0, 0, &[])?.mr[0] == CC_ERR_BAD_HANDLE,
        "managed seeded image exposed an automatic guest"
    );
    let token = format!(
        "managed-{}",
        log.file_stem()
            .context("missing log stem")?
            .to_string_lossy()
    );
    let mut retired = None;
    let original_known = log.with_extension("first.known_hosts");
    for second in [false, true] {
        let round_log = log.with_extension(if second { "second.log" } else { "first.log" });
        let handle = create_guest_via_cc_wait(
            &mut cc,
            profile.control_type as u8,
            VIBEOS_ARCH_AARCH64,
            64,
            &profile.id,
            timeout,
            qemu,
        )?;
        anyhow::ensure!(
            handle != 0 && Some(handle) != retired,
            "reused managed guest handle"
        );
        // QEMU's CC chardev accepts one client. Each proof owns its connection;
        // never leave the lifecycle connection open while a proof reconnects.
        drop(cc);
        seeded_ssh_via_cc(
            socket,
            &round_log,
            profile,
            key,
            second.then_some(original_known.as_path()),
            None,
            port,
            timeout,
            qemu,
            handle,
        )?;
        {
            let mut console = connect_cc_client(socket, Duration::from_secs(30), qemu)?;
            cc_send_raw_byte(&mut console, handle, b'\r')?;
        }
        let known = round_log.with_extension("known_hosts");
        let identity = SshTestKey {
            _temporary_dir: None,
            private_key: key.to_path_buf(),
            public_key: String::new(),
            known_hosts: Some(known.clone()),
        };
        prove_seeded_profile_steps(socket, &round_log, profile, &identity, false, qemu, handle)?;
        if profile.devices.iter().any(|d| d == "gpu") {
            for extension in ["frame.ppm", "frame.json"] {
                std::fs::copy(
                    socket.with_extension(extension),
                    round_log.with_extension(extension),
                )?;
            }
        }
        if profile.devices.iter().any(|d| d == "input") {
            prove_profile_input(
                repo,
                socket,
                &round_log,
                input_helper.context("managed input helper missing")?,
                profile,
                &identity,
                qemu,
                handle,
            )?;
        }
        let stdout = round_log.with_extension("witness.stdout");
        let stderr = round_log.with_extension("witness.stderr");
        let mut command = seeded_ssh_command_with_liveness(
            key,
            port,
            &known,
            &ssh.account,
            SSH_SESSION_LIVENESS_OPTIONS,
        );
        command
            .arg("sudo -n timeout 60 sh -s")
            .stdin(Stdio::piped())
            .stdout(std::fs::File::create(&stdout)?)
            .stderr(std::fs::File::create(&stderr)?);
        let mut child = ChildGuard::new(command.spawn()?);
        child
            .stdin
            .take()
            .context("witness stdin missing")?
            .write_all(persistence_script(&token, second)?.as_bytes())?;
        wait_qualification_child(&mut child, qemu, 90)?;
        anyhow::ensure!(
            std::fs::read(&stdout)? == format!("{token}\n").as_bytes(),
            "managed disk witness mismatch; see {}",
            stdout.display()
        );
        cc = connect_cc_client(socket, Duration::from_secs(30), qemu)?;
        if let Some(old) = retired {
            for opcode in [
                MSG_CC_GUEST_STATUS,
                MSG_CC_RESUME_GUEST,
                MSG_CC_SUSPEND_GUEST,
            ] {
                anyhow::ensure!(
                    cc.call(opcode, old, 0, 0, &[])?.mr[0] == CC_ERR_BAD_HANDLE,
                    "retired handle became usable after reconstruction"
                );
            }
        }
        destroy_guest_via_cc(&mut cc, handle, Some(profile))?;
        println!("[xtask:test] managed seeded round second={second} handle={handle}: pinned SSH, profile assertions, disk witness and destroy passed");
        retired = Some(handle);
    }
    Ok("Managed seeded guest recreated with fresh handle, pinned SSH identity, persistent disk witness and stale-handle rejection".into())
}

fn seeded_ssh_via_cc(
    socket: &Path,
    log: &Path,
    profile: &HostProfilePlan,
    key: &Path,
    known: Option<&Path>,
    witness: Option<&str>,
    port: u16,
    timeout: Duration,
    qemu: &mut Child,
    handle: u32,
) -> anyhow::Result<String> {
    anyhow::ensure!(
        profile.architecture == "aarch64"
            && profile.provision.is_empty()
            && profile.console.interaction.is_empty(),
        "seeded SSH requires an ARM profile without console provisioning or interactions"
    );
    let ssh = profile
        .qemu
        .as_ref()
        .and_then(|q| q.ssh.as_ref())
        .context("seeded profile requires host.qemu.ssh")?;
    let deadline = Instant::now() + timeout;
    let retained = known
        .map(|path| x86_retained_host_key(path, port))
        .transpose()?;
    let mut cc = connect_cc_client(socket, timeout.min(Duration::from_secs(30)), qemu)?;
    let mut transcript = String::new();
    let mut output = std::fs::File::create(log.with_extension("console.log"))?;
    let mut progress = Instant::now();
    let markers = profile_console_markers(profile);
    anyhow::ensure!(
        !markers.is_empty(),
        "seeded profile requires console login markers"
    );
    while Instant::now() < deadline {
        ensure_qemu_running(qemu, "waiting for seeded guest console identity")?;
        let chunk = match cc_log_stream_for_handle(&mut cc, handle, Some(profile)) {
            Ok(chunk) => chunk,
            Err(error) if cc.is_closed() => {
                return Err(error).context("seeded CC transport closed")
            }
            Err(_) => {
                std::thread::sleep(Duration::from_millis(250));
                continue;
            }
        };
        output.write_all(chunk.as_bytes())?;
        transcript.push_str(&chunk);
        anyhow::ensure!(
            transcript.len() <= 1024 * 1024,
            "seeded console exceeds 1 MiB"
        );
        reject_profile_console(Some(&profile.console), &transcript)?;
        let login = profile
            .console
            .require
            .iter()
            .all(|marker| transcript.contains(marker))
            && markers.iter().any(|marker| transcript.contains(marker));
        if login {
            let host_key = match &retained {
                Some(value) => Some(value.clone()),
                None => x86_console_host_key(&transcript)?,
            };
            if let Some(host_key) = host_key {
                return seeded_ssh_proof(
                    key,
                    port,
                    &host_key,
                    log,
                    deadline,
                    &ssh.account,
                    "aarch64",
                    witness.map(|token| (token, known.is_some())),
                );
            }
        }
        if progress.elapsed() >= Duration::from_secs(30) {
            println!(
                "[xtask:test] seeded console: {} bytes; login={login}; tail:\n{}",
                transcript.len(),
                tail_chars(&transcript, 800)
            );
            progress = Instant::now();
        }
        std::thread::sleep(Duration::from_millis(if chunk.is_empty() {
            250
        } else {
            10
        }));
    }
    anyhow::bail!(
        "seeded guest console identity timed out; see {}",
        log.with_extension("console.log").display()
    )
}

fn apply_test_ssh_identity(command: &mut std::process::Command, key: &SshTestKey) {
    apply_ssh_identity(command, key.known_hosts.as_deref());
}

pub(crate) fn apply_ssh_identity(command: &mut std::process::Command, known: Option<&Path>) {
    if let Some(known) = known {
        command
            .args([
                "-F",
                "/dev/null",
                "-o",
                "BatchMode=yes",
                "-o",
                "IdentitiesOnly=yes",
                "-o",
                "IdentityAgent=none",
                "-o",
                "PreferredAuthentications=publickey",
                "-o",
                "PasswordAuthentication=no",
                "-o",
                "KbdInteractiveAuthentication=no",
                "-o",
                "StrictHostKeyChecking=yes",
                "-o",
                "GlobalKnownHostsFile=/dev/null",
                "-o",
                "HostKeyAlgorithms=ssh-ed25519",
            ])
            .arg("-o")
            .arg(format!("UserKnownHostsFile={}", known.display()));
    } else {
        command.args(SSH_AUTH_OPTIONS);
    }
}

fn prove_seeded_profile_steps(
    socket: &Path,
    log: &Path,
    profile: &HostProfilePlan,
    key: &SshTestKey,
    display: bool,
    qemu: &mut Child,
    handle: u32,
) -> anyhow::Result<()> {
    let ssh = profile
        .qemu
        .as_ref()
        .and_then(|q| q.ssh.as_ref())
        .context("seeded test steps require SSH")?;
    for (index, step) in profile
        .test
        .iter()
        .enumerate()
        .filter(|(_, step)| step.action == "assert-ssh-output")
    {
        let stdout = log.with_extension(format!("profile-step-{index}.stdout"));
        let stderr = log.with_extension(format!("profile-step-{index}.stderr"));
        let mut command = std::process::Command::new("ssh");
        command
            .arg("-i")
            .arg(&key.private_key)
            .args(["-p", &ssh.host_port.to_string()])
            .args(SSH_PROBE_LIVENESS_OPTIONS);
        apply_test_ssh_identity(&mut command, key);
        command
            .arg(format!("{}@127.0.0.1", ssh.account))
            .arg(format!(
                "sudo -n timeout 180 sh -c '{}'",
                step.args["command"].replace('\'', "'\\''")
            ))
            .stdin(Stdio::null())
            .stdout(std::fs::File::create(&stdout)?)
            .stderr(std::fs::File::create(&stderr)?);
        let mut child = ChildGuard::new(command.spawn()?);
        wait_qualification_child(&mut child, qemu, 200).with_context(|| {
            format!(
                "profile SSH step {index} failed; stdout: {}; stderr: {}",
                stdout.display(),
                stderr.display()
            )
        })?;
        anyhow::ensure!(
            std::fs::metadata(&stdout)?.len() <= 4096,
            "profile SSH output exceeds 4096 bytes; see {}",
            stdout.display()
        );
        anyhow::ensure!(
            std::fs::read(&stdout)? == step.args["stdout"].as_bytes(),
            "profile SSH output mismatch; see {}",
            stdout.display()
        );
    }
    if profile.devices.iter().any(|device| device == "gpu") {
        anyhow::ensure!(
            profile.test.iter().any(|s| s.action == "assert-ssh-output")
                && profile
                    .test
                    .iter()
                    .any(|s| s.action == "assert-frame-pixels"),
            "seeded graphics requires SSH preparation and exact frame assertions"
        );
        let mut cc = connect_cc_client(socket, Duration::from_secs(30), qemu)?;
        if display {
            suspend_guest_via_cc(&mut cc, handle)?;
        }
        let proof = (|| -> anyhow::Result<()> {
            println!(
                "[xtask:test] {}",
                capture_guest_frame(&mut cc, handle, socket, Some(profile))?
            );
            if display {
                let expected = std::fs::read(socket.with_extension("frame.ppm"))?;
                let receipt: serde_json::Value =
                    serde_json::from_slice(&std::fs::read(socket.with_extension("frame.json"))?)?;
                println!(
                    "[xtask:test] {}",
                    verify_display(
                        log,
                        &expected,
                        receipt["width"].as_u64().context("missing frame width")?,
                        receipt["height"].as_u64().context("missing frame height")?
                    )?
                );
            }
            Ok(())
        })();
        let resumed = if display {
            resume_guest_via_cc(&mut cc, handle).map(|_| ())
        } else {
            Ok(())
        };
        proof?;
        resumed?;
    }
    Ok(())
}

fn x86_retained_host_key(path: &Path, port: u16) -> anyhow::Result<String> {
    anyhow::ensure!(
        std::fs::metadata(path)?.len() <= 4096,
        "known_hosts receipt exceeds 4096 bytes"
    );
    let text = std::fs::read_to_string(path)?;
    let fields: Vec<_> = text.split_whitespace().collect();
    anyhow::ensure!(
        fields.len() == 3
            && fields[0] == format!("[127.0.0.1]:{port}")
            && fields[1] == "ssh-ed25519",
        "expected one loopback Ed25519 known_hosts receipt for the selected port"
    );
    anyhow::ensure!(
        !fields[2].is_empty()
            && fields[2].len() <= 128
            && fields[2]
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b"+/=".contains(&b)),
        "invalid retained host-key encoding"
    );
    Ok(format!("ssh-ed25519 {}", fields[2]))
}

fn seeded_ssh_command(key: &Path, port: u16, known: &Path, account: &str) -> std::process::Command {
    seeded_ssh_command_with_liveness(
        key,
        port,
        known,
        account,
        &["-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=2"],
    )
}

fn seeded_ssh_command_with_liveness(
    key: &Path,
    port: u16,
    known: &Path,
    account: &str,
    liveness: &[&str],
) -> std::process::Command {
    let mut command = std::process::Command::new("ssh");
    command
        .args([
            "-F",
            "/dev/null",
            "-o",
            "BatchMode=yes",
            "-o",
            "IdentitiesOnly=yes",
            "-o",
            "IdentityAgent=none",
            "-o",
            "PreferredAuthentications=publickey",
            "-o",
            "PasswordAuthentication=no",
            "-o",
            "KbdInteractiveAuthentication=no",
            "-o",
            "StrictHostKeyChecking=yes",
            "-o",
            "GlobalKnownHostsFile=/dev/null",
            "-o",
            "HostKeyAlgorithms=ssh-ed25519",
            "-o",
            "ConnectTimeout=10",
        ])
        .args(liveness)
        .arg("-o")
        .arg(format!("UserKnownHostsFile={}", known.display()))
        .arg("-i")
        .arg(key)
        .arg("-p")
        .arg(port.to_string())
        .arg(format!("{account}@127.0.0.1"));
    command
}

fn seeded_ssh_proof(
    key: &Path,
    port: u16,
    host_key: &str,
    log: &Path,
    deadline: Instant,
    account: &str,
    expected_arch: &str,
    witness: Option<(&str, bool)>,
) -> anyhow::Result<String> {
    let known = log.with_extension("known_hosts");
    let mut known_file = std::fs::OpenOptions::new()
        .create_new(true)
        .write(true)
        .open(&known)?;
    writeln!(known_file, "[127.0.0.1]:{port} {host_key}")?;
    let mut attempt = 0;
    let mut witness_pending = false;
    while Instant::now() < deadline {
        attempt += 1;
        let stdout_path = log.with_extension(format!("ssh-{attempt}.out"));
        let stderr_path = log.with_extension(format!("ssh-{attempt}.err"));
        let mut command = seeded_ssh_command(key, port, &known, account);
        command
            .arg(if witness_pending {
                let (token, second) = witness.context("missing persistence witness")?;
                let script = persistence_script(token, second)?;
                format!("sudo -n sh -c '{}'", script.replace('\'', "'\\''"))
            } else {
                "uname -m && sudo -n sync".to_string()
            })
            .stdin(Stdio::null())
            .stdout(std::fs::File::create(&stdout_path)?)
            .stderr(std::fs::File::create(&stderr_path)?);
        let mut child = ChildGuard::new(command.spawn()?);
        let attempt_deadline = deadline.min(Instant::now() + Duration::from_secs(90));
        let status = loop {
            if let Some(status) = child.try_wait()? {
                break Some(status);
            }
            if Instant::now() >= attempt_deadline {
                break None;
            }
            std::thread::sleep(Duration::from_millis(100));
        };
        drop(child);
        if witness_pending {
            // Never replay a write after uncertain delivery. A fresh qualification
            // is required if the single authenticated witness command fails.
            anyhow::ensure!(
                status.is_some_and(|s| s.success()),
                "seeded persistence command failed or timed out; see {}",
                stderr_path.display()
            );
            let (token, second) = witness.context("missing persistence witness")?;
            anyhow::ensure!(
                std::fs::read(&stdout_path)? == format!("{token}\n").as_bytes(),
                "seeded persistence witness output mismatch"
            );
            return Ok(format!("Public-key SSH verified with pinned Ed25519 host key; {expected_arch}; persistence witness {}",
                if second { "survived cold boot" } else { "written and synced" }));
        }
        if status.is_some_and(|s| s.success()) {
            anyhow::ensure!(
                std::fs::read(&stdout_path)? == format!("{expected_arch}\n").as_bytes(),
                "seeded SSH architecture output mismatch"
            );
            if witness.is_some() {
                witness_pending = true;
                continue;
            }
            return Ok(format!("Public-key SSH verified with pinned Ed25519 host key; {expected_arch} and sync succeeded"));
        }
        if Instant::now() < deadline {
            std::thread::sleep(Duration::from_millis(500));
        }
    }
    anyhow::bail!(
        "Seeded SSH proof timed out; retained attempts beside {}",
        log.display()
    )
}

#[cfg(test)]
fn x86_linux_login(socket: &Path, log_path: &Path, timeout: Duration) -> anyhow::Result<String> {
    x86_linux_login_probe(socket, log_path, timeout, None)
}

fn x86_has_login_prompt(text: &str) -> bool {
    // printk records can interrupt getty between the hostname and its prompt.
    // Remove only complete timestamped records for prompt recognition. The
    // caller retains and checks the original bytes for guest faults first.
    let mut getty = String::with_capacity(text.len());
    for line in text.split_inclusive('\n') {
        let interruption = line.find('[').filter(|&start| {
            let Some((timestamp, _)) = line[start + 1..].split_once("] ") else {
                return false;
            };
            let Some((seconds, fraction)) = timestamp.trim_start().split_once('.') else {
                return false;
            };
            line.ends_with('\n')
                && !seconds.is_empty()
                && !fraction.is_empty()
                && seconds.bytes().all(|b| b.is_ascii_digit())
                && fraction.bytes().all(|b| b.is_ascii_digit())
        });
        getty.push_str(interruption.map_or(line, |start| &line[..start]));
    }
    getty.lines().any(|line| {
        let Some((hostname, _)) = line.split_once(" login:") else {
            return false;
        };
        let hostname = hostname.trim();
        !hostname.is_empty()
            && hostname.len() <= 253
            && hostname
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b".-_".contains(&b))
    })
}

fn x86_linux_login_probe(
    socket: &Path,
    log_path: &Path,
    timeout: Duration,
    ssh: Option<(&Path, u16, Option<&Path>)>,
) -> anyhow::Result<String> {
    let deadline = Instant::now() + timeout;
    let mut stream = loop {
        match UnixStream::connect(socket) {
            Ok(stream) => break stream,
            Err(error) if Instant::now() >= deadline => return Err(error.into()),
            Err(_) => std::thread::sleep(Duration::from_millis(20)),
        }
    };
    stream.set_read_timeout(Some(Duration::from_millis(200)))?;
    x86_linux_login_reader(|chunk| stream.read(chunk), log_path, deadline, ssh)
}

fn x86_linux_login_reader(
    read: impl FnMut(&mut [u8]) -> std::io::Result<usize>,
    log_path: &Path,
    deadline: Instant,
    ssh: Option<(&Path, u16, Option<&Path>)>,
) -> anyhow::Result<String> {
    x86_linux_login_reader_with_artifacts(read, log_path, log_path, deadline, ssh)
}

fn x86_linux_login_reader_with_artifacts(
    mut read: impl FnMut(&mut [u8]) -> std::io::Result<usize>,
    target_log_path: &Path,
    log_path: &Path,
    deadline: Instant,
    ssh: Option<(&Path, u16, Option<&Path>)>,
) -> anyhow::Result<String> {
    let retained_key = ssh
        .and_then(|(_, port, known)| known.map(|path| x86_retained_host_key(path, port)))
        .transpose()?;
    let transcript_path = log_path.with_extension("console.log");
    let mut transcript_file = std::fs::File::create(&transcript_path)?;
    println!(
        "[xtask:test] Intel Linux console transcript: {}",
        transcript_path.display()
    );
    let mut transcript = Vec::new();
    while Instant::now() < deadline {
        let target_log = std::fs::read_to_string(target_log_path)?;
        anyhow::ensure!(
            !target_log.contains("x86 VMX EPT proof FAILED"),
            "Intel VMM reported a target failure; see {}",
            target_log_path.display()
        );
        let mut chunk = [0u8; 4096];
        match read(&mut chunk) {
            Ok(0) => anyhow::bail!("Intel Linux console closed before login"),
            Ok(count) => {
                transcript_file.write_all(&chunk[..count])?;
                transcript.extend_from_slice(&chunk[..count]);
                anyhow::ensure!(
                    transcript.len() <= 1024 * 1024,
                    "Intel Linux console exceeds 1 MiB"
                );
                let text = String::from_utf8_lossy(&transcript);
                anyhow::ensure!(
                    !text.contains("Kernel panic")
                        && !text.contains("Entering emergency mode")
                        && !text.contains("segfault at ")
                        && !text.contains("general protection fault")
                        && !text.contains("Oops:")
                        && !text.contains("reboot: Restarting system")
                        && !text.contains("reboot: System halted"),
                    "Intel Linux boot failed; see {}",
                    transcript_path.display()
                );
                if x86_has_login_prompt(&text) {
                    if let Some((key, port, _)) = ssh {
                        let host_key = if let Some(key) = &retained_key {
                            Some(key.clone())
                        } else {
                            x86_console_host_key(&text)?
                        };
                        let Some(host_key) = host_key else {
                            continue;
                        };
                        return seeded_ssh_proof(
                            key, port, &host_key, log_path, deadline, "debian", "x86_64", None,
                        );
                    }
                    return Ok(
                        "Linux login prompt through canonical virtio-console and serial_virt"
                            .into(),
                    );
                }
            }
            Err(error)
                if matches!(
                    error.kind(),
                    std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut
                ) => {}
            Err(error) => return Err(error.into()),
        }
    }
    anyhow::bail!(
        "Intel Linux login timed out; see {}",
        transcript_path.display()
    )
}

fn x86_cc_console_bytes(cc: &mut CcClient, handle: u32) -> anyhow::Result<Vec<u8>> {
    let reply = cc.call(MSG_CC_LOG_STREAM, handle, 0, 1, &[])?;
    let len = reply.mr[1] as usize;
    anyhow::ensure!(
        reply.mr[0] == CC_OK && len <= reply.shmem.len() && reply.mr[2] == handle,
        "invalid Intel CC console reply: {:?}",
        reply.mr
    );
    Ok(reply.shmem[..len].to_vec())
}

fn x86_smp_ssh_proof(probe: &Path, key: &Path, port: u16, log: &Path) -> anyhow::Result<String> {
    let size = std::fs::metadata(probe)?.len();
    anyhow::ensure!(size > 0 && size <= 1024 * 1024, "invalid SMP payload size");
    let known = log.with_extension("known_hosts");
    // The preceding SSH proof created this pinned receipt for this generation.
    x86_retained_host_key(&known, port)?;
    let output = log.with_extension("smp.out");
    let errors = log.with_extension("smp.err");
    let mut command = seeded_ssh_command(key, port, &known, "debian");
    command
        .arg(concat!(
            "umask 077; d=$(mktemp -d /tmp/agentos-smp.XXXXXX) || exit 1; ",
            "cat > \"$d/probe\" && chmod 700 \"$d/probe\" && \"$d/probe\"; ",
            "r=$?; rm -f \"$d/probe\"; rmdir \"$d\"; exit \"$r\""
        ))
        .stdin(std::fs::File::open(probe)?)
        .stdout(std::fs::File::create(&output)?)
        .stderr(std::fs::File::create(&errors)?);
    let mut child = ChildGuard::new(command.spawn()?);
    let deadline = Instant::now() + Duration::from_secs(120);
    let status = loop {
        if let Some(status) = child.try_wait()? {
            break status;
        }
        anyhow::ensure!(
            Instant::now() < deadline,
            "SMP workload timed out; see {}",
            errors.display()
        );
        std::thread::sleep(Duration::from_millis(100));
    };
    anyhow::ensure!(
        status.success(),
        "SMP workload failed; see {}",
        errors.display()
    );
    anyhow::ensure!(
        std::fs::read(&output)? == b"PASS: online=0-1 affinity=0,1 overlapping x87/SSE workers\n",
        "SMP workload evidence mismatch; see {}",
        output.display()
    );
    Ok("two online CPUs, pinned overlapping workers and x87/SSE state verified".into())
}

fn x86_reject_oversized_create(cc: &mut CcClient) -> anyhow::Result<()> {
    // The firmware composition admits at most 2 GiB. This request is above
    // that bound but still valid under the public 64..8192 MiB wire contract.
    // An empty inventory is essential: duplicate-profile rejection happens
    // before the manager's capacity check and would prove the wrong property.
    anyhow::ensure!(
        cc.call(MSG_CC_LIST_GUESTS, 64, 0, 0, &[])?.mr[0] == 0,
        "oversized admission probe requires an empty inventory"
    );
    let mut request = [0u8; 52];
    request[0] = 1;
    request[1] = VIBEOS_ARCH_X86_64;
    wr32(&mut request, 4, 2052);
    wr32(
        &mut request,
        16,
        VIBEOS_DEV_SERIAL | VIBEOS_DEV_NET | VIBEOS_DEV_BLOCK,
    );
    let reply = cc.call(MSG_CC_CREATE_GUEST, 0, 0, 0, &request)?;
    anyhow::ensure!(
        reply.mr[..3] == [CC_ERR_RELAY_FAULT, 0, 0],
        "oversized CREATE was accepted or left a public/recovery handle: {:?}",
        reply.mr
    );
    anyhow::ensure!(
        cc.call(MSG_CC_LIST_GUESTS, 64, 0, 0, &[])?.mr[0] == 0,
        "oversized CREATE changed the inventory"
    );
    println!("[xtask:test] Intel 2052 MiB CREATE rejected with no handle or inventory change");
    Ok(())
}

fn x86_cc_linux_probe(
    socket: &Path,
    log_path: &Path,
    timeout: Duration,
    ssh: Option<(&Path, u16, Option<&Path>)>,
    smp_probe: Option<&Path>,
    qemu: &mut Child,
) -> anyhow::Result<String> {
    let mut cc = connect_cc_client(socket, timeout.min(Duration::from_secs(30)), qemu)?;
    let absent = cc.call(MSG_CC_GUEST_STATUS, 0, 0, 0, &[])?;
    anyhow::ensure!(
        absent.mr[0] == CC_ERR_BAD_HANDLE,
        "CC image exposed an automatic guest"
    );
    for (handle, pd, mode, expected) in [
        (999, 0, 1, CC_ERR_BAD_HANDLE),
        (999, 0, 2, 9),
        (999, 41, 1, 9),
    ] {
        let reply = cc.call(MSG_CC_LOG_STREAM, handle, pd, mode, &[])?;
        anyhow::ensure!(
            reply.mr[0] == expected,
            "invalid console addressing was accepted"
        );
    }
    let mut proofs = Vec::new();
    let mut previous_handle = None;
    let first_known_hosts = log_path.with_extension("known_hosts");
    let mut generation_ssh = ssh;
    for generation in 0..2 {
        x86_reject_oversized_create(&mut cc)?;
        let generation_log = if generation == 0 {
            log_path.to_path_buf()
        } else {
            log_path.with_extension("recreated.log")
        };
        // This exercises public admission against the image's boot-reserved RAM.
        let handle = create_guest_via_cc_wait(
            &mut cc,
            1,
            VIBEOS_ARCH_X86_64,
            64,
            "Intel Linux",
            timeout,
            qemu,
        )?;
        anyhow::ensure!(handle != 0, "CREATE returned reserved boot handle");
        let mut proof = x86_linux_login_reader_with_artifacts(
            |chunk| {
                let bytes = x86_cc_console_bytes(&mut cc, handle).map_err(std::io::Error::other)?;
                if bytes.is_empty() {
                    std::thread::sleep(Duration::from_millis(100));
                    return Err(std::io::ErrorKind::WouldBlock.into());
                }
                if bytes.len() > chunk.len() {
                    return Err(std::io::Error::other("CC console read buffer too small"));
                }
                chunk[..bytes.len()].copy_from_slice(&bytes);
                Ok(bytes.len())
            },
            log_path,
            &generation_log,
            Instant::now() + timeout,
            generation_ssh,
        )?;
        // Cloud-init prints the host key only on first boot. Recreated guests
        // must authenticate against the key that already passed SSH, including
        // when the caller did not supply an external known-hosts file.
        generation_ssh = ssh.map(|(key, port, _)| (key, port, Some(first_known_hosts.as_path())));
        if let Some(payload) = smp_probe {
            let (key, port, _) = ssh.context("SMP qualification requires pinned SSH")?;
            proof.push_str("; ");
            proof.push_str(&x86_smp_ssh_proof(payload, key, port, &generation_log)?);
        }

        // Exercise real guest input after login readiness, without requiring a
        // password or changing the guest. The terminal must echo these exact bytes.
        let marker: &[u8] = if generation == 0 {
            b"agentos-cc-input-probe"
        } else {
            b"agentos-cc-recreated-probe"
        };
        cc_send_raw_bytes(&mut cc, handle, marker)?;
        let mut echoed = Vec::new();
        let deadline = Instant::now() + Duration::from_secs(30);
        while Instant::now() < deadline && !echoed.windows(marker.len()).any(|w| w == marker) {
            ensure_qemu_running(qemu, "checking Intel CC input echo")?;
            echoed.extend(x86_cc_console_bytes(&mut cc, handle)?);
            anyhow::ensure!(
                echoed.len() <= 1024 * 1024,
                "CC input transcript exceeds 1 MiB"
            );
            std::thread::sleep(Duration::from_millis(100));
        }
        std::fs::write(generation_log.with_extension("cc-input.log"), &echoed)?;
        anyhow::ensure!(
            echoed.windows(marker.len()).any(|w| w == marker),
            "Intel CC input was not echoed by Linux"
        );
        let destroyed = cc.call(MSG_CC_DESTROY_GUEST, handle, GUEST_DESTROY_NORMAL, 0, &[])?;
        anyhow::ensure!(
            destroyed.mr[0] == CC_OK,
            "Intel Linux destroy failed: {:?}",
            destroyed.mr
        );
        for opcode in [
            MSG_CC_GUEST_STATUS,
            MSG_CC_SUSPEND_GUEST,
            MSG_CC_RESUME_GUEST,
        ] {
            let reply = cc.call(opcode, handle, 0, 0, &[])?;
            anyhow::ensure!(
                reply.mr[0] == CC_ERR_BAD_HANDLE,
                "stale Intel handle accepted by {opcode:#x}"
            );
        }
        let stale_console = cc.call(MSG_CC_LOG_STREAM, handle, 0, 1, &[])?;
        anyhow::ensure!(
            stale_console.mr[0] == CC_ERR_BAD_HANDLE,
            "destroyed guest console remained accessible"
        );

        anyhow::ensure!(
            previous_handle != Some(handle),
            "recreation reused a stale public handle"
        );
        previous_handle = Some(handle);
        proofs.push(proof);
    }
    Ok(format!("{}; oversized RAM rejected before both generations; binary CC CREATE, input echo, DESTROY, recreate and second boot verified with stale handle rejection", proofs.join("; ")))
}

fn x86_console_roundtrip(socket: &Path, timeout: Duration) -> anyhow::Result<()> {
    let deadline = Instant::now() + timeout;
    let mut stream = loop {
        match UnixStream::connect(socket) {
            Ok(stream) => break stream,
            Err(error) if Instant::now() >= deadline => return Err(error.into()),
            Err(_) => std::thread::sleep(Duration::from_millis(20)),
        }
    };
    stream.set_read_timeout(Some(deadline.saturating_duration_since(Instant::now())))?;
    stream.set_write_timeout(Some(Duration::from_secs(5)))?;
    let mut ready = [0; b"agentos-uart-ready\n".len()];
    stream
        .read_exact(&mut ready)
        .context("Intel console readiness bytes")?;
    anyhow::ensure!(
        &ready == b"agentos-uart-ready\n",
        "Intel console readiness mismatch: {ready:?}"
    );
    stream.write_all(b"agentos-uart-request\n")?;
    let mut reply = [0; b"agentos-uart-reply\n".len()];
    stream
        .read_exact(&mut reply)
        .context("Intel console reply bytes")?;
    anyhow::ensure!(
        &reply == b"agentos-uart-reply\n",
        "Intel console reply mismatch: {reply:?}"
    );
    println!("PASS: Intel hvc0 console request/reply through COM2 and serial_virt queues");
    Ok(())
}

fn host_kvm_available(board: &str) -> bool {
    if !Path::new("/dev/kvm").exists() {
        return false;
    }
    if cfg!(all(target_os = "linux", target_arch = "aarch64")) && board == "qemu_virt_aarch64" {
        // A device node alone does not prove KVM can expose EL2 to seL4.
        // Probe the actual QEMU machine/CPU combination once, without booting
        // any image or attaching storage/network devices.
        static ARM_KVM: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
        return *ARM_KVM.get_or_init(probe_arm_kvm);
    }
    cfg!(all(target_os = "linux", target_arch = "x86_64")) && board == "x86_64_generic_vtx"
}

fn probe_arm_kvm() -> bool {
    let Ok(mut child) = std::process::Command::new("qemu-system-aarch64")
        .args([
            "-machine",
            "virt,virtualization=on",
            "-cpu",
            "host",
            "-accel",
            "kvm",
            "-m",
            "128M",
            "-S",
            "-nodefaults",
            "-display",
            "none",
            "-monitor",
            "none",
            "-serial",
            "none",
            "-qmp",
            "stdio",
        ])
        .stdin(Stdio::piped())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
    else {
        return false;
    };
    let sent = child.stdin.take().is_some_and(|mut input| {
        input
            .write_all(b"{\"execute\":\"qmp_capabilities\"}\n{\"execute\":\"quit\"}\n")
            .is_ok()
    });
    let deadline = Instant::now() + Duration::from_secs(5);
    if sent {
        while Instant::now() < deadline {
            match child.try_wait() {
                Ok(Some(status)) => return status.success(),
                Ok(None) => std::thread::sleep(Duration::from_millis(20)),
                Err(_) => break,
            }
        }
    }
    let _ = child.kill();
    let _ = child.wait();
    false
}

fn qemu_netdev_arg(
    ssh_port: u16,
    profile: Option<&HostProfilePlan>,
    scenario: Option<&HostScenarioPlan>,
) -> anyhow::Result<String> {
    if let Some(scenario) = scenario {
        for guest in &scenario.guests {
            ensure_host_port_available(guest.ssh_host_port)?;
        }
        return Ok(scenario_qemu_netdev_arg(scenario));
    }
    if ssh_port == 0 {
        return Ok("user,id=net0".to_string());
    }
    ensure_host_port_available(ssh_port)?;
    let guest = profile
        .and_then(|value| value.qemu.as_ref())
        .and_then(|qemu| qemu.ssh.as_ref())
        .and_then(|ssh| ssh.guest_address.as_deref())
        .map(|address| format!("{address}:22"))
        .unwrap_or_else(|| String::from(":22"));
    if let Some(ssh) = profile
        .and_then(|value| value.qemu.as_ref())
        .and_then(|qemu| qemu.ssh.as_ref())
    {
        println!(
            "[xtask:test] profile SSH forward: {}@127.0.0.1:{} -> {}",
            ssh.account, ssh_port, guest
        );
    }
    Ok(format!(
        "user,id=net0,hostfwd=tcp:127.0.0.1:{ssh_port}-{guest}"
    ))
}

fn scenario_qemu_netdev_arg(scenario: &HostScenarioPlan) -> String {
    let mut value = String::from("user,id=net0");
    for guest in &scenario.guests {
        value.push_str(&format!(
            ",hostfwd=tcp:127.0.0.1:{}-{}:22",
            guest.ssh_host_port, guest.ssh_guest_address
        ));
    }
    value
}

fn ensure_host_port_available(port: u16) -> anyhow::Result<()> {
    let listener = std::net::TcpListener::bind(("127.0.0.1", port)).with_context(|| {
        format!(
            "host TCP port {} is already in use; pass --ssh-port 0 to disable SSH forwarding or choose another port",
            port
        )
    })?;
    drop(listener);
    Ok(())
}

/// Locate the OpenSBI RISCV64 firmware binary, searching common locations.
fn find_opensbi_bios() -> String {
    let candidates = [
        // macOS Homebrew (both Intel and Apple Silicon)
        "/opt/homebrew/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin",
        "/usr/local/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin",
        // Linux system package
        "/usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin",
        // Debian/Ubuntu alternate path
        "/usr/lib/riscv64-linux-gnu/opensbi/generic/fw_dynamic.bin",
    ];
    for path in candidates {
        if std::path::Path::new(path).exists() {
            return path.to_string();
        }
    }
    // Also check via `brew --prefix` at runtime for non-standard Homebrew roots
    if let Ok(output) = std::process::Command::new("brew")
        .args(["--prefix"])
        .output()
    {
        if let Ok(prefix) = std::str::from_utf8(&output.stdout) {
            let p = format!(
                "{}/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin",
                prefix.trim()
            );
            if std::path::Path::new(&p).exists() {
                return p;
            }
        }
    }
    // Fall back to the Linux path and let QEMU report a clear error
    "/usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.bin".to_string()
}

/// Poll the log file until one of `markers` appears or `timeout` elapses.
/// Returns the matched marker string on success.
pub fn wait_for_markers(
    log_path: &Path,
    markers: &[&str],
    timeout: Duration,
) -> anyhow::Result<String> {
    let start = Instant::now();
    let mut file = std::fs::File::open(log_path).context("failed to open log file")?;
    let mut offset: u64 = 0;
    let mut accumulated = String::new();

    loop {
        if start.elapsed() >= timeout {
            anyhow::bail!("timeout after {}s", timeout.as_secs());
        }

        file.seek(SeekFrom::Start(offset))?;
        let mut raw = Vec::new();
        let bytes_read = file.read_to_end(&mut raw)?;
        if bytes_read > 0 {
            offset += bytes_read as u64;
            // QEMU may emit non-UTF-8 bytes (e.g. from OpenSBI/seL4 early boot);
            // replace invalid sequences rather than failing.
            accumulated.push_str(&String::from_utf8_lossy(&raw));

            for &marker in markers {
                if accumulated.contains(marker) {
                    return Ok(marker.to_string());
                }
            }
        }

        std::thread::sleep(Duration::from_millis(200));
    }
}

fn wait_for_all_markers(
    log_path: &Path,
    markers: &[&str],
    timeout: Duration,
    qemu: &mut Child,
) -> anyhow::Result<String> {
    let start = Instant::now();
    let mut file = std::fs::File::open(log_path).context("failed to open log file")?;
    let mut offset: u64 = 0;
    let mut accumulated = String::new();

    loop {
        ensure_qemu_running(qemu, "waiting for required runtime evidence")?;
        if start.elapsed() >= timeout {
            let missing: Vec<&str> = markers
                .iter()
                .copied()
                .filter(|marker| !accumulated.contains(marker))
                .collect();
            anyhow::bail!(
                "timeout after {}s waiting for all markers; missing {:?}",
                timeout.as_secs(),
                missing
            );
        }

        file.seek(SeekFrom::Start(offset))?;
        let mut raw = Vec::new();
        let bytes_read = file.read_to_end(&mut raw)?;
        if bytes_read > 0 {
            offset += bytes_read as u64;
            accumulated.push_str(&String::from_utf8_lossy(&raw));
            if markers.iter().all(|marker| accumulated.contains(marker)) {
                return Ok(markers.join(" + "));
            }
        }

        std::thread::sleep(Duration::from_millis(200));
    }
}

const EMU_NET_REQUIRED: &[&str] = &[
    "emulated virtio-net: guest probed",
    "emulated virtio-net: guest DRIVER_OK",
    "emulated virtio-net: pumped",
];

const EMU_NET_GUEST_ANY: &[&str] = &[
    "0a010000.virtio_mmio",
    "a010000.virtio_mmio",
    "02:00:00:00:00:01",
    "Sending DHCP requests",
];

const EMU_BLK_REQUIRED: &[&str] = &[
    "emulated virtio-blk: guest probed",
    "emulated virtio-blk: guest DRIVER_OK",
    "emulated virtio-blk: pumped",
    "emulated virtio-blk: drain=",
];

fn wait_for_emulated_blk(
    log_path: &Path,
    timeout: Duration,
    qemu: &mut Child,
) -> anyhow::Result<String> {
    let start = Instant::now();
    let mut file = std::fs::File::open(log_path).context("failed to open log file")?;
    let mut offset: u64 = 0;
    let mut accumulated = String::new();

    loop {
        ensure_qemu_running(qemu, "waiting for emulated virtio-blk proof")?;
        if start.elapsed() >= timeout {
            let missing: Vec<&str> = EMU_BLK_REQUIRED
                .iter()
                .copied()
                .filter(|m| !accumulated.contains(m))
                .collect();
            anyhow::bail!(
                "emulated virtio-blk proof timeout after {}s; missing VMM markers {:?}",
                timeout.as_secs(),
                missing,
            );
        }

        file.seek(SeekFrom::Start(offset))?;
        let mut raw = Vec::new();
        let bytes_read = file.read_to_end(&mut raw)?;
        if bytes_read > 0 {
            offset += bytes_read as u64;
            accumulated.push_str(&String::from_utf8_lossy(&raw));

            let vmm_ok = EMU_BLK_REQUIRED.iter().all(|m| accumulated.contains(m));
            if vmm_ok {
                return Ok(
                    "emulated virtio-blk: guest configured queue + DRIVER_OK + completed request"
                        .to_string(),
                );
            }
        }

        std::thread::sleep(Duration::from_millis(200));
    }
}

fn wait_for_emulated_net(
    log_path: &Path,
    timeout: Duration,
    qemu: &mut Child,
) -> anyhow::Result<String> {
    let start = Instant::now();
    let mut file = std::fs::File::open(log_path).context("failed to open log file")?;
    let mut offset: u64 = 0;
    let mut accumulated = String::new();

    loop {
        ensure_qemu_running(qemu, "waiting for emulated virtio-net proof")?;
        if start.elapsed() >= timeout {
            let missing: Vec<&str> = EMU_NET_REQUIRED
                .iter()
                .copied()
                .filter(|m| !accumulated.contains(m))
                .collect();
            let guest_ok = EMU_NET_GUEST_ANY.iter().any(|m| accumulated.contains(m));
            anyhow::bail!(
                "emulated virtio-net proof timeout after {}s; missing VMM markers {:?}; guest IPA/MAC observed={}",
                timeout.as_secs(),
                missing,
                guest_ok
            );
        }

        file.seek(SeekFrom::Start(offset))?;
        let mut raw = Vec::new();
        let bytes_read = file.read_to_end(&mut raw)?;
        if bytes_read > 0 {
            offset += bytes_read as u64;
            accumulated.push_str(&String::from_utf8_lossy(&raw));

            let vmm_ok = EMU_NET_REQUIRED.iter().all(|m| accumulated.contains(m));
            let guest_ok = EMU_NET_GUEST_ANY.iter().any(|m| accumulated.contains(m));
            if vmm_ok && guest_ok {
                return Ok(
                    "emulated virtio-net: guest probed + DRIVER_OK + pumped + guest IPA/MAC"
                        .to_string(),
                );
            }
        }

        std::thread::sleep(Duration::from_millis(200));
    }
}

fn wait_for_x86_reduced_smoke(log_path: &Path, timeout: Duration) -> anyhow::Result<String> {
    let marker = wait_for_markers(log_path, &["[rt] boot complete"], timeout)?;
    std::thread::sleep(Duration::from_secs(2));

    let output = std::fs::read_to_string(log_path).unwrap_or_default();
    anyhow::ensure!(
        !output.contains("[rt] FAULT"),
        "x86 reduced smoke emitted root-task fault endpoint reports"
    );
    Ok(format!(
        "{marker} (x86 reduced smoke, no fault endpoint reports)"
    ))
}

fn wait_for_x86_vtx_proof(
    log_path: &Path,
    timeout: Duration,
    qemu: &mut Child,
    firmware_modes: bool,
    firmware_reset: bool,
    guest_faults: bool,
    userspace: bool,
) -> anyhow::Result<String> {
    let expected = if userspace {
        "[rt] x86 Linux ring3 initramfs syscall proof verified"
    } else if guest_faults {
        "[rt] x86 guest GP read/write handlers and IRET recovery verified"
    } else if firmware_reset {
        "[rt] x86 OVMF PCI configuration and fw_cfg string exit verified"
    } else if firmware_modes {
        "[rt] x86 VMX real protected long entry modes verified"
    } else {
        "[rt] x86 VMX EPT HLT exit verified"
    };
    let marker = wait_for_all_markers(
        log_path,
        &["[rt] x86 VMX EPT proof provisioned", expected],
        timeout,
        qemu,
    )?;
    let output = std::fs::read_to_string(log_path).unwrap_or_default();
    anyhow::ensure!(
        !output.contains("[rt] x86 VMX EPT proof FAILED") && !output.contains("[rt] FAULT"),
        "x86 VMX/EPT proof emitted a failure or root-task fault report"
    );
    Ok(format!("{marker} (x86 VMX/EPT entry qualification)"))
}

fn verify_inspect(socket: &Path, root: &Path) -> anyhow::Result<String> {
    let first;
    {
        let mut client = CcClient::connect(socket)?;
        for version in [0, 2, u32::MAX] {
            let bad = client.call(0x261a, version, 0, 0, &[])?;
            anyhow::ensure!(
                bad.mr == [9, 0, 0, 0] && bad.shmem.iter().all(|b| *b == 0),
                "inspect invalid version returned data or wrong error"
            );
        }
        let bad = client.call(0x261a, 1, 1, 0, &[])?;
        anyhow::ensure!(bad.mr == [9, 0, 0, 0], "inspect accepted reserved argument");
        first = client.call(0x261a, 1, 0, 0, &[])?;
        anyhow::ensure!(
            first.mr == [0, 1488, 7, 1],
            "inspect header: {:?}",
            first.mr
        );
        let count = rd32(&first.shmem, 72);
        anyhow::ensure!(
            count == 14 && rd32(&first.shmem, 32) == count,
            "inspect did not report the 14 successfully started default PDs"
        );
        for i in 0..count as usize {
            anyhow::ensure!(
                rd32(&first.shmem, 80 + i * 44 + 8) == 0,
                "boot snapshot advertised live thread state"
            );
        }
    }
    let out = std::process::Command::new(root.join("tools/agentctl/agentctl"))
        .arg("--socket")
        .arg(socket)
        .arg("inspect")
        .output()
        .context("run make -C tools/agentctl before inspect qualification")?;
    anyhow::ensure!(
        out.status.success(),
        "agentctl inspect failed: {}",
        String::from_utf8_lossy(&out.stderr)
    );
    let report = String::from_utf8(out.stdout)?;
    for expected in [
        "inspect.observation=boot\n",
        "memory.ut_used_kind=accounted_pages_lower_bound\n",
        "hardware.arch=aarch64\n",
        "hardware.virtio_net_ipa=0xa010000\n",
        "hardware.virtio_net_virq=50\n",
        "memory.pd_count=14\n",
        ".name=cc_pd\n",
        ".name=net_virt\n",
        ".name=serial_virt\n",
    ] {
        anyhow::ensure!(report.contains(expected), "inspect missing {expected}");
    }
    println!("{report}");
    let mut client = CcClient::connect(socket)?;
    let second = client.call(0x261a, 1, 0, 0, &[])?;
    anyhow::ensure!(
        first.mr == second.mr && first.shmem == second.shmem,
        "boot snapshot changed after reconnect and intervening requests"
    );
    Ok("root boot observations returned by CC and agentctl; invalid requests rejected; repeat stable".into())
}

fn operator_read_exact(
    client: &mut CcClient,
    expected: &[u8],
    timeout: Duration,
) -> anyhow::Result<()> {
    let deadline = Instant::now() + timeout;
    let mut received = Vec::new();
    while received.len() < expected.len() && Instant::now() < deadline {
        let reply = client.call(0x261c, 1, 4096, 0, &[])?;
        anyhow::ensure!(
            reply.mr[0] == 0 && reply.mr[1] <= 4096,
            "operator read failed"
        );
        received.extend_from_slice(&reply.shmem[..reply.mr[1] as usize]);
        anyhow::ensure!(
            received.len() <= expected.len() && expected.starts_with(&received),
            "operator response bytes differ at offset {}",
            received.len()
        );
        if reply.mr[1] == 0 {
            std::thread::sleep(Duration::from_millis(5));
        }
    }
    anyhow::ensure!(
        received == expected,
        "operator response timed out at {}/{} bytes",
        received.len(),
        expected.len()
    );
    Ok(())
}

fn verify_operator_session(
    socket: &Path,
    root: &Path,
    timeout: Duration,
) -> anyhow::Result<String> {
    verify_inspect(socket, root)?;
    let tool = root.join("tools/agentctl/agentctl");
    let direct = std::process::Command::new(&tool)
        .arg("--socket")
        .arg(socket)
        .arg("inspect")
        .output()?;
    anyhow::ensure!(direct.status.success(), "reference boot inspection failed");
    let mut expected = format!("ok {}\n", direct.stdout.len()).into_bytes();
    expected.extend_from_slice(&direct.stdout);
    {
        let mut client = CcClient::connect(socket)?;
        for (version, length, reserved) in [(0, 0, 0), (1, 4097, 0), (1, 0, 1)] {
            let reply = client.call(0x261b, version, length, reserved, &[])?;
            anyhow::ensure!(
                reply.mr == [9, 0, 0, 0],
                "invalid operator request accepted"
            );
        }
        for chunk in [b"inspect.".as_slice(), b"snapshot\r\n".as_slice()] {
            let reply = client.call(0x261b, 1, chunk.len() as u32, 0, chunk)?;
            anyhow::ensure!(
                reply.mr == [0, chunk.len() as u32, 0, 0],
                "fragment write failed"
            );
        }
        operator_read_exact(&mut client, &expected, timeout)?;
        for (line, error) in [
            (b"bad\n".as_slice(), b"error unknown-command\n".as_slice()),
            (
                b"bad\0line\n".as_slice(),
                b"error invalid-line\n".as_slice(),
            ),
        ] {
            let reply = client.call(0x261b, 1, line.len() as u32, 0, line)?;
            anyhow::ensure!(reply.mr[0] == 0, "invalid-line transport failed");
            operator_read_exact(&mut client, error, timeout)?;
        }
        let mut long = vec![b'x'; 1024];
        long.push(b'\n');
        anyhow::ensure!(
            client.call(0x261b, 1, long.len() as u32, 0, &long)?.mr[0] == 0,
            "long-line transport failed"
        );
        operator_read_exact(&mut client, b"error line-too-long\n", timeout)?;
        // Replies exceed both 64 KiB output queues, forcing retained output
        // and input backpressure. Recover every framed response exactly.
        let burst = b"inspect.snapshot\n".repeat(128);
        anyhow::ensure!(
            expected.len() * 128 > 2 * 65536,
            "burst does not fill output queues"
        );
        anyhow::ensure!(
            client.call(0x261b, 1, burst.len() as u32, 0, &burst)?.mr[0] == 0,
            "burst rejected"
        );
        std::thread::sleep(Duration::from_millis(100));
        operator_read_exact(&mut client, &expected.repeat(128), timeout)?;
    }
    let reply = std::process::Command::new(&tool)
        .arg("--socket")
        .arg(socket)
        .arg("session-inspect")
        .output()?;
    anyhow::ensure!(
        reply.status.success() && reply.stdout == direct.stdout,
        "public serial session CLI differs from the root snapshot: {}",
        String::from_utf8_lossy(&reply.stderr)
    );
    Ok("operator serial_virt round trip: fragmentation, error recovery, 128 exact reports after backpressure, public CLI".into())
}

pub struct CcReply {
    pub mr: [u32; 4],
    pub shmem: Vec<u8>,
}

struct CcClient {
    stream: Option<UnixStream>,
}

#[cfg(test)]
fn mock_cc_sync(stream: &mut UnixStream) {
    let mut greeting = [0u8; CC_REPLY_SIZE];
    for (offset, word) in [(0, 0x43435244), (4, 1), (8, 7), (12, 9)] {
        wr32(&mut greeting, offset, word);
    }
    stream.write_all(&greeting).unwrap();
    let mut request = [0u8; CC_REQ_SIZE];
    stream.read_exact(&mut request).unwrap();
    wr32(&mut greeting, 0, 0x261f);
    assert_eq!(request, greeting);
    wr32(&mut greeting, 0, 0);
    stream.write_all(&greeting).unwrap();
}

impl CcClient {
    fn connect(cc_sock: &Path) -> anyhow::Result<Self> {
        let stream = Self::connect_stream(cc_sock)?;
        Ok(Self {
            stream: Some(stream),
        })
    }

    fn connect_stream(cc_sock: &Path) -> anyhow::Result<UnixStream> {
        let mut stream = UnixStream::connect(cc_sock)
            .with_context(|| format!("failed to connect to {}", cc_sock.display()))?;
        stream
            .set_read_timeout(Some(CC_IO_TIMEOUT))
            .context("failed to set CC socket read timeout")?;
        stream
            .set_write_timeout(Some(CC_IO_TIMEOUT))
            .context("failed to set CC socket write timeout")?;
        let mut greeting = [0u8; CC_REPLY_SIZE];
        read_cc_frame(&mut stream, &mut greeting).context("CC ready greeting")?;
        anyhow::ensure!(
            rd32(&greeting, 0) == 0x43435244
                && rd32(&greeting, 4) == 1
                && (rd32(&greeting, 8) | rd32(&greeting, 12)) != 0
                && greeting[16..].iter().all(|b| *b == 0),
            "invalid CC ready greeting"
        );
        let mut sync = greeting;
        wr32(&mut sync, 0, 0x261f);
        write_cc_frame(&mut stream, &sync).context("CC connection sync")?;
        let mut reply = [0u8; CC_REPLY_SIZE];
        read_cc_frame(&mut stream, &mut reply).context("CC connection acknowledgment")?;
        wr32(&mut greeting, 0, 0);
        anyhow::ensure!(reply == greeting, "invalid CC connection acknowledgment");
        Ok(stream)
    }

    fn is_closed(&self) -> bool {
        self.stream.is_none()
    }

    fn call(
        &mut self,
        opcode: u32,
        mr1: u32,
        mr2: u32,
        mr3: u32,
        shmem_in: &[u8],
    ) -> anyhow::Result<CcReply> {
        let mut req = [0u8; CC_REQ_SIZE];
        wr32(&mut req, 0, opcode);
        wr32(&mut req, 4, mr1);
        wr32(&mut req, 8, mr2);
        wr32(&mut req, 12, mr3);
        let copy_len = shmem_in.len().min(CC_WIRE_SHMEM_SIZE);
        if copy_len > 0 {
            req[16..16 + copy_len].copy_from_slice(&shmem_in[..copy_len]);
        }

        // A failed reply leaves delivery ambiguous. Connection close invalidates
        // the server cache and schedules input releases; never replay a request
        // into a replacement connection with a new input/lifecycle identity.
        self.call_frame(&req)
    }

    fn call_frame(&mut self, req: &[u8; CC_REQ_SIZE]) -> anyhow::Result<CcReply> {
        let result: anyhow::Result<CcReply> = (|| {
            let stream = self.stream.as_mut().context("CC connection is closed")?;
            write_cc_frame(stream, req).context("failed to write CC request")?;

            let mut raw = [0u8; CC_REPLY_SIZE];
            read_cc_frame(stream, &mut raw)?;
            Ok(CcReply {
                mr: [rd32(&raw, 0), rd32(&raw, 4), rd32(&raw, 8), rd32(&raw, 12)],
                shmem: raw[16..].to_vec(),
            })
        })();
        if result.is_err() {
            self.stream.take();
        }
        result
    }
}

fn retryable_cc_io(err: &std::io::Error) -> bool {
    matches!(
        err.kind(),
        std::io::ErrorKind::Interrupted
            | std::io::ErrorKind::WouldBlock
            | std::io::ErrorKind::TimedOut
    )
}

fn write_cc_frame(stream: &mut UnixStream, frame: &[u8]) -> anyhow::Result<()> {
    let start = Instant::now();
    let mut written = 0usize;
    while written < frame.len() {
        match stream.write(&frame[written..]) {
            Ok(0) => anyhow::bail!("CC connection closed while writing frame"),
            Ok(count) => written += count,
            Err(err) if retryable_cc_io(&err) && start.elapsed() < CC_FRAME_DEADLINE => {}
            Err(err) if retryable_cc_io(&err) => {
                anyhow::bail!("timed out writing CC frame after {} bytes", written)
            }
            Err(err) => return Err(err).context("failed to write CC frame"),
        }
    }
    Ok(())
}

fn read_cc_frame(stream: &mut UnixStream, frame: &mut [u8]) -> anyhow::Result<()> {
    let start = Instant::now();
    let mut read = 0usize;
    while read < frame.len() {
        match stream.read(&mut frame[read..]) {
            Ok(0) => anyhow::bail!("CC connection closed while reading frame"),
            Ok(count) => read += count,
            Err(err) if retryable_cc_io(&err) && start.elapsed() < CC_FRAME_DEADLINE => {}
            Err(err) if retryable_cc_io(&err) => {
                anyhow::bail!("timed out reading CC frame after {} bytes", read)
            }
            Err(err) => return Err(err).context("failed to read CC frame"),
        }
    }
    Ok(())
}

fn wait_for_guest_console_login_via_cc(
    cc_sock: &Path,
    guest_handle: u32,
    guest_os: &str,
    profile: Option<&HostProfilePlan>,
    timeout: Duration,
    qemu: &mut Child,
    display_log: Option<&Path>,
) -> anyhow::Result<String> {
    let mut cc = connect_cc_client(cc_sock, timeout.min(Duration::from_secs(30)), qemu)?;
    wait_for_guest_console_login_on_cc(
        cc_sock,
        &mut cc,
        guest_handle,
        guest_os,
        profile,
        timeout,
        qemu,
        display_log,
    )
}

fn wait_for_guest_console_login_on_cc(
    cc_sock: &Path,
    cc: &mut CcClient,
    guest_handle: u32,
    guest_os: &str,
    profile: Option<&HostProfilePlan>,
    timeout: Duration,
    qemu: &mut Child,
    display_log: Option<&Path>,
) -> anyhow::Result<String> {
    let start = Instant::now();
    let mut transcript = String::new();
    let mut matched_prompt = None;
    let console = profile.map(|value| &value.console);
    let prompt_markers = console
        .filter(|plan| !plan.success.is_empty())
        .map(|plan| plan.success.clone())
        .or_else(|| {
            profile
                .map(profile_console_markers)
                .filter(|markers| !markers.is_empty())
        })
        .unwrap_or_else(|| vec![String::from("login:")]);
    let mut interaction_fires = console
        .map(|plan| vec![0u8; plan.interaction.len()])
        .unwrap_or_default();
    let mut last_progress = Instant::now();
    let mut last_reported_len = 0usize;

    while start.elapsed() < timeout {
        ensure_qemu_running(qemu, "waiting for guest login prompt via CC-PD API")?;
        let mut drained_console = false;
        match cc_log_stream_for_handle(cc, guest_handle, profile) {
            Ok(chunk) => {
                if !chunk.is_empty() {
                    drained_console = true;
                    transcript.push_str(&chunk);
                    reject_profile_console(console, &transcript)?;
                    if last_progress.elapsed() >= Duration::from_secs(30) {
                        println!(
                            "[xtask:test] {} console progress ({} bytes), tail:\n{}",
                            guest_os,
                            transcript.len(),
                            tail_chars(&transcript, 800)
                        );
                        last_progress = Instant::now();
                        last_reported_len = transcript.len();
                    }
                    run_console_interactions(
                        console,
                        &transcript,
                        start.elapsed(),
                        &mut interaction_fires,
                        cc,
                        guest_handle,
                    )?;

                    let requirements_met = console.is_none_or(|plan| {
                        plan.require
                            .iter()
                            .all(|marker| transcript.contains(marker))
                    });
                    let matched = requirements_met
                        .then(|| {
                            prompt_markers
                                .iter()
                                .find(|marker| transcript.contains(marker.as_str()))
                                .cloned()
                        })
                        .flatten();

                    if let Some(marker) = matched {
                        matched_prompt = Some(marker);
                        break;
                    }
                }
            }
            Err(err) => {
                if cc.is_closed() {
                    return Err(err).context("CC console transport closed");
                }
                println!("[xtask:test] CC console drain not ready yet: {err:#}");
            }
        }
        run_console_interactions(
            console,
            &transcript,
            start.elapsed(),
            &mut interaction_fires,
            cc,
            guest_handle,
        )?;
        if transcript.len() > last_reported_len
            && last_progress.elapsed() >= Duration::from_secs(30)
        {
            println!(
                "[xtask:test] {} console idle after {} bytes, tail:\n{}",
                guest_os,
                transcript.len(),
                tail_chars(&transcript, 800)
            );
            last_progress = Instant::now();
            last_reported_len = transcript.len();
        }
        std::thread::sleep(if drained_console {
            Duration::from_millis(10)
        } else {
            Duration::from_millis(250)
        });
    }

    let prompt = matched_prompt.ok_or_else(|| {
        anyhow::anyhow!(
            "timed out after {}s waiting for {:?} via CC-PD console API; tail:\n{}",
            timeout.as_secs(),
            prompt_markers,
            tail_chars(&transcript, 4000)
        )
    })?;

    println!("[xtask:test] CC console matched prompt marker {:?}", prompt);

    let proof = verify_guest_console_input(
        cc_sock,
        cc,
        guest_handle,
        profile,
        timeout
            .saturating_sub(start.elapsed())
            .min(Duration::from_secs(
                console.map_or(20, |plan| plan.probe_timeout_secs),
            )),
        qemu,
    )?;
    if profile.is_some_and(|plan| plan.devices.iter().any(|device| device == "gpu")) {
        if let Some(plan) =
            profile.filter(|plan| plan.test.iter().any(|s| s.action == "assert-frame-pixels"))
        {
            // A console marker after fbdev write/sleep is not a display fence.
            // Observe the declared pixels while the guest can still complete
            // asynchronous GPU work, before freezing it for scanout comparison.
            let (attempts, sequence) = wait_frame_ready(
                || {
                    ensure_qemu_running(qemu, "waiting for expected frame pixels")?;
                    probe_guest_frame_pixels(cc, guest_handle, &plan.test)
                },
                timeout
                    .saturating_sub(start.elapsed())
                    .min(Duration::from_secs(30)),
                Duration::from_millis(250),
            )?;
            println!("[xtask:test] expected framebuffer pixels ready: probes={attempts}, sequence={sequence}");
        }
        if display_log.is_some() {
            suspend_guest_via_cc(cc, guest_handle)?;
            println!("[xtask:test] guest suspended for coherent framebuffer/scanout comparison");
        }
        let capture = (|| {
            let capture = capture_guest_frame(cc, guest_handle, cc_sock, profile)?;
            if let Some(log) = display_log {
                let expected = std::fs::read(cc_sock.with_extension("frame.ppm"))?;
                let receipt: serde_json::Value =
                    serde_json::from_slice(&std::fs::read(cc_sock.with_extension("frame.json"))?)?;
                let displayed = verify_display(
                    log,
                    &expected,
                    receipt["width"].as_u64().context("missing frame width")?,
                    receipt["height"].as_u64().context("missing frame height")?,
                )?;
                println!("[xtask:test] {displayed}");
            }
            Ok::<_, anyhow::Error>(capture)
        })();
        // Always attempt resume, including after a failed capture or comparison.
        let resumed = if display_log.is_some() {
            resume_guest_via_cc(cc, guest_handle).map(|_| ())
        } else {
            Ok(())
        };
        let capture = capture?;
        resumed?;
        println!("[xtask:test] {capture}");
    }
    Ok(format!(
        "CC console API saw {guest_os} handle {guest_handle} prompt {:?} and {proof}",
        prompt
    ))
}

fn verify_native_display(log: &Path) -> anyhow::Result<String> {
    let mut expected = b"P6\n40 40\n255\n".to_vec();
    for offset in (0..40u32 * 40 * 4).step_by(4) {
        for channel in [2, 1, 0] {
            expected.push(((offset + channel) * 37) as u8);
        }
    }
    verify_display(log, &expected, 40, 40)
}

fn verify_display(log: &Path, expected: &[u8], width: u64, height: u64) -> anyhow::Result<String> {
    anyhow::ensure!(
        width > 0 && width <= 1024 && height > 0 && height <= 768,
        "invalid display expectation dimensions"
    );
    let mut socket = UnixStream::connect(log.with_extension("display.qmp.sock"))?;
    socket.set_read_timeout(Some(Duration::from_secs(10)))?;
    socket.set_write_timeout(Some(Duration::from_secs(10)))?;
    // Bound both message size and asynchronous events; a broken QMP peer must
    // not turn a display assertion into an unbounded qualification wait.
    fn receive(socket: &mut UnixStream) -> anyhow::Result<serde_json::Value> {
        let mut bytes = Vec::new();
        loop {
            anyhow::ensure!(bytes.len() < 65536, "oversized QMP response");
            let mut byte = [0u8];
            socket.read_exact(&mut byte)?;
            if byte[0] == b'\n' {
                return Ok(serde_json::from_slice(&bytes)?);
            }
            bytes.push(byte[0]);
        }
    }
    anyhow::ensure!(
        receive(&mut socket)?.get("QMP").is_some(),
        "missing QMP greeting"
    );
    let ppm = log.with_extension("display.ppm");
    for (id, command) in [
        serde_json::json!({"execute":"qmp_capabilities"}),
        serde_json::json!({"execute":"screendump","arguments":{
            "filename":ppm,"device":"display0"}}),
    ]
    .into_iter()
    .enumerate()
    {
        let mut command = command;
        command["id"] = serde_json::json!(id);
        socket.write_all(serde_json::to_string(&command)?.as_bytes())?;
        socket.write_all(b"\n")?;
        let mut completed = false;
        for _ in 0..32 {
            let reply = receive(&mut socket)?;
            if reply.get("event").is_some() {
                continue;
            }
            anyhow::ensure!(
                reply["id"] == id && reply.get("return").is_some(),
                "QMP command failed: {reply}"
            );
            completed = true;
            break;
        }
        anyhow::ensure!(completed, "QMP event limit exceeded");
    }
    let actual = std::fs::read(&ppm)?;
    anyhow::ensure!(
        actual == expected,
        "QEMU display pixels differ from primary client framebuffer"
    );
    std::fs::write(
        log.with_extension("display.json"),
        serde_json::to_vec_pretty(
            &serde_json::json!({"width":width,"height":height,"client":0,
            "asserted_pixels":width*height,"sha256":sha256_bytes(&actual),
            "capture":"QMP screendump","path":ppm}),
        )?,
    )?;
    Ok(format!(
        "display: all {} QEMU scanout pixels match primary client",
        width * height
    ))
}

fn verify_native_frame_observer(
    socket: &Path,
    log: &Path,
    timeout: Duration,
    qemu: &mut Child,
) -> anyhow::Result<String> {
    wait_for_all_markers(log, &["[cc_pd] VirtIO serial ready"], timeout, qemu)?;
    let mut cc = connect_cc_client(socket, timeout, qemu)?;
    let mut request = [0u8; 32];
    wr32(&mut request, 0, 1);
    wr32(&mut request, 4, 1);
    let denied = cc.call(0x261d, 0, 0, 0, &request)?;
    anyhow::ensure!(
        denied.mr[0] == CC_ERR_BAD_HANDLE,
        "native observer accepted an unknown handle"
    );
    wr32(&mut request, 12, 1); // callers cannot select a private raw client slot
    let invalid = cc.call(0x261d, 0xfb000000, 0, 0, &request)?;
    anyhow::ensure!(
        invalid.mr[0] == 9,
        "native observer accepted a raw client slot"
    );
    for client in 0..2u32 {
        let artifact = socket.with_extension(format!("native-{client}.sock"));
        capture_guest_frame(&mut cc, 0xfb000000 + client, &artifact, None)?;
        let actual = std::fs::read(artifact.with_extension("frame.ppm"))?;
        let mut expected = b"P6\n40 40\n255\n".to_vec();
        for offset in (0..40u32 * 40 * 4).step_by(4) {
            for channel in [2, 1, 0] {
                expected.push(((offset + channel) * 37 + client * 83) as u8);
            }
        }
        anyhow::ensure!(
            actual == expected,
            "native observer client {client} pixel mismatch"
        );
    }
    Ok(
        "native observer: exact isolated client frames exported through CC in multiple chunks"
            .into(),
    )
}

fn verify_frame_pixels(
    pixels: &[u8],
    width: u32,
    height: u32,
    steps: &[cmd_guest_profile::RecipeStep],
) -> anyhow::Result<usize> {
    anyhow::ensure!(
        width > 0
            && width <= 1024
            && height > 0
            && height <= 768
            && pixels.len() == width as usize * height as usize * 4,
        "invalid captured frame buffer"
    );
    let mut checked = 0;
    for step in steps.iter().filter(|s| s.action == "assert-frame-pixels") {
        let (x, y, expected) = cmd_guest_profile::frame_pixel_expectation(step)?;
        anyhow::ensure!(
            y < height as usize && x + expected.len() <= width as usize,
            "expected pixel span lies outside captured frame"
        );
        for (i, rgb) in expected.iter().enumerate() {
            let offset = (y * width as usize + x + i) * 4;
            let actual = [pixels[offset + 2], pixels[offset + 1], pixels[offset]];
            anyhow::ensure!(
                actual == *rgb,
                "guest frame pixel ({}, {}) expected {:?}, got {:?}",
                x + i,
                y,
                rgb,
                actual
            );
            checked += 1;
        }
    }
    Ok(checked)
}

fn wait_frame_ready(
    mut probe: impl FnMut() -> anyhow::Result<(bool, u64)>,
    timeout: Duration,
    interval: Duration,
) -> anyhow::Result<(usize, u64)> {
    let started = Instant::now();
    let mut attempts = 0;
    let mut last_sequence = None;
    // Deadline is checked between bounded CC exchanges; neither timeout nor
    // malformed protocol data is converted into a retryable pixel mismatch.
    while attempts < 120 && started.elapsed() < timeout {
        attempts += 1;
        let (ready, sequence) = probe()?;
        last_sequence = Some(sequence);
        if ready {
            return Ok((attempts, sequence));
        }
        println!("[xtask:test] expected framebuffer pixels not ready: probe={attempts}, sequence={sequence}");
        std::thread::sleep(interval.min(timeout.saturating_sub(started.elapsed())));
    }
    anyhow::bail!("expected framebuffer pixels did not become ready after {attempts} probes; last sequence={last_sequence:?}, elapsed={:?}", started.elapsed())
}

fn probe_guest_frame_pixels(
    cc: &mut CcClient,
    handle: u32,
    steps: &[cmd_guest_profile::RecipeStep],
) -> anyhow::Result<(bool, u64)> {
    let mut request = [0u8; 32];
    wr32(&mut request, 0, 1);
    wr32(&mut request, 4, 1);
    let snapshot = cc.call(0x261d, handle, 0, 0, &request)?;
    // NO_FRAME is a valid not-ready result before the first completed flip.
    // Accept only its exact empty response shape; all other errors propagate.
    if snapshot.mr == [CC_OK, 40, 3, 1]
        && rd32(&snapshot.shmem, 0) == 1
        && rd32(&snapshot.shmem, 4) == 3
        && snapshot.shmem[8..40].iter().all(|byte| *byte == 0)
    {
        return Ok((false, 0));
    }
    let identity = decode_frame_reply(&snapshot, 0)?;
    let (cookie, sequence, width, height) = identity;
    anyhow::ensure!(
        cookie != 0 && sequence != 0,
        "frame readiness has no committed snapshot"
    );
    request[16..24].copy_from_slice(&cookie.to_le_bytes());
    let checked = (|| -> anyhow::Result<bool> {
        let mut ready = true;
        let mut checked_pixels = 0;
        for step in steps.iter().filter(|s| s.action == "assert-frame-pixels") {
            let (x, y, expected) = cmd_guest_profile::frame_pixel_expectation(step)?;
            anyhow::ensure!(
                y < height as usize && x + expected.len() <= width as usize,
                "expected pixel span lies outside readiness snapshot"
            );
            let start = (y * width as usize + x) * 4;
            for (chunk_index, pixels) in expected.chunks((CC_WIRE_SHMEM_SIZE - 40) / 4).enumerate()
            {
                let offset = start + chunk_index * ((CC_WIRE_SHMEM_SIZE - 40) / 4) * 4;
                wr32(&mut request, 4, 2);
                wr32(&mut request, 24, offset as u32);
                wr32(&mut request, 28, (pixels.len() * 4) as u32);
                let reply = cc.call(0x261d, 0, 0, 0, &request)?;
                anyhow::ensure!(
                    decode_frame_reply(&reply, pixels.len() * 4)? == identity,
                    "frame readiness snapshot changed during read"
                );
                for (rgb, bytes) in pixels.iter().zip(reply.shmem[40..].chunks_exact(4)) {
                    ready &= *rgb == [bytes[2], bytes[1], bytes[0]];
                    checked_pixels += 1;
                }
            }
        }
        anyhow::ensure!(
            checked_pixels > 0,
            "frame readiness requires declared pixels"
        );
        Ok(ready)
    })();
    wr32(&mut request, 4, 3);
    wr32(&mut request, 24, 0);
    wr32(&mut request, 28, 0);
    let released = cc.call(0x261d, 0, 0, 0, &request);
    let ready = checked?;
    anyhow::ensure!(
        decode_frame_reply(&released?, 0)? == (0, sequence, width, height),
        "frame readiness snapshot release failed"
    );
    Ok((ready, sequence))
}

fn capture_guest_frame(
    cc: &mut CcClient,
    handle: u32,
    socket: &Path,
    profile: Option<&HostProfilePlan>,
) -> anyhow::Result<String> {
    const OPCODE: u32 = 0x261d;
    const HEADER: usize = 40;
    let started = Instant::now();
    println!("[xtask:test] requesting guest framebuffer snapshot for handle {handle}");
    let mut request = [0u8; 32];
    wr32(&mut request, 0, 1);
    wr32(&mut request, 4, 1); // CAPTURE
    let snapshot = cc.call(OPCODE, handle, 0, 0, &request)?;
    let (cookie, sequence, width, height) = decode_frame_reply(&snapshot, 0)?;
    anyhow::ensure!(
        cookie != 0 && sequence != 0,
        "frame has no committed snapshot"
    );
    request[16..24].copy_from_slice(&cookie.to_le_bytes());
    let captured = (|| -> anyhow::Result<Vec<u8>> {
        let bytes = width as usize * height as usize * 4;
        println!("[xtask:test] reading immutable framebuffer: {width}x{height}, sequence={sequence}, bytes={bytes}");
        let mut pixels = Vec::with_capacity(bytes);
        let mut next_report = 0x40000;
        wr32(&mut request, 4, 2); // READ
        while pixels.len() < bytes {
            let length = (bytes - pixels.len()).min(CC_WIRE_SHMEM_SIZE - HEADER);
            wr32(&mut request, 24, pixels.len() as u32);
            wr32(&mut request, 28, length as u32);
            let reply = cc.call(OPCODE, 0, 0, 0, &request)?;
            anyhow::ensure!(
                decode_frame_reply(&reply, length)? == (cookie, sequence, width, height),
                "frame snapshot changed during chunked read"
            );
            pixels.extend_from_slice(&reply.shmem[HEADER..HEADER + length]);
            if pixels.len() >= next_report || pixels.len() == bytes {
                println!(
                    "[xtask:test] framebuffer capture: {}/{bytes} bytes in {}s",
                    pixels.len(),
                    started.elapsed().as_secs()
                );
                next_report = pixels.len() + 0x40000;
            }
        }
        anyhow::ensure!(
            pixels
                .chunks_exact(4)
                .any(|pixel| pixel[..3].iter().any(|byte| *byte != 0)),
            "guest frame has no nonblack RGB pixels"
        );
        Ok(pixels)
    })();
    wr32(&mut request, 4, 3); // RELEASE, including after a failed read
    wr32(&mut request, 24, 0);
    wr32(&mut request, 28, 0);
    let released = cc.call(OPCODE, 0, 0, 0, &request);
    let pixels = captured?;
    anyhow::ensure!(
        decode_frame_reply(&released?, 0)?.0 == 0,
        "snapshot release failed"
    );
    let checked_pixels = verify_frame_pixels(
        &pixels,
        width,
        height,
        profile.map_or(&[], |p| p.test.as_slice()),
    )?;
    let mut ppm = format!("P6\n{width} {height}\n255\n").into_bytes();
    for pixel in pixels.chunks_exact(4) {
        ppm.extend_from_slice(&[pixel[2], pixel[1], pixel[0]]);
    }
    let path = socket.with_extension("frame.ppm");
    std::fs::write(&path, &ppm)?;
    let digest = format!("{:x}", Sha256::digest(&ppm));
    std::fs::write(
        socket.with_extension("frame.json"),
        serde_json::to_vec_pretty(
            &serde_json::json!({"version":1,"guest_handle":handle,"width":width,
            "height":height,"sequence":sequence,"format":"P6 RGB888",
            "bytes":ppm.len(),"sha256":digest,"asserted_pixels":checked_pixels}),
        )?,
    )?;
    Ok(format!(
        "guest framebuffer captured: {}x{}, sequence={}, sha256={}, path={}",
        width,
        height,
        sequence,
        digest,
        path.display()
    ))
}

fn decode_frame_reply(reply: &CcReply, length: usize) -> anyhow::Result<(u64, u64, u32, u32)> {
    anyhow::ensure!(
        length <= CC_WIRE_SHMEM_SIZE - 40 && reply.shmem.len() >= 40 + length,
        "invalid framebuffer response length"
    );
    anyhow::ensure!(
        reply.mr == [CC_OK, (40 + length) as u32, 0, 1]
            && rd32(&reply.shmem, 0) == 1
            && rd32(&reply.shmem, 4) == 0
            && rd32(&reply.shmem, 8) == 0
            && rd32(&reply.shmem, 12) == length as u32,
        "framebuffer service rejected capture or returned a malformed response: {:?}",
        reply.mr
    );
    let width = rd32(&reply.shmem, 32);
    let height = rd32(&reply.shmem, 36);
    anyhow::ensure!(
        width > 0 && width <= 1024 && height > 0 && height <= 768,
        "invalid framebuffer dimensions"
    );
    Ok((
        u64::from_le_bytes(reply.shmem[16..24].try_into()?),
        u64::from_le_bytes(reply.shmem[24..32].try_into()?),
        width,
        height,
    ))
}

fn marker_occurrences(transcript: &str, marker: &str) -> usize {
    transcript.match_indices(marker).count()
}

fn available_console_interactions(
    interaction: &crate::cmd_guest_profile::ConsoleInteractionPlan,
    transcript: &str,
    elapsed: Duration,
) -> usize {
    if elapsed < Duration::from_secs(interaction.after_secs) {
        return 0;
    }
    if interaction.when.is_empty() {
        return 1;
    }
    interaction
        .when
        .iter()
        .map(|marker| marker_occurrences(transcript, marker))
        .min()
        .unwrap_or(0)
}

fn run_console_interactions(
    console: Option<&crate::cmd_guest_profile::ConsolePlan>,
    transcript: &str,
    elapsed: Duration,
    fires: &mut [u8],
    cc: &mut CcClient,
    guest_handle: u32,
) -> anyhow::Result<()> {
    let Some(console) = console else {
        return Ok(());
    };
    for (index, interaction) in console.interaction.iter().enumerate() {
        if fires[index] >= interaction.max_fires {
            continue;
        }
        let available = available_console_interactions(interaction, transcript, elapsed);
        if available <= usize::from(fires[index]) {
            continue;
        }
        cc_send_raw_bytes(cc, guest_handle, interaction.send.as_bytes())?;
        fires[index] += 1;
        println!(
            "[xtask:test] console rule {} fired ({}/{})",
            index + 1,
            fires[index],
            interaction.max_fires
        );
    }
    Ok(())
}

fn profile_console_markers(profile: &HostProfilePlan) -> Vec<String> {
    profile
        .test
        .iter()
        .filter(|step| matches!(step.action.as_str(), "wait-console" | "assert-console"))
        .filter_map(|step| step.args.get("marker").cloned())
        .collect()
}

fn reject_profile_console(
    console: Option<&crate::cmd_guest_profile::ConsolePlan>,
    transcript: &str,
) -> anyhow::Result<()> {
    if let Some(marker) = console
        .into_iter()
        .flat_map(|plan| &plan.reject)
        .find(|marker| transcript.contains(marker.as_str()))
    {
        anyhow::bail!(
            "guest console reached profile rejection marker {marker:?}; tail:\n{}",
            tail_chars(transcript, 4000)
        );
    }
    Ok(())
}

const CONSOLE_STRESS_BYTES: usize = 0x40000;
const CONSOLE_BACKPRESSURE_MARKER: &str =
    "VIRTIO(CONSOLE): TX backpressure retained pending descriptor";

fn verify_console_stress_stream(stream: &[u8]) -> anyhow::Result<String> {
    use sha2::{Digest, Sha256};
    let begin = b"AOS_STRESS_BEGIN";
    let end = b"AOS_STRESS_END";
    let start = stream
        .windows(begin.len())
        .position(|value| value == begin)
        .context("stress stream has no begin marker")?
        + begin.len();
    let newline = stream[start..]
        .iter()
        .position(|value| *value == b'\n')
        .context("stress begin marker has no newline")?;
    anyhow::ensure!(
        newline <= 1 && (newline == 0 || stream[start] == b'\r'),
        "unexpected bytes after stress begin marker"
    );
    let payload = &stream[start + newline + 1..];
    let end_offset = payload
        .windows(end.len())
        .position(|value| value == end)
        .context("stress stream has no end marker")?;
    anyhow::ensure!(
        end_offset >= CONSOLE_STRESS_BYTES,
        "stress payload was truncated"
    );
    let separator = &payload[CONSOLE_STRESS_BYTES..end_offset];
    anyhow::ensure!(
        separator == b"\n" || separator == b"\r\n",
        "stress payload has unexpected length or trailing bytes"
    );
    let payload = &payload[..CONSOLE_STRESS_BYTES];
    for (index, value) in payload.iter().enumerate() {
        let expected = b'A' + ((index ^ (index >> 8) ^ (index >> 16)) & 15) as u8;
        anyhow::ensure!(*value == expected, "stress byte mismatch at offset {index}");
    }
    Ok(format!("{:x}", Sha256::digest(payload)))
}

fn verify_console_backpressure(
    cc_sock: &Path,
    log_path: &Path,
    profile: Option<&HostProfilePlan>,
    timeout: Duration,
    qemu: &mut Child,
) -> anyhow::Result<String> {
    let mut cc = connect_cc_client(cc_sock, Duration::from_secs(30), qemu)?;
    anyhow::ensure!(
        !std::fs::read_to_string(log_path)?.contains(CONSOLE_BACKPRESSURE_MARKER),
        "console was already backpressured before the deliberate stress trigger"
    );
    cc_send_console_line(&mut cc, 0, b"!")?;
    println!("[xtask:test] Console drain paused until actual backend backpressure is observed");
    wait_for_all_markers(
        log_path,
        &[CONSOLE_BACKPRESSURE_MARKER],
        timeout.min(Duration::from_secs(30)),
        qemu,
    )?;
    println!("[xtask:test] Backend full with pending descriptor; resuming console drain");
    let start = Instant::now();
    let mut stream = Vec::new();
    while start.elapsed() < timeout {
        ensure_qemu_running(qemu, "draining the backpressured console stream")?;
        let chunk = cc_log_stream_for_handle(&mut cc, 0, profile)?;
        stream.extend_from_slice(chunk.as_bytes());
        anyhow::ensure!(
            stream.len() <= CONSOLE_STRESS_BYTES + 65536,
            "stress console exceeded its bounded receive buffer"
        );
        if stream
            .windows(b"AOS_STRESS_END".len())
            .any(|value| value == b"AOS_STRESS_END")
        {
            let checksum = verify_console_stress_stream(&stream)?;
            return Ok(format!(
                "console backpressure: {} exact bytes recovered, sha256={checksum}",
                CONSOLE_STRESS_BYTES
            ));
        }
        std::thread::sleep(Duration::from_millis(if chunk.is_empty() { 20 } else { 1 }));
    }
    anyhow::bail!(
        "backpressured console did not complete; received {} bytes",
        stream.len()
    )
}

fn verify_guest_console_input(
    _cc_sock: &Path,
    cc: &mut CcClient,
    guest_handle: u32,
    profile: Option<&HostProfilePlan>,
    timeout: Duration,
    qemu: &mut Child,
) -> anyhow::Result<String> {
    let console = profile.map(|value| &value.console);
    let (probe, marker, line_mode) = console
        .and_then(|plan| plan.probe_line.as_ref().zip(plan.probe_marker.as_ref()))
        .map(|(probe, marker)| (probe.as_str(), marker.as_str(), true))
        .unwrap_or(("~", "~", false));
    if line_mode {
        println!("[xtask:test] sending console probe ({} bytes)", probe.len());
        cc_send_console_line(cc, guest_handle, probe.as_bytes())?;
    } else {
        cc_send_raw_bytes(cc, guest_handle, probe.as_bytes())?;
    }

    let mut echo = String::new();
    let start = Instant::now();
    let mut last_report = start;
    println!("[xtask:test] console probe sent; waiting for marker {marker:?}");
    while start.elapsed() < timeout {
        ensure_qemu_running(qemu, "waiting for guest console input echo via CC-PD API")?;
        let chunk = match cc_log_stream_for_handle(cc, guest_handle, profile) {
            Ok(chunk) => chunk,
            Err(err) => {
                if cc.is_closed() {
                    return Err(err).context("CC console transport closed after input");
                }
                println!("[xtask:test] CC console post-input drain not ready yet: {err:#}");
                String::new()
            }
        };
        if !chunk.is_empty() {
            echo.push_str(&chunk);
            if echo.contains(marker) {
                if !line_mode {
                    let _ = cc_send_raw_byte(cc, guest_handle, 0x15); /* Ctrl-U */
                }
                return Ok(format!("guest completed console probe {marker:?}"));
            }
        }
        if last_report.elapsed() >= Duration::from_secs(30) {
            println!(
                "[xtask:test] console probe waiting {}s; received {} bytes; tail:\n{}",
                start.elapsed().as_secs(),
                echo.len(),
                tail_chars(&echo, 800)
            );
            last_report = Instant::now();
        }
        std::thread::sleep(Duration::from_millis(500));
    }

    anyhow::bail!(
        "guest reached prompt, but console probe {:?} did not emit {:?}; post-input tail:\n{}",
        probe,
        marker,
        tail_chars(&echo, 2000)
    );
}

fn try_create_guest_via_cc(
    cc: &mut CcClient,
    os_type: u8,
    arch: u8,
    ram_mb: u32,
) -> anyhow::Result<Result<u32, (u32, u32)>> {
    let mut shmem = [0u8; 52];
    shmem[0] = os_type;
    shmem[1] = arch;
    wr32(&mut shmem, 4, ram_mb);
    wr32(
        &mut shmem,
        16,
        VIBEOS_DEV_SERIAL | VIBEOS_DEV_NET | VIBEOS_DEV_BLOCK,
    );

    let reply = cc
        .call(MSG_CC_CREATE_GUEST, 0, 0, 0, &shmem)
        .context("MSG_CC_CREATE_GUEST failed")?;
    if reply.mr[0] == CC_OK {
        Ok(Ok(reply.mr[1]))
    } else {
        Ok(Err((reply.mr[0], reply.mr[1])))
    }
}

fn create_guest_via_cc_wait(
    cc: &mut CcClient,
    os_type: u8,
    arch: u8,
    ram_mb: u32,
    label: &str,
    timeout: Duration,
    qemu: &mut Child,
) -> anyhow::Result<u32> {
    let start = Instant::now();
    let mut attempts = 0u32;
    let mut last_err = String::from("no create attempt completed");

    while start.elapsed() < timeout {
        attempts += 1;
        ensure_qemu_running(qemu, &format!("creating {label} guest through CC-PD"))?;

        match try_create_guest_via_cc(cc, os_type, arch, ram_mb) {
            Ok(Ok(handle)) => {
                println!(
                    "[xtask:test] created {label} guest handle={handle} after {attempts} attempt(s)"
                );
                return Ok(handle);
            }
            Ok(Err((status, detail))) => {
                last_err = format!("MSG_CC_CREATE_GUEST returned ok={status} detail={detail}");
                if status != CC_ERR_RELAY_FAULT {
                    anyhow::bail!("failed to create {label} guest: {last_err}");
                }
                if attempts == 1 || attempts % 5 == 0 {
                    println!("[xtask:test] {label} guest create not ready yet: {last_err}");
                }
            }
            Err(err) => {
                last_err = format!("{err:#}");
                if cc.is_closed() {
                    return Err(err).context(format!("{label} guest create transport closed"));
                }
                if attempts == 1 || attempts % 5 == 0 {
                    println!(
                        "[xtask:test] {label} guest create transport not ready yet: {last_err}"
                    );
                }
            }
        }

        std::thread::sleep(Duration::from_secs(1));
    }

    anyhow::bail!(
        "timed out after {}s creating {label} guest through CC-PD/vm_manager; last error: {last_err}",
        timeout.as_secs()
    );
}

fn run_guest_console_command(
    _cc_sock: &Path,
    cc: &mut CcClient,
    guest_handle: u32,
    guest_os: &str,
    profile: Option<&HostProfilePlan>,
    command: &str,
    marker: &str,
    timeout: Duration,
    qemu: &mut Child,
) -> anyhow::Result<()> {
    /*
     * Do not issue a speculative log drain here.  Immediately after a guest
     * handoff that extra synchronous RPC can consume the entire frame deadline
     * and close the retained CC session before any input is sent.  Each command
     * has a fresh success marker, so stale console output cannot satisfy it.
     */
    cc_send_console_line(cc, guest_handle, command.as_bytes())?;

    let start = Instant::now();
    let mut output = String::new();
    let failure_marker = format!("agentos-{guest_os}-ssh-failed");
    while start.elapsed() < timeout {
        ensure_qemu_running(qemu, "waiting for guest provisioning command")?;
        match cc_log_stream_for_handle(cc, guest_handle, profile) {
            Ok(chunk) => {
                output.push_str(&chunk);
                if output.contains(marker) {
                    return Ok(());
                }
                if output.contains(&failure_marker) {
                    anyhow::bail!(
                        "{guest_os} SSH provisioning reported failure; tail:\n{}",
                        tail_chars(&output, 3000)
                    );
                }
            }
            Err(err) => {
                if cc.is_closed() {
                    return Err(err).context("CC provisioning transport closed");
                }
                println!("[xtask:test] CC provisioning poll not ready yet: {err:#}");
            }
        }
        std::thread::sleep(Duration::from_millis(250));
    }
    anyhow::bail!(
        "{guest_os} SSH provisioning command did not emit {marker:?}; tail:\n{}",
        tail_chars(&output, 3000)
    );
}

fn run_guest_console_commands(
    cc_sock: &Path,
    cc: &mut CcClient,
    guest_handle: u32,
    guest_os: &str,
    profile: Option<&HostProfilePlan>,
    commands: &[String],
    timeout: Duration,
    qemu: &mut Child,
) -> anyhow::Result<()> {
    anyhow::ensure!(
        !commands.is_empty(),
        "guest provisioning command list is empty"
    );
    for (index, command) in commands.iter().enumerate() {
        println!(
            "[xtask:test] provisioning {guest_os} over CC console (step {}/{})",
            index + 1,
            commands.len()
        );
        let last = index + 1 == commands.len();
        let success = if last {
            "ready".to_string()
        } else {
            format!("step-{}", index + 1)
        };
        let marker = format!("agentos-{guest_os}-ssh-{success}");
        let wrapped = guest_provision_command(command, guest_os, &success);
        run_guest_console_command(
            cc_sock,
            cc,
            guest_handle,
            guest_os,
            profile,
            &wrapped,
            &marker,
            timeout,
            qemu,
        )?;
    }
    Ok(())
}

fn guest_provision_command(command: &str, guest_os: &str, success: &str) -> String {
    format!(
        "({command}) && printf 'agentos-{guest_os}-ssh-%s\\n' '{success}' || printf 'agentos-{guest_os}-ssh-%s\\n' failed"
    )
}

fn profile_provision_commands(
    profile: &HostProfilePlan,
    public_key: &str,
) -> anyhow::Result<Vec<String>> {
    anyhow::ensure!(
        !public_key
            .chars()
            .any(|ch| matches!(ch, '\n' | '\r' | '\'')),
        "SSH public key contains shell-hostile characters"
    );
    let commands: Vec<String> = profile
        .provision
        .iter()
        .filter(|step| step.action == "send-console")
        .map(|step| step.args["text"].replace("{{ssh_public_key}}", public_key))
        .collect();
    anyhow::ensure!(
        !commands.is_empty(),
        "profile {} has no send-console provisioning recipe",
        profile.id
    );
    anyhow::ensure!(
        commands.iter().all(|command| !command.contains("{{")),
        "profile {} provisioning contains an unknown template variable",
        profile.id
    );
    Ok(commands)
}

fn run_ssh_script(
    private_key: &Path,
    port: u16,
    account: &str,
    timeout: Duration,
    script: &str,
) -> anyhow::Result<String> {
    let remote_shell = if account == "root" {
        format!("timeout {} sh -s", timeout.as_secs())
    } else {
        format!("sudo -n timeout {} sh -s", timeout.as_secs())
    };
    let mut child = std::process::Command::new("ssh");
    child
        .arg("-i")
        .arg(private_key)
        .args(["-p", &port.to_string()])
        .args(SSH_AUTH_OPTIONS)
        .args(SSH_SESSION_LIVENESS_OPTIONS)
        .args([&format!("{account}@127.0.0.1"), &remote_shell])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    let mut child = child
        .spawn()
        .context("failed to launch profile desktop provisioning over SSH")?;
    child
        .stdin
        .take()
        .context("desktop provisioning SSH stdin was not piped")?
        .write_all(script.as_bytes())
        .context("failed to send profile desktop provisioning script")?;
    let output = child
        .wait_with_output()
        .context("failed to wait for profile desktop provisioning")?;
    anyhow::ensure!(
        output.status.success(),
        "profile desktop provisioning failed with {}: stdout={:?} stderr={:?}",
        output.status,
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    );
    Ok(String::from_utf8_lossy(&output.stdout).into_owned())
}

fn render_desktop_script(script: &str, host_unix_time: u64) -> anyhow::Result<String> {
    let rendered = script.replace("{{host_unix_time}}", &host_unix_time.to_string());
    anyhow::ensure!(
        !rendered.contains("{{"),
        "desktop provisioning contains an unknown template variable"
    );
    Ok(rendered)
}

fn wait_for_profile_ssh(
    profile: &HostProfilePlan,
    ssh_key: &SshTestKey,
    timeout: Duration,
    qemu: &mut Child,
) -> anyhow::Result<()> {
    let ssh = profile
        .qemu
        .as_ref()
        .and_then(|qemu| qemu.ssh.as_ref())
        .context("profile has no host.qemu.ssh plan")?;
    let (account, marker) = profile_ssh_expectation(profile)?;
    let start = Instant::now();
    let mut last = String::from("no SSH attempt completed");
    while start.elapsed() < timeout {
        ensure_qemu_running(qemu, "waiting for profile SSH")?;
        let probe = spawn_ssh_probe(&ssh_key.private_key, ssh.host_port, account, None)?;
        let output = wait_ssh_probe(probe, timeout.saturating_sub(start.elapsed()))?;
        if output.status.success()
            && (marker.is_empty() || String::from_utf8_lossy(&output.stdout).trim() == marker)
        {
            println!(
                "[xtask:test] {} SSH remote exec uname -s: status={} stdout={:?} stderr={:?}",
                profile.id,
                output.status,
                String::from_utf8_lossy(&output.stdout),
                String::from_utf8_lossy(&output.stderr)
            );
            return Ok(());
        }
        last = format!(
            "status={} stdout={:?} stderr={:?}",
            output.status,
            String::from_utf8_lossy(&output.stdout).trim(),
            String::from_utf8_lossy(&output.stderr).trim()
        );
        std::thread::sleep(Duration::from_secs(2));
    }
    anyhow::bail!("profile SSH did not become ready: {last}")
}

fn prove_profile_ssh(
    cc_sock: &Path,
    profile: &HostProfilePlan,
    ssh_key: &SshTestKey,
    timeout: Duration,
    qemu: &mut Child,
) -> anyhow::Result<Instant> {
    let mut cc = connect_cc_client(cc_sock, timeout.min(Duration::from_secs(30)), qemu)?;
    let provision_ssh = profile_provision_commands(profile, &ssh_key.public_key)?;
    run_guest_console_commands(
        cc_sock,
        &mut cc,
        0,
        &profile.id,
        Some(profile),
        &provision_ssh,
        timeout.min(Duration::from_secs(600)),
        qemu,
    )?;
    drop(cc);
    wait_for_profile_ssh(
        profile,
        ssh_key,
        timeout.min(Duration::from_secs(600)),
        qemu,
    )?;
    let ssh_ready = Instant::now();
    let ssh = profile.qemu.as_ref().and_then(|q| q.ssh.as_ref()).unwrap();
    crate::guest_session::prove(
        profile,
        &ssh_key.private_key,
        ssh.host_port,
        None,
        timeout.min(Duration::from_secs(600)),
    )?;
    Ok(ssh_ready)
}

fn wait_qualification_child(
    child: &mut Child,
    qemu: &mut Child,
    seconds: u64,
) -> anyhow::Result<()> {
    let deadline = Instant::now() + Duration::from_secs(seconds);
    loop {
        anyhow::ensure!(
            qemu.try_wait()?.is_none(),
            "QEMU exited during qualification"
        );
        if let Some(status) = child.try_wait()? {
            anyhow::ensure!(status.success(), "qualification process failed: {status}");
            return Ok(());
        }
        anyhow::ensure!(
            Instant::now() < deadline,
            "qualification process deadline expired"
        );
        std::thread::sleep(Duration::from_millis(100));
    }
}

// Bound both the number and length of guest-controlled output lines.
fn input_probe_line(reader: &mut impl Read) -> anyhow::Result<String> {
    let mut line = Vec::new();
    for _ in 0..128 {
        let mut byte = [0];
        reader
            .read_exact(&mut byte)
            .context("input probe output ended early")?;
        if byte[0] == b'\n' {
            return String::from_utf8(line).context("input probe output is not UTF-8");
        }
        line.push(byte[0]);
    }
    anyhow::bail!("input probe output line exceeds 128 bytes")
}

fn expect_input_probe_line(
    receive: &std::sync::mpsc::Receiver<anyhow::Result<String>>,
    probe: &mut Child,
    stderr_path: &Path,
    expected: &str,
    seconds: u64,
) -> anyhow::Result<()> {
    let result = receive
        .recv_timeout(Duration::from_secs(seconds))
        .map_err(anyhow::Error::from)
        .and_then(|line| line);
    let failure = match result {
        Ok(line) if line == expected => return Ok(()),
        Ok(line) => format!("expected {expected:?}, received {line:?}"),
        Err(error) => format!("waiting for {expected:?}: {error:#}"),
    };
    // EOF can reach the reader just before ssh exits. Retain its actual status
    // before ChildGuard cleanup otherwise replaces the useful failure context.
    let deadline = Instant::now() + Duration::from_secs(1);
    let status = loop {
        let status = probe.try_wait()?;
        if status.is_some() || Instant::now() >= deadline {
            break status;
        }
        std::thread::sleep(Duration::from_millis(20));
    };
    let mut stderr = Vec::new();
    std::fs::File::open(stderr_path)?
        .take(2048)
        .read_to_end(&mut stderr)?;
    let status = status
        .map(|value| value.to_string())
        .unwrap_or_else(|| "still running".into());
    anyhow::bail!(
        "input probe {failure}; ssh process status={status}; stderr prefix={:?}; full stderr: {}",
        String::from_utf8_lossy(&stderr),
        stderr_path.display()
    );
}

fn prove_profile_input(
    repo: &Path,
    socket: &Path,
    log: &Path,
    helper: &Path,
    profile: &HostProfilePlan,
    key: &SshTestKey,
    qemu: &mut Child,
    handle: u32,
) -> anyhow::Result<String> {
    for mode in [
        InputProofMode::Events,
        InputProofMode::Release,
        InputProofMode::Backpressure,
        InputProofMode::Disconnect,
        InputProofMode::PausedDisconnect,
    ] {
        prove_profile_input_pass(repo, socket, log, helper, profile, key, qemu, mode, handle)?;
    }
    Ok(
        "exact guest input batches, held-state releases and paused-guest backpressure passed"
            .into(),
    )
}

#[derive(Clone, Copy, PartialEq)]
enum InputProofMode {
    Events,
    Release,
    Backpressure,
    Disconnect,
    PausedDisconnect,
}

impl InputProofMode {
    fn name(self) -> &'static str {
        match self {
            Self::Events => "explicit-events",
            Self::Release => "server-held-state",
            Self::Backpressure => "paused-backpressure",
            Self::Disconnect => "connection-close",
            Self::PausedDisconnect => "paused-connection-close",
        }
    }
    fn stem(self) -> &'static str {
        match self {
            Self::Events => "input",
            Self::Release => "input-release",
            Self::Backpressure => "input-backpressure",
            Self::Disconnect => "input-disconnect",
            Self::PausedDisconnect => "input-paused-disconnect",
        }
    }
    fn paused(self) -> bool {
        matches!(self, Self::Backpressure | Self::PausedDisconnect)
    }
}

// Keep a single public binary CC connection for each input session.
// A transport error or malformed reply must never count as queue backpressure.
fn input_session_result(
    cc: &mut CcClient,
    qemu: &mut Child,
    release: bool,
    device: &str,
    events: &[&str],
    handle: u32,
) -> anyhow::Result<u32> {
    ensure_qemu_running(qemu, "submitting guest input")?;
    let query = input_session_request(release, device, events)?;
    let reply = cc.call(0x261e, handle, 0, 0, &query)?;
    validate_input_session_reply(&reply, &query)
}

fn input_session_request(
    release: bool,
    device: &str,
    events: &[&str],
) -> anyhow::Result<[u8; 544]> {
    anyhow::ensure!(
        events.len() % 3 == 0 && events.len() / 3 < 64,
        "invalid event count"
    );
    anyhow::ensure!(
        !release || events.is_empty(),
        "release must not contain events"
    );
    anyhow::ensure!(
        release || !events.is_empty(),
        "input batch must contain events"
    );
    let device = match device {
        "keyboard" => 0,
        "pointer" => 1,
        _ => anyhow::bail!("unknown input device"),
    };
    let mut query = [0u8; 544];
    wr32(&mut query, 0, if release { 2 } else { 1 });
    wr32(&mut query, 12, device);
    wr32(
        &mut query,
        16,
        if release {
            0
        } else {
            events.len() as u32 / 3 + 1
        },
    );
    for (i, event) in events.chunks_exact(3).enumerate() {
        let kind: u16 = event[0].parse()?;
        let code: u16 = event[1].parse()?;
        let value: i32 = event[2].parse()?;
        anyhow::ensure!(kind != 0, "SYN_REPORT is appended by the harness");
        query[32 + i * 8..34 + i * 8].copy_from_slice(&kind.to_le_bytes());
        query[34 + i * 8..36 + i * 8].copy_from_slice(&code.to_le_bytes());
        wr32(&mut query, 36 + i * 8, value as u32);
    }
    Ok(query)
}

fn validate_input_session_reply(reply: &CcReply, query: &[u8; 544]) -> anyhow::Result<u32> {
    anyhow::ensure!(reply.shmem.len() >= 16, "truncated input response");
    let version = rd32(query, 0);
    let status = rd32(&reply.shmem, 8);
    let accepted = rd32(&reply.shmem, 12);
    anyhow::ensure!(
        reply.mr == [CC_OK, 16, status, version]
            && rd32(&reply.shmem, 0) == version
            && rd32(&reply.shmem, 4) == 0,
        "invalid input response identity"
    );
    anyhow::ensure!(
        status == 0 || status == 3,
        "unexpected input status {status}"
    );
    let expected = if status == 0 { rd32(query, 16) } else { 0 };
    anyhow::ensure!(accepted == expected, "input accepted count mismatch");
    Ok(status)
}

fn prove_paused_input_release(
    socket: &Path,
    qemu: &mut Child,
    disconnect: bool,
    handle: u32,
) -> anyhow::Result<[usize; 2]> {
    let mut cc = CcClient::connect(socket)?;
    suspend_guest_via_cc(&mut cc, handle)?;
    let result = (|| {
        let mut accepted = [0usize; 2];
        for (device, (name, code, rejected_code)) in
            [("keyboard", "183", "184"), ("pointer", "272", "273")]
                .iter()
                .enumerate()
        {
            // Duplicate presses fill transport queues but Linux filters them
            // to a single down event. This avoids overflowing evdev itself
            // when the guest resumes, and leaves one held key/button.
            for repeats in [63usize, 1] {
                let events: Vec<&str> = (0..repeats).flat_map(|_| ["1", *code, "1"]).collect();
                let mut blocked = false;
                for _ in 0..64 {
                    if input_session_result(&mut cc, qemu, false, name, &events, handle)? == 3 {
                        blocked = true;
                        break;
                    }
                    accepted[device] += 1;
                }
                anyhow::ensure!(blocked, "paused {name} queue never applied backpressure");
            }
            anyhow::ensure!(accepted[device] > 0, "{name} accepted no held-state input");
            for _ in 0..if disconnect { 0 } else { 2 } {
                anyhow::ensure!(
                    input_session_result(&mut cc, qemu, true, name, &[], handle)? == 0,
                    "{name} release was not retained"
                );
            }
            anyhow::ensure!(
                input_session_result(
                    &mut cc,
                    qemu,
                    false,
                    name,
                    &["1", rejected_code, "1"],
                    handle
                )? == 3,
                "new {name} input bypassed pending release"
            );
        }
        if disconnect {
            // EOF is the only release trigger. Reconnect while still paused:
            // the new generation must not erase releases retained by input_virt.
            drop(cc.stream.take());
            cc = CcClient::connect(socket)?;
            for (name, code) in [("keyboard", "184"), ("pointer", "273")] {
                anyhow::ensure!(
                    input_session_result(&mut cc, qemu, false, name, &["1", code, "1"], handle)?
                        == 3,
                    "reconnect bypassed pending {name} release"
                );
            }
        }
        Ok(accepted)
    })();
    // Resume even after a failed assertion, before awaiting guest SSH cleanup.
    let resumed = (|| {
        if cc.stream.is_none() {
            cc = CcClient::connect(socket)?;
        }
        resume_guest_via_cc(&mut cc, handle)
    })();
    if let Err(error) = &resumed {
        eprintln!("[xtask:test] paused input cleanup could not resume guest: {error:#}");
    }
    let accepted = result?;
    resumed?;
    Ok(accepted)
}

fn prove_profile_input_pass(
    repo: &Path,
    socket: &Path,
    log: &Path,
    helper: &Path,
    profile: &HostProfilePlan,
    key: &SshTestKey,
    qemu: &mut Child,
    mode: InputProofMode,
    handle: u32,
) -> anyhow::Result<String> {
    let ssh = profile
        .qemu
        .as_ref()
        .and_then(|q| q.ssh.as_ref())
        .context("input proof needs SSH")?;
    let stderr_path = log.with_extension(format!("{}.stderr", mode.stem()));
    let stderr = std::fs::File::create(&stderr_path)?;
    // These are bounded upload/evdev sessions, not readiness probes. In
    // particular, backpressure qualification deliberately pauses the guest.
    // Let the upload and output deadlines below govern failure, rather than
    // dropping an otherwise valid session after one short keepalive interval.
    let command = |remote: &str| -> anyhow::Result<std::process::Command> {
        let mut cmd = std::process::Command::new("ssh");
        cmd.arg("-i")
            .arg(&key.private_key)
            .args(["-p", &ssh.host_port.to_string()])
            .args(SSH_SESSION_LIVENESS_OPTIONS);
        apply_test_ssh_identity(&mut cmd, key);
        cmd.arg(format!("{}@127.0.0.1", ssh.account))
            .arg(if ssh.account == "root" {
                remote.to_string()
            } else {
                format!("sudo -n {remote}")
            })
            .stderr(Stdio::from(stderr.try_clone()?));
        Ok(cmd)
    };
    // The disposable qualification guest owns this fixed path. Remove it before
    // creation so an existing symlink cannot redirect the upload.
    let mut upload = ChildGuard::new(command(
        "timeout 120 sh -c 'umask 077; rm -f /tmp/agentos-input-probe && cat > /tmp/agentos-input-probe && chmod 700 /tmp/agentos-input-probe'"
    )?.stdin(Stdio::from(std::fs::File::open(helper)?)).stdout(Stdio::null()).spawn()?);
    wait_qualification_child(&mut upload, qemu, 150)
        .with_context(|| format!("input probe upload failed; see {}", stderr_path.display()))?;
    let mut probe = ChildGuard::new(
        command(if mode.paused() {
            "timeout 240 /tmp/agentos-input-probe --backpressure"
        } else {
            "timeout 130 /tmp/agentos-input-probe"
        })?
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .spawn()?,
    );
    let mut output = probe.stdout.take().context("input probe stdout missing")?;
    let (send, receive) = std::sync::mpsc::sync_channel(2);
    std::thread::spawn(move || {
        for _ in 0..2 {
            let line = input_probe_line(&mut output);
            let failed = line.is_err();
            if send.send(line).is_err() || failed {
                return;
            }
        }
    });
    expect_input_probe_line(
        &receive,
        &mut probe,
        &stderr_path,
        "AGENTOS_INPUT_READY",
        60,
    )?;
    let batches: &[&[&str]] = &[
        &["keyboard", "1", "183", "1"],
        &["keyboard", "1", "183", "0"],
        &[
            "pointer", "2", "0", "17", "2", "1", "-9", "2", "8", "1", "1", "272", "1",
        ],
        &["pointer", "1", "272", "0"],
    ];
    let accepted_while_paused = if mode.paused() {
        Some(prove_paused_input_release(
            socket,
            qemu,
            mode == InputProofMode::PausedDisconnect,
            handle,
        )?)
    } else {
        None
    };
    let mut input_connection = None;
    for (index, batch) in batches.iter().enumerate().filter(|_| !mode.paused()) {
        // The same guest checker requires identical evdev output. In the
        // release pass only the server knows which key/button must be released.
        let up = index == 1 || index == 3;
        if mode == InputProofMode::Disconnect && up {
            // No key-up, release request, or protocol goodbye: EOF is the
            // only trigger available to CC for the guest-observed release.
            drop(input_connection.take());
            continue;
        }
        if input_connection.is_none() {
            input_connection = Some(CcClient::connect(socket)?);
        }
        let releasing = mode == InputProofMode::Release && up;
        anyhow::ensure!(
            input_session_result(
                input_connection.as_mut().unwrap(),
                qemu,
                releasing,
                batch[0],
                if releasing { &[] } else { &batch[1..] },
                handle,
            )? == 0,
            "guest input was not accepted"
        );
    }
    expect_input_probe_line(
        &receive,
        &mut probe,
        &stderr_path,
        if mode.paused() {
            "AGENTOS_INPUT_PASS keyboard=4 pointer=4"
        } else {
            "AGENTOS_INPUT_PASS keyboard=4 pointer=7"
        },
        120,
    )?;
    wait_qualification_child(&mut probe, qemu, 15)
        .with_context(|| format!("input probe failed; see {}", stderr_path.display()))?;
    let receipt = serde_json::json!({
        "schema": "agentos.guest_input.v1", "status": "pass", "profile": profile.id,
        "guest_handle": handle,
        "agentos_revision": agentos_revision(repo)?, "source_tree_clean": agentos_worktree_clean(repo)?,
        "helper_sha256": sha256_bytes(&std::fs::read(helper)?),
        "keyboard_events": 4, "pointer_events": if mode.paused() { 4 } else { 7 },
        "accepted_batches_while_paused": accepted_while_paused,
        "scope": "persistent public binary CC connection through virtio-input to exact Linux evdev packets",
        "release_mode": mode.name(),
        "connection_close_without_release_request": matches!(mode, InputProofMode::Disconnect | InputProofMode::PausedDisconnect),
        "paused_disconnect_qualified": mode == InputProofMode::PausedDisconnect,
        "excludes": ["physical input devices", "peer guest isolation", "guest recreation", "GUI process termination"],
        "stderr": stderr_path,
    });
    std::fs::write(
        log.with_extension(format!("{}.json", mode.stem())),
        serde_json::to_vec_pretty(&receipt)?,
    )?;
    Ok("exact guest keyboard, pointer, button and packet-boundary delivery passed".into())
}

fn desktop_tunnel_forward_spec(desktop: &DesktopPlan) -> String {
    if let Some(socket) = &desktop.guest_socket {
        format!("127.0.0.1:{}:{socket}", desktop.local_port)
    } else {
        format!(
            "127.0.0.1:{}:127.0.0.1:{}",
            desktop.local_port,
            desktop.guest_port.expect("validated desktop guest port")
        )
    }
}

fn spawn_desktop_tunnel(
    private_key: &Path,
    port: u16,
    account: &str,
    desktop: &DesktopPlan,
) -> anyhow::Result<Child> {
    std::process::Command::new("ssh")
        .arg("-i")
        .arg(private_key)
        .args(["-p", &port.to_string()])
        .args(SSH_AUTH_OPTIONS)
        .args(SSH_SESSION_LIVENESS_OPTIONS)
        .args([
            "-o",
            "ExitOnForwardFailure=yes",
            "-N",
            "-L",
            &desktop_tunnel_forward_spec(desktop),
            &format!("{account}@127.0.0.1"),
        ])
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::piped())
        .spawn()
        .context("failed to launch SSH tunnel for profile desktop")
}

fn prove_profile_desktop(
    cc_sock: &Path,
    profile: &HostProfilePlan,
    ssh_key: &SshTestKey,
    timeout: Duration,
    qemu: &mut Child,
) -> anyhow::Result<(rfb::RfbFrameEvidence, Child)> {
    let desktop = profile
        .desktop
        .as_ref()
        .context("profile has no host.desktop plan")?;
    let ssh = profile
        .qemu
        .as_ref()
        .and_then(|qemu| qemu.ssh.as_ref())
        .context("desktop profile has no host.qemu.ssh plan")?;
    prove_profile_ssh(cc_sock, profile, ssh_key, timeout, qemu)?;
    let host_unix_time = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .context("host clock is before the Unix epoch")?
        .as_secs();
    let provisioning_script = render_desktop_script(&desktop.provision_script, host_unix_time)?;
    let provisioning = run_ssh_script(
        &ssh_key.private_key,
        ssh.host_port,
        &ssh.account,
        Duration::from_secs(desktop.provision_timeout_secs),
        &provisioning_script,
    )?;
    println!("[xtask:test] profile desktop provisioning:\n{provisioning}");

    let mut tunnel =
        spawn_desktop_tunnel(&ssh_key.private_key, ssh.host_port, &ssh.account, desktop)?;
    let start = Instant::now();
    let mut last = String::from("SSH tunnel did not accept a connection");
    while start.elapsed() < timeout.min(Duration::from_secs(desktop.frame_timeout_secs)) {
        ensure_qemu_running(qemu, "waiting for profile desktop RFB frame")?;
        if let Some(status) = tunnel
            .try_wait()
            .context("failed to inspect profile desktop SSH tunnel")?
        {
            let mut stderr = String::new();
            if let Some(mut pipe) = tunnel.stderr.take() {
                let _ = pipe.read_to_string(&mut stderr);
            }
            anyhow::bail!("profile desktop SSH tunnel exited with {status}: {stderr}");
        }
        match TcpStream::connect(SocketAddr::from(([127, 0, 0, 1], desktop.local_port))) {
            Ok(mut stream) => {
                stream
                    .set_read_timeout(Some(Duration::from_secs(desktop.io_timeout_secs)))
                    .context("failed to bound desktop RFB reads")?;
                stream
                    .set_write_timeout(Some(Duration::from_secs(desktop.io_timeout_secs)))
                    .context("failed to bound desktop RFB writes")?;
                match rfb::verify_raw_frame(&mut stream) {
                    Ok(evidence) => return Ok((evidence, tunnel)),
                    Err(error) => last = format!("{error:#}"),
                }
            }
            Err(error) => last = error.to_string(),
        }
        std::thread::sleep(Duration::from_secs(1));
    }

    let _ = tunnel.kill();
    let _ = tunnel.wait();
    let mut tunnel_stderr = String::new();
    if let Some(mut pipe) = tunnel.stderr.take() {
        let _ = pipe.read_to_string(&mut tunnel_stderr);
    }
    anyhow::bail!(
        "profile desktop did not yield an RFB frame: {last}; SSH tunnel stderr={:?}",
        tunnel_stderr.trim()
    )
}

fn spawn_ssh_probe(
    private_key: &Path,
    port: u16,
    user: &str,
    known: Option<&Path>,
) -> anyhow::Result<std::process::Child> {
    let mut command = std::process::Command::new("ssh");
    let identity = SshTestKey {
        _temporary_dir: None,
        private_key: private_key.to_path_buf(),
        public_key: String::new(),
        known_hosts: known.map(Path::to_path_buf),
    };
    apply_test_ssh_identity(&mut command, &identity);
    command
        .args(["-o", "ConnectTimeout=30", "-o", "ConnectionAttempts=1"])
        .args([
            "-i",
            private_key
                .to_str()
                .context("SSH private key path is not UTF-8")?,
            "-p",
            &port.to_string(),
        ])
        .args(SSH_PROBE_LIVENESS_OPTIONS)
        .args([&format!("{user}@127.0.0.1"), "uname -s"])
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .with_context(|| format!("failed to launch SSH probe for {user} on port {port}"))
}

fn wait_ssh_probe(mut child: Child, timeout: Duration) -> anyhow::Result<std::process::Output> {
    let deadline = Instant::now() + timeout.min(Duration::from_secs(45));
    loop {
        if child.try_wait()?.is_some() {
            return child.wait_with_output().context("collect SSH probe output");
        }
        if Instant::now() >= deadline {
            let _ = child.kill();
            // Return the killed status and captured diagnostics for the retry log.
            return child
                .wait_with_output()
                .context("collect timed-out SSH probe");
        }
        std::thread::sleep(Duration::from_millis(50));
    }
}

fn profile_ssh_expectation(profile: &HostProfilePlan) -> anyhow::Result<(&str, &str)> {
    let step = profile
        .test
        .iter()
        .find(|step| step.action == "wait-ssh")
        .with_context(|| format!("profile {} has no wait-ssh test step", profile.id))?;
    Ok((
        step.args["account"].as_str(),
        step.args.get("marker").map(String::as_str).unwrap_or(""),
    ))
}

fn retain_scenario_ssh_attempt(
    directory: &Path,
    attempt: usize,
    profile: &str,
    account: &str,
    port: u16,
    marker: &str,
    output: &std::process::Output,
) -> anyhow::Result<bool> {
    std::fs::create_dir_all(directory)?;
    let prefix = directory.join(format!("attempt-{attempt}"));
    let stdout = String::from_utf8_lossy(&output.stdout);
    let accepted = output.status.success() && (marker.is_empty() || stdout.trim() == marker);
    std::fs::write(prefix.with_extension("stdout"), &output.stdout)?;
    std::fs::write(prefix.with_extension("stderr"), &output.stderr)?;
    let receipt = serde_json::json!({
        "schema": "agentos.scenario_ssh_attempt.v1", "profile": profile,
        "account": account, "host_port": port, "command": "uname -s",
        "attempt": attempt, "exit_code": output.status.code(),
        "process_success": output.status.success(), "expected_stdout": marker,
        "accepted": accepted,
        "stdout_sha256": sha256_bytes(&output.stdout),
        "stderr_sha256": sha256_bytes(&output.stderr),
    });
    std::fs::write(
        prefix.with_extension("json"),
        serde_json::to_vec_pretty(&receipt)?,
    )?;
    Ok(accepted)
}

fn wait_for_scenario_guest_ssh(
    guest: &ScenarioGuestPlan,
    ssh_key: &SshTestKey,
    timeout: Duration,
    qemu: &mut Child,
    evidence: &Path,
) -> anyhow::Result<()> {
    let (account, marker) = profile_ssh_expectation(&guest.profile)?;
    let start = Instant::now();
    let mut last = String::from("no SSH attempt completed");
    let mut attempt = 0;
    while start.elapsed() < timeout {
        ensure_qemu_running(qemu, "waiting for profile authenticated SSH")?;
        let known = if guest.profile.seed.is_some() {
            Some(
                ssh_key
                    .known_hosts
                    .as_deref()
                    .context("seeded scenario SSH requires pinned identity")?,
            )
        } else {
            None
        };
        let probe = spawn_ssh_probe(&ssh_key.private_key, guest.ssh_host_port, account, known)?;
        let output = wait_ssh_probe(probe, timeout.saturating_sub(start.elapsed()))?;
        let stdout = String::from_utf8_lossy(&output.stdout);
        attempt += 1;
        if retain_scenario_ssh_attempt(
            evidence,
            attempt,
            &guest.profile.id,
            account,
            guest.ssh_host_port,
            marker,
            &output,
        )? {
            println!(
                "[xtask:test] {} SSH remote exec uname -s: status={} stdout={:?} stderr={:?}",
                guest.profile.id,
                output.status,
                stdout,
                String::from_utf8_lossy(&output.stderr)
            );
            return Ok(());
        }
        last = format!(
            "status={} stdout={:?} stderr={:?}",
            output.status,
            stdout.trim(),
            String::from_utf8_lossy(&output.stderr).trim(),
        );
        std::thread::sleep(Duration::from_secs(2));
    }
    anyhow::bail!(
        "profile {} authenticated SSH did not become ready: {last}",
        guest.profile.id
    )
}

fn wait_for_scenario_ssh(
    scenario: &HostScenarioPlan,
    ssh_key: &SshTestKey,
    timeout: Duration,
    qemu: &mut Child,
    evidence: &Path,
) -> anyhow::Result<String> {
    let start = Instant::now();
    let mut failures = Vec::new();
    let mut round = 0;
    while start.elapsed() < timeout {
        ensure_qemu_running(qemu, "waiting for scenario authenticated SSH")?;
        failures.clear();
        round += 1;
        for (index, guest) in scenario.guests.iter().enumerate() {
            let directory = evidence.join(format!("round-{round}-guest-{index}"));
            match wait_for_scenario_guest_ssh(
                guest,
                ssh_key,
                Duration::from_secs(35),
                qemu,
                &directory,
            ) {
                Ok(()) => println!(
                    "[xtask:test] {} authenticated SSH ready in concurrent probe",
                    guest.profile.id
                ),
                Err(error) => {
                    let detail = format!("{error:#}");
                    eprintln!("[xtask:test] concurrent SSH retry: {detail}");
                    failures.push(detail);
                }
            }
        }
        if failures.is_empty() {
            return Ok(format!(
                "scenario {} has concurrent authenticated SSH for {} profiles",
                scenario.id,
                scenario.guests.len()
            ));
        }
        std::thread::sleep(Duration::from_secs(2));
    }
    anyhow::bail!(
        "scenario {} authenticated SSH did not become ready: {}",
        scenario.id,
        failures.join("; ")
    )
}

fn prepare_scenario_seed(
    repo: &Path,
    guest: &mut ScenarioGuestPlan,
    key: &SshTestKey,
) -> anyhow::Result<()> {
    let seed = guest
        .profile
        .seed
        .as_ref()
        .context("missing scenario seed contract")?;
    let qemu = guest
        .profile
        .qemu
        .as_mut()
        .context("seed requires QEMU profile")?;
    anyhow::ensure!(
        qemu.media.iter().filter(|disk| disk.writable).count() == 1,
        "scenario seed requires exactly one writable disk"
    );
    let disk = qemu.media.iter_mut().find(|disk| disk.writable).unwrap();
    anyhow::ensure!(
        disk.override_env
            .iter()
            .all(|name| std::env::var_os(name).is_none()),
        "scenario seed rejects writable media overrides"
    );
    let parent = repo.join("build/evidence");
    std::fs::create_dir_all(&parent)?;
    let directory = tempfile::Builder::new()
        .prefix("scenario-seed-")
        .tempdir_in(parent)?
        .keep();
    let output = directory.join("seeded.raw");
    crate::cmd_seed_guest::run(&crate::cmd_seed_guest::SeedGuestArgs {
        root_ext4: repo.join(&seed.root_ext4),
        public_key: key.private_key.with_extension("pub"),
        output: output.clone(),
        guest_address: guest.ssh_guest_address.parse()?,
        instance_id: directory
            .file_name()
            .unwrap()
            .to_str()
            .context("seed directory is not UTF-8")?
            .into(),
        disk_raw: Some(repo.join(&seed.disk_raw)),
        partition_offset: Some(seed.partition_offset),
    })?;
    disk.path = output.to_str().context("seed output is not UTF-8")?.into();
    disk.managed_persistent = true;
    println!(
        "[xtask:test] scenario seed for {}: {}",
        guest.profile.id,
        directory.display()
    );
    Ok(())
}

fn provision_scenario_guest(
    socket: &Path,
    cc: &mut CcClient,
    guest: &ScenarioGuestPlan,
    handle: u32,
    key: &SshTestKey,
    log: &Path,
    timeout: Duration,
    qemu: &mut Child,
) -> anyhow::Result<String> {
    if guest.profile.seed.is_some() {
        // The seed provisions key-only SSH; there is no console shell to send
        // commands to. Release the single-client CC socket for its boot proof.
        drop(cc.stream.take());
        let known = key
            .known_hosts
            .as_ref()
            .context("seeded scenario lacks host-key receipt")?;
        let proof = seeded_ssh_via_cc(
            socket,
            log,
            &guest.profile,
            &key.private_key,
            known.exists().then_some(known.as_path()),
            None,
            guest.ssh_host_port,
            timeout,
            qemu,
            handle,
        )?;
        if !known.exists() {
            std::fs::copy(log.with_extension("known_hosts"), known)?;
        }
        *cc = connect_cc_client(socket, Duration::from_secs(30), qemu)?;
        return Ok(proof);
    }
    let console = wait_for_guest_console_login_on_cc(
        socket,
        cc,
        handle,
        &guest.profile.id,
        Some(&guest.profile),
        timeout,
        qemu,
        None,
    )?;
    let commands = profile_provision_commands(&guest.profile, &key.public_key)?;
    run_guest_console_commands(
        socket,
        cc,
        handle,
        &guest.profile.id,
        Some(&guest.profile),
        &commands,
        Duration::from_secs(600),
        qemu,
    )?;
    Ok(console)
}

fn wait_for_dual_guest_consoles_via_cc(
    cc_sock: &Path,
    scenario: &HostScenarioPlan,
    timeout: Duration,
    qemu: &mut Child,
    ssh_key: &SshTestKey,
    keep_running: bool,
    recreate: bool,
) -> anyhow::Result<String> {
    let start = Instant::now();
    let ssh_evidence = cc_sock.with_extension("ssh-evidence");
    std::fs::create_dir(&ssh_evidence).context("create fresh scenario SSH evidence directory")?;
    println!(
        "[xtask:test] Scenario SSH transcripts: {}",
        ssh_evidence.display()
    );
    let create_timeout = timeout;
    /*
     * VirtIO-console is a byte stream. Keep one connection across both
     * CREATE replies and the first SUSPEND so QEMU cannot still be retiring
     * a closed socket while the lifecycle request is already buffered on a
     * replacement connection.
     */
    let mut boot_cc = connect_cc_client(cc_sock, timeout.min(Duration::from_secs(30)), qemu)?;

    anyhow::ensure!(
        scenario.guests.len() == 2,
        "the ordered suspend/resume scenario executor requires exactly two profiles"
    );
    let lead = &scenario.guests[0];
    let deferred = &scenario.guests[1];

    let lead_handle = create_guest_via_cc_wait(
        &mut boot_cc,
        lead.profile.control_type as u8,
        VIBEOS_ARCH_AARCH64,
        lead.ram_mb,
        &lead.profile.id,
        create_timeout,
        qemu,
    )
    .with_context(|| format!("failed to create {} through vm_manager", lead.profile.id))?;

    let mut deferred_handle = create_guest_via_cc_wait(
        &mut boot_cc,
        deferred.profile.control_type as u8,
        VIBEOS_ARCH_AARCH64,
        deferred.ram_mb,
        &deferred.profile.id,
        create_timeout,
        qemu,
    )
    .with_context(|| {
        format!(
            "failed to create {} through vm_manager",
            deferred.profile.id
        )
    })?;

    /*
     * Creating both guests establishes both clients of the shared host block
     * path. Quiesce the deferred profile immediately so the lead profile's
     * media boot cannot lose the single emulated CPU to a busier guest.
     */
    for guest in [lead, deferred] {
        let result = try_create_guest_via_cc(
            &mut boot_cc,
            guest.profile.control_type as u8,
            VIBEOS_ARCH_AARCH64,
            guest.ram_mb,
        )?;
        anyhow::ensure!(
            matches!(result, Err((CC_ERR_RELAY_FAULT, _))),
            "duplicate profile {} creation was not rejected: {result:?}",
            guest.profile.id
        );
    }
    println!("[xtask:test] duplicate profile creates rejected without alias handles");
    let deferred_boot_suspend = suspend_guest_via_cc(&mut boot_cc, deferred_handle)
        .with_context(|| format!("failed to defer {} boot", deferred.profile.id))?;
    println!(
        "[xtask:test] suspended {} handle={} state={} while {} boots",
        deferred.profile.id, deferred_handle, deferred_boot_suspend, lead.profile.id
    );

    let lead_console = provision_scenario_guest(
        cc_sock,
        &mut boot_cc,
        lead,
        lead_handle,
        ssh_key,
        &ssh_evidence.join("lead-first.log"),
        timeout.saturating_sub(start.elapsed()),
        qemu,
    )?;
    wait_for_scenario_guest_ssh(
        lead,
        ssh_key,
        Duration::from_secs(180),
        qemu,
        &ssh_evidence.join("lead-before-suspend"),
    )
    .with_context(|| format!("{} SSH was not live before checkpoint", lead.profile.id))?;
    println!(
        "[xtask:test] {} authenticated SSH live before suspend",
        lead.profile.id
    );
    let lead_probe_suspend = suspend_guest_via_cc(&mut boot_cc, lead_handle)
        .with_context(|| format!("failed to suspend {} for checkpoint", lead.profile.id))?;
    println!(
        "[xtask:test] suspended {} handle={} state={} for immediate resume checkpoint",
        lead.profile.id, lead_handle, lead_probe_suspend
    );
    let lead_probe_resume = resume_guest_via_cc(&mut boot_cc, lead_handle)
        .with_context(|| format!("failed to resume {} after checkpoint", lead.profile.id))?;
    println!(
        "[xtask:test] resumed {} handle={} state={} for immediate SSH checkpoint",
        lead.profile.id, lead_handle, lead_probe_resume
    );
    wait_for_scenario_guest_ssh(
        lead,
        ssh_key,
        Duration::from_secs(180),
        qemu,
        &ssh_evidence.join("lead-after-resume"),
    )
    .with_context(|| format!("{} SSH did not survive checkpoint", lead.profile.id))?;
    let lead_boot_suspend = suspend_guest_via_cc(&mut boot_cc, lead_handle)
        .with_context(|| format!("failed to defer provisioned {}", lead.profile.id))?;
    println!(
        "[xtask:test] suspended provisioned {} handle={} state={} while {} boots",
        lead.profile.id, lead_handle, lead_boot_suspend, deferred.profile.id
    );
    let deferred_boot_resume = resume_guest_via_cc(&mut boot_cc, deferred_handle)
        .with_context(|| format!("failed to resume {}", deferred.profile.id))?;
    println!(
        "[xtask:test] resumed {} handle={} state={}",
        deferred.profile.id, deferred_handle, deferred_boot_resume
    );

    let deferred_console = provision_scenario_guest(
        cc_sock,
        &mut boot_cc,
        deferred,
        deferred_handle,
        ssh_key,
        &ssh_evidence.join("deferred-first.log"),
        timeout.saturating_sub(start.elapsed()),
        qemu,
    )?;
    resume_guest_via_cc(&mut boot_cc, lead_handle)
        .with_context(|| format!("failed to resume provisioned {}", lead.profile.id))?;
    let ssh = wait_for_scenario_ssh(
        scenario,
        ssh_key,
        Duration::from_secs(600),
        qemu,
        &ssh_evidence.join("concurrent"),
    )?;

    if recreate {
        let retired = deferred_handle;
        destroy_guest_via_cc(&mut boot_cc, retired, Some(&deferred.profile))?;
        wait_for_scenario_guest_ssh(
            lead,
            ssh_key,
            Duration::from_secs(180),
            qemu,
            &ssh_evidence.join("peer-after-destroy"),
        )?;
        deferred_handle = create_guest_via_cc_wait(
            &mut boot_cc,
            deferred.profile.control_type as u8,
            VIBEOS_ARCH_AARCH64,
            deferred.ram_mb,
            &deferred.profile.id,
            timeout.saturating_sub(start.elapsed()),
            qemu,
        )?;
        anyhow::ensure!(
            deferred_handle != retired && deferred_handle != lead_handle,
            "scenario recreation reused a retired or peer handle"
        );
        // Never suspend the peer during reconstruction or replacement boot.
        provision_scenario_guest(
            cc_sock,
            &mut boot_cc,
            deferred,
            deferred_handle,
            ssh_key,
            &ssh_evidence.join("deferred-recreated.log"),
            timeout.saturating_sub(start.elapsed()),
            qemu,
        )?;
        wait_for_scenario_ssh(
            scenario,
            ssh_key,
            Duration::from_secs(600),
            qemu,
            &ssh_evidence.join("concurrent-after-recreation"),
        )?;
        for opcode in [
            MSG_CC_GUEST_STATUS,
            MSG_CC_RESUME_GUEST,
            MSG_CC_SUSPEND_GUEST,
        ] {
            anyhow::ensure!(
                boot_cc.call(opcode, retired, 0, 0, &[])?.mr[0] == CC_ERR_BAD_HANDLE,
                "retired scenario handle became usable after recreation"
            );
        }
        println!("[xtask:test] scenario guest recreated: retired={retired} fresh={deferred_handle} peer={lead_handle}; concurrent authenticated SSH and stale-handle rejection passed");
    }

    for guest in &scenario.guests {
        crate::guest_session::prove(
            &guest.profile,
            &ssh_key.private_key,
            guest.ssh_host_port,
            if guest.profile.seed.is_some() {
                Some(ssh_key.known_hosts.as_deref().context("seeded session requires pinned identity")?)
            } else {
                None
            },
            Duration::from_secs(600),
        )?;
    }

    if !keep_running {
        destroy_guest_via_cc(&mut boot_cc, deferred_handle, Some(&deferred.profile))
            .with_context(|| format!("failed to destroy {}", deferred.profile.id))?;
        destroy_guest_via_cc(&mut boot_cc, lead_handle, Some(&lead.profile))
            .with_context(|| format!("failed to destroy {}", lead.profile.id))?;
        for handle in [lead_handle, deferred_handle, u32::MAX] {
            let reply = boot_cc.call(MSG_CC_GUEST_STATUS, handle, 0, 0, &[])?;
            anyhow::ensure!(
                reply.mr[0] == CC_ERR_BAD_HANDLE,
                "stale/invalid guest handle {handle} returned status {}",
                reply.mr[0]
            );
        }
        println!("[xtask:test] destroyed and invalid guest handles rejected");
    }

    Ok(format!(
        "scenario {} consoles ready: {} handle={} ({}); {} handle={} ({}); {ssh}; functional SSH sessions verified; {}",
        scenario.id,
        lead.profile.id,
        lead_handle,
        lead_console,
        deferred.profile.id,
        deferred_handle,
        deferred_console,
        if keep_running {
            "profiles retained for manual SSH"
        } else {
            "profiles destroyed"
        }
    ))
}

fn connect_cc_client(
    cc_sock: &Path,
    timeout: Duration,
    qemu: &mut Child,
) -> anyhow::Result<CcClient> {
    let start = Instant::now();
    let mut last_err = None;
    while start.elapsed() < timeout {
        ensure_qemu_running(qemu, "connecting to CC-PD socket")?;
        if cc_sock.exists() {
            match CcClient::connect(cc_sock) {
                Ok(cc) => return Ok(cc),
                Err(err) => last_err = Some(format!("{err:#}")),
            }
        }
        std::thread::sleep(Duration::from_millis(200));
    }

    if let Some(err) = last_err {
        anyhow::bail!(
            "failed to connect to {} within {}s: {}",
            cc_sock.display(),
            timeout.as_secs(),
            err
        );
    }
    anyhow::bail!(
        "CC-PD socket {} did not appear within {}s",
        cc_sock.display(),
        timeout.as_secs()
    );
}

pub fn wait_for_cc_socket(
    cc_sock: &Path,
    timeout: Duration,
    qemu: &mut Child,
) -> anyhow::Result<()> {
    let start = Instant::now();
    while start.elapsed() < timeout {
        ensure_qemu_running(qemu, "waiting for CC-PD socket")?;
        if cc_sock.exists() {
            return Ok(());
        }
        std::thread::sleep(Duration::from_millis(200));
    }

    anyhow::bail!(
        "CC-PD socket {} did not appear within {}s",
        cc_sock.display(),
        timeout.as_secs()
    );
}

fn ensure_qemu_running(qemu: &mut Child, context: &str) -> anyhow::Result<()> {
    if let Some(status) = qemu
        .try_wait()
        .with_context(|| format!("failed to poll QEMU status while {context}"))?
    {
        anyhow::bail!("QEMU exited with status {status} while {context}");
    }
    Ok(())
}

fn connect_host_net_stimulus(port: u16, qemu: &mut Child) -> Option<TcpStream> {
    let addr = SocketAddr::from(([127, 0, 0, 1], port));
    let start = Instant::now();
    while start.elapsed() < Duration::from_secs(10) {
        if ensure_qemu_running(qemu, "connecting host network stimulus").is_err() {
            return None;
        }
        if let Ok(stream) = TcpStream::connect_timeout(&addr, Duration::from_millis(250)) {
            println!("[xtask:test] Host network stimulus connected through 127.0.0.1:{port}");
            return Some(stream);
        }
        std::thread::sleep(Duration::from_millis(100));
    }
    println!("[xtask:test] WARN: host network stimulus could not connect to 127.0.0.1:{port}");
    None
}

fn verify_native_guest_network(
    cc_sock: &Path,
    profile: Option<&HostProfilePlan>,
    timeout: Duration,
    qemu: &mut Child,
) -> anyhow::Result<String> {
    let mut cc = connect_cc_client(cc_sock, timeout.min(Duration::from_secs(30)), qemu)?;
    anyhow::ensure!(
        cc.call(0x2e80, 2, 0, 0, &[])?.mr[0] == CC_ERR_RELAY_FAULT,
        "native test relay accepted an invalid contract version"
    );
    let mut previous_sequence = None;
    for round in 1..=3 {
        let reply = cc.call(0x2e80, 1, 0, 0, &[])?;
        anyhow::ensure!(
            reply.mr[0] == CC_OK && reply.mr[1] == 1 && reply.mr[2] == 3,
            "native live NIC exchange failed: {:?}",
            reply.mr
        );
        if let Some(previous) = previous_sequence {
            anyhow::ensure!(
                reply.mr[3] == previous + 1,
                "native reply was not a fresh exchange"
            );
        }
        previous_sequence = Some(reply.mr[3]);
        let marker = format!("NATIVE_COEXIST_GUEST_{round}");
        let command = format!(
            "ping -c 1 -W 5 10.0.2.2 >/dev/null && printf 'NATIVE_COEXIST_%s\\n' 'GUEST_{round}'"
        );
        cc_send_console_line(&mut cc, 0, command.as_bytes())?;
        let start = Instant::now();
        let mut output = String::new();
        loop {
            ensure_qemu_running(qemu, "checking live native/guest network traffic")?;
            output.push_str(&cc_log_stream_for_handle(&mut cc, 0, profile)?);
            if output.contains(&marker) {
                break;
            }
            anyhow::ensure!(
                start.elapsed() < timeout.min(Duration::from_secs(60)),
                "guest network response missing after native exchange: {output}"
            );
            output = tail_chars(&output, 4096);
            std::thread::sleep(Duration::from_millis(20));
        }
        println!(
            "[xtask:test] native NIC exchange {} and live guest ping round {round} verified",
            reply.mr[3]
        );
    }
    Ok(String::from(
        "native NIC exchanges interleaved with three live guest network responses",
    ))
}

fn cc_log_stream_for_handle(
    cc: &mut CcClient,
    guest_handle: u32,
    profile: Option<&HostProfilePlan>,
) -> anyhow::Result<String> {
    let reply = cc_log_stream_reply(cc, guest_handle, profile)?;
    anyhow::ensure!(
        reply.mr[0] == CC_OK,
        "MSG_CC_LOG_STREAM returned ok={}",
        reply.mr[0]
    );
    let len = (reply.mr[1] as usize).min(reply.shmem.len());
    Ok(String::from_utf8_lossy(&reply.shmem[..len]).into_owned())
}

fn cc_log_stream_reply(
    cc: &mut CcClient,
    guest_handle: u32,
    profile: Option<&HostProfilePlan>,
) -> anyhow::Result<CcReply> {
    let pd_id = if guest_handle == 0 {
        0
    } else {
        match profile.map(|value| value.control_type) {
            Some(1) => TRACE_PD_GUEST_VMM_PRIMARY,
            Some(2) => TRACE_PD_GUEST_VMM_SECONDARY,
            Some(value) => {
                anyhow::bail!("profile control_type {value} has no configured trace-PD slot")
            }
            None => anyhow::bail!("dynamic guest log streaming requires a resolved profile"),
        }
    };
    cc.call(MSG_CC_LOG_STREAM, guest_handle, pd_id, 0, &[])
        .context("MSG_CC_LOG_STREAM failed")
}

fn cc_send_raw_byte(cc: &mut CcClient, guest_handle: u32, byte: u8) -> anyhow::Result<()> {
    let mut shmem = [0u8; 24];
    wr32(&mut shmem, 0, CC_INPUT_KEY_DOWN);
    wr32(&mut shmem, 4, CC_INPUT_RAW_BYTE_BASE | u32::from(byte));

    cc_send_input_frame(cc, guest_handle, &shmem, "MSG_CC_SEND_INPUT")?;
    Ok(())
}

fn cc_text_event(chunk: &[u8]) -> Vec<u8> {
    let mut shmem = vec![0u8; 24 + chunk.len()];
    wr32(&mut shmem, 0, CC_INPUT_TEXT);
    wr32(&mut shmem, 4, chunk.len() as u32);
    shmem[24..].copy_from_slice(chunk);
    shmem
}

fn cc_send_raw_bytes(cc: &mut CcClient, guest_handle: u32, bytes: &[u8]) -> anyhow::Result<()> {
    for chunk in bytes.chunks(CC_INPUT_TEXT_CHUNK) {
        let shmem = cc_text_event(chunk);
        cc_send_input_frame(cc, guest_handle, &shmem, "MSG_CC_SEND_INPUT text")?;
        /*
         * Let the lower-priority guest consume RX descriptors between frames.
         * Without this yield, a host can fill the VMM ingress queue while the
         * higher-priority CC/Vibe/VM-manager call chain remains runnable.
         */
        std::thread::sleep(Duration::from_millis(50));
    }
    Ok(())
}

fn cc_send_console_line(cc: &mut CcClient, guest_handle: u32, line: &[u8]) -> anyhow::Result<()> {
    cc_send_raw_bytes(cc, guest_handle, line)?;
    cc_send_raw_byte(cc, guest_handle, b'\r')
}

fn cc_send_input_frame(
    cc: &mut CcClient,
    guest_handle: u32,
    shmem: &[u8],
    operation: &str,
) -> anyhow::Result<()> {
    let deadline = Instant::now() + CC_INPUT_RETRY_DEADLINE;
    loop {
        let reply = cc
            .call(MSG_CC_SEND_INPUT, guest_handle, 0, 0, shmem)
            .with_context(|| format!("{operation} failed"))?;
        if reply.mr[0] == CC_OK {
            return Ok(());
        }
        /*
         * CC currently folds a full downstream console queue into
         * CC_ERR_RELAY_FAULT. The VMM rejects before enqueue, so retrying the
         * same frame cannot duplicate input. Keep the retry bounded so a
         * permanent relay failure still terminates the acceptance gate.
         */
        if reply.mr[0] != CC_ERR_RELAY_FAULT || Instant::now() >= deadline {
            anyhow::bail!("{operation} returned ok={}", reply.mr[0]);
        }
        std::thread::sleep(Duration::from_millis(250));
    }
}

fn verify_guest_teardown(
    cc_sock: &Path,
    log_path: &Path,
    qemu: &mut Child,
    input_detach_required: bool,
    graphics_detach_required: bool,
) -> anyhow::Result<String> {
    let mut cc = CcClient::connect(cc_sock)?;
    let status = cc.call(MSG_CC_GUEST_STATUS, 0, 0, 0, &[])?;
    anyhow::ensure!(status.mr[0] == CC_OK, "boot guest absent before teardown");
    destroy_guest_via_cc(&mut cc, 0, None)?;
    for opcode in [
        MSG_CC_GUEST_STATUS,
        MSG_CC_RESUME_GUEST,
        MSG_CC_SUSPEND_GUEST,
    ] {
        let reply = cc.call(opcode, 0, 0, 0, &[])?;
        anyhow::ensure!(
            reply.mr[0] == CC_ERR_BAD_HANDLE,
            "destroyed guest accepted opcode {opcode:#x}: {}",
            reply.mr[0]
        );
    }
    let mut markers = vec![
        "guest teardown: execution and RAM revoked",
        "guest teardown: private paging revoked",
        "guest teardown: network queues detached",
        "guest teardown: block queues detached",
        "guest teardown: serial queues detached",
        "guest teardown: private queue pages revoked",
    ];
    if input_detach_required {
        markers.push("guest teardown: input queues detached");
    }
    if graphics_detach_required {
        markers.push("guest teardown: framebuffer queues detached");
        markers.push("guest teardown: private graphics pages revoked");
    }
    wait_for_all_markers(log_path, &markers, Duration::from_secs(10), qemu)?;
    Ok("running guest destroyed; execution/RAM revoked and stale lifecycle handle rejected".into())
}

fn destroy_guest_via_cc(
    cc: &mut CcClient,
    guest_handle: u32,
    profile: Option<&HostProfilePlan>,
) -> anyhow::Result<()> {
    let deadline = Instant::now() + Duration::from_secs(120);
    loop {
        let reply = cc
            .call(
                MSG_CC_DESTROY_GUEST,
                guest_handle,
                GUEST_DESTROY_NORMAL,
                0,
                &[],
            )
            .context("MSG_CC_DESTROY_GUEST failed")?;
        if reply.mr[0] == CC_OK {
            return Ok(());
        }
        anyhow::ensure!(
            reply.mr[0] == CC_ERR_RELAY_FAULT && Instant::now() < deadline,
            "MSG_CC_DESTROY_GUEST returned ok={} before drain completed",
            reply.mr[0]
        );
        // Consume copied output so a partially drained console can finish.
        // This also separates retries from CC's identical-request replay cache.
        let console = cc_log_stream_reply(cc, guest_handle, profile)?;
        // Serial detach can finish before input/graphics teardown. The boot
        // console then rejects reads with RELAY_FAULT. Handle-addressed console
        // reads map the same drain failure to BAD_HANDLE. Neither is successful
        // destruction: require an explicit DESTROY acknowledgement within the
        // original deadline. Ordinary console reads remain strict.
        anyhow::ensure!(
            console.mr[0] == CC_OK
                || console.mr[0]
                    == if guest_handle == 0 {
                        CC_ERR_RELAY_FAULT
                    } else {
                        CC_ERR_BAD_HANDLE
                    },
            "MSG_CC_LOG_STREAM during destroy returned ok={}",
            console.mr[0]
        );
        std::thread::sleep(Duration::from_millis(100));
    }
}

fn lifecycle_guest_via_cc(
    cc: &mut CcClient,
    opcode: u32,
    guest_handle: u32,
    action: &str,
) -> anyhow::Result<u32> {
    let reply = cc
        .call(opcode, guest_handle, 0, 0, &[])
        .with_context(|| format!("MSG_CC_{action}_GUEST failed"))?;
    anyhow::ensure!(
        reply.mr[0] == CC_OK,
        "MSG_CC_{action}_GUEST returned ok={} detail={}",
        reply.mr[0],
        reply.mr[1]
    );
    Ok(reply.mr[1])
}

fn suspend_guest_via_cc(cc: &mut CcClient, guest_handle: u32) -> anyhow::Result<u32> {
    lifecycle_guest_via_cc(cc, MSG_CC_SUSPEND_GUEST, guest_handle, "SUSPEND")
}

fn resume_guest_via_cc(cc: &mut CcClient, guest_handle: u32) -> anyhow::Result<u32> {
    lifecycle_guest_via_cc(cc, MSG_CC_RESUME_GUEST, guest_handle, "RESUME")
}

pub fn cc_call(
    cc_sock: &Path,
    opcode: u32,
    mr1: u32,
    mr2: u32,
    mr3: u32,
    shmem_in: &[u8],
) -> anyhow::Result<CcReply> {
    CcClient::connect(cc_sock)?.call(opcode, mr1, mr2, mr3, shmem_in)
}

fn rd32(src: &[u8], off: usize) -> u32 {
    u32::from_le_bytes(src[off..off + 4].try_into().unwrap())
}

fn wr32(dst: &mut [u8], off: usize, value: u32) {
    dst[off..off + 4].copy_from_slice(&value.to_le_bytes());
}

fn tail_chars(s: &str, max_chars: usize) -> String {
    let len = s.chars().count();
    s.chars().skip(len.saturating_sub(max_chars)).collect()
}

#[cfg(test)]
mod tests {
    #[test]
    fn second_intel_media_requires_independent_regular_disks() {
        use std::os::unix::fs::symlink;
        let dir = tempfile::tempdir().unwrap();
        let primary = dir.path().join("primary.raw");
        let secondary = dir.path().join("secondary.raw");
        let hardlink = dir.path().join("hardlink.raw");
        let link = dir.path().join("symlink.raw");
        std::fs::write(&primary, [0x11; 512]).unwrap();
        std::fs::write(&secondary, [0x22; 512]).unwrap();
        assert!(super::validate_x86_distinct_media(&primary, &secondary).is_ok());
        assert!(super::validate_x86_distinct_media(&primary, &primary).is_err());
        std::fs::hard_link(&primary, &hardlink).unwrap();
        symlink(&primary, &link).unwrap();
        assert!(super::validate_x86_distinct_media(&primary, &hardlink).is_err());
        assert!(super::validate_x86_distinct_media(&primary, &link).is_err());
        assert!(super::validate_x86_distinct_media(&primary, dir.path()).is_err());
        std::fs::write(&secondary, [0x22; 511]).unwrap();
        assert!(super::validate_x86_distinct_media(&primary, &secondary).is_err());
        let comma = dir.path().join("disk,readonly=off");
        std::fs::write(&comma, [0x22; 512]).unwrap();
        assert!(super::validate_x86_distinct_media(&primary, &comma).is_err());
        assert_eq!(std::fs::read(&primary).unwrap(), vec![0x11; 512]);
    }
    #[test]
    fn destroy_retries_detached_console_but_requires_destroy_acknowledgement() {
        use std::os::unix::net::UnixListener;
        let repo = Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap();
        let profile = cmd_guest_profile::host_profile_plan(
            &repo.join("guest-profiles"),
            Path::new("debian-arm64-nocloud.toml"),
        )
        .unwrap();
        for (handle, console_status, final_destroy, retries, succeeds) in [
            (0, CC_OK, CC_OK, true, true),
            (0, CC_ERR_RELAY_FAULT, CC_OK, true, true),
            (0, CC_ERR_RELAY_FAULT, CC_ERR_BAD_HANDLE, true, false),
            (0, CC_ERR_BAD_HANDLE, CC_OK, false, false),
            (17, CC_OK, CC_OK, true, true),
            (17, CC_ERR_BAD_HANDLE, CC_OK, true, true),
            (17, CC_ERR_BAD_HANDLE, CC_ERR_BAD_HANDLE, true, false),
            (17, CC_ERR_RELAY_FAULT, CC_OK, false, false),
        ] {
            let directory = tempfile::tempdir().unwrap();
            let socket = directory.path().join("cc.sock");
            let listener = UnixListener::bind(&socket).unwrap();
            let server = std::thread::spawn(move || {
                let (mut stream, _) = listener.accept().unwrap();
                mock_cc_sync(&mut stream);
                let mut exchanges = vec![
                    (MSG_CC_DESTROY_GUEST, CC_ERR_RELAY_FAULT),
                    (MSG_CC_LOG_STREAM, console_status),
                ];
                if retries {
                    exchanges.push((MSG_CC_DESTROY_GUEST, final_destroy));
                }
                for (opcode, status) in exchanges {
                    let mut request = [0u8; CC_REQ_SIZE];
                    stream.read_exact(&mut request).unwrap();
                    assert_eq!(rd32(&request, 0), opcode);
                    assert_eq!(rd32(&request, 4), handle);
                    let mut reply = [0u8; CC_REPLY_SIZE];
                    wr32(&mut reply, 0, status);
                    stream.write_all(&reply).unwrap();
                }
            });
            let mut cc = CcClient::connect(&socket).unwrap();
            assert_eq!(
                destroy_guest_via_cc(&mut cc, handle, (handle != 0).then_some(&profile)).is_ok(),
                succeeds
            );
            server.join().unwrap();
        }
    }

    #[test]
    fn ordinary_boot_console_still_rejects_relay_fault() {
        use std::os::unix::net::UnixListener;
        let directory = tempfile::tempdir().unwrap();
        let socket = directory.path().join("cc.sock");
        let listener = UnixListener::bind(&socket).unwrap();
        let server = std::thread::spawn(move || {
            let (mut stream, _) = listener.accept().unwrap();
            mock_cc_sync(&mut stream);
            let mut request = [0u8; CC_REQ_SIZE];
            stream.read_exact(&mut request).unwrap();
            assert_eq!(rd32(&request, 0), MSG_CC_LOG_STREAM);
            let mut reply = [0u8; CC_REPLY_SIZE];
            wr32(&mut reply, 0, CC_ERR_RELAY_FAULT);
            stream.write_all(&reply).unwrap();
        });
        let mut cc = CcClient::connect(&socket).unwrap();
        assert!(cc_log_stream_for_handle(&mut cc, 0, None).is_err());
        server.join().unwrap();
    }

    #[test]
    fn managed_cc_create_preserves_architecture_and_console_rejects_bad_lengths() {
        use std::os::unix::net::UnixListener;
        let directory = tempfile::tempdir().unwrap();
        let socket = directory.path().join("cc.sock");
        let listener = UnixListener::bind(&socket).unwrap();
        let server = std::thread::spawn(move || {
            let (mut stream, _) = listener.accept().unwrap();
            mock_cc_sync(&mut stream);
            for arch in [VIBEOS_ARCH_AARCH64, VIBEOS_ARCH_X86_64] {
                let mut request = [0u8; CC_REQ_SIZE];
                stream.read_exact(&mut request).unwrap();
                assert_eq!(rd32(&request, 0), MSG_CC_CREATE_GUEST);
                assert_eq!(&request[16..18], &[1, arch]);
                assert_eq!(rd32(&request, 20), 64);
                assert_eq!(rd32(&request, 32), 7);
                let mut reply = [0u8; CC_REPLY_SIZE];
                wr32(&mut reply, 4, 17);
                stream.write_all(&reply).unwrap();
            }
            for (status, length) in [(0, 3), (0, 4097), (CC_ERR_BAD_HANDLE, 0)] {
                let mut request = [0u8; CC_REQ_SIZE];
                stream.read_exact(&mut request).unwrap();
                assert_eq!(rd32(&request, 0), MSG_CC_LOG_STREAM);
                assert_eq!(rd32(&request, 4), 17);
                assert_eq!(rd32(&request, 8), 0);
                assert_eq!(rd32(&request, 12), 1);
                let mut reply = [0u8; CC_REPLY_SIZE];
                wr32(&mut reply, 0, status);
                wr32(&mut reply, 4, length);
                wr32(&mut reply, 8, 17);
                reply[16..19].copy_from_slice(b"abc");
                stream.write_all(&reply).unwrap();
            }
        });
        let mut cc = CcClient::connect(&socket).unwrap();
        for arch in [VIBEOS_ARCH_AARCH64, VIBEOS_ARCH_X86_64] {
            assert_eq!(
                try_create_guest_via_cc(&mut cc, 1, arch, 64).unwrap(),
                Ok(17)
            );
        }
        assert_eq!(x86_cc_console_bytes(&mut cc, 17).unwrap(), b"abc");
        assert!(x86_cc_console_bytes(&mut cc, 17).is_err());
        assert!(x86_cc_console_bytes(&mut cc, 17).is_err());
        server.join().unwrap();
    }

    #[test]
    fn x86_oversized_admission_requires_empty_inventory_and_clean_rejection() {
        use std::os::unix::net::UnixListener;
        for (before, reply_words, after, accepted) in [
            (0, [CC_ERR_RELAY_FAULT, 0, 0], 0, true),
            (1, [CC_ERR_RELAY_FAULT, 0, 0], 0, false),
            (0, [CC_OK, 17, 0], 0, false),
            (0, [CC_ERR_RELAY_FAULT, 0, 17], 0, false),
            (0, [CC_ERR_RELAY_FAULT, 17, 0], 0, false),
            (0, [CC_ERR_BAD_HANDLE, 0, 0], 0, false),
            (0, [CC_ERR_RELAY_FAULT, 0, 0], 1, false),
        ] {
            let directory = tempfile::tempdir().unwrap();
            let socket = directory.path().join("cc.sock");
            let listener = UnixListener::bind(&socket).unwrap();
            let server = std::thread::spawn(move || {
                let (mut stream, _) = listener.accept().unwrap();
                mock_cc_sync(&mut stream);
                let mut request = [0u8; CC_REQ_SIZE];
                let mut reply = [0u8; CC_REPLY_SIZE];
                stream.read_exact(&mut request).unwrap();
                assert_eq!(rd32(&request, 0), MSG_CC_LIST_GUESTS);
                wr32(&mut reply, 0, before);
                stream.write_all(&reply).unwrap();
                if before != 0 {
                    return;
                }
                stream.read_exact(&mut request).unwrap();
                assert_eq!(rd32(&request, 0), MSG_CC_CREATE_GUEST);
                assert_eq!(&request[16..18], &[1, VIBEOS_ARCH_X86_64]);
                assert_eq!(rd32(&request, 20), 2052);
                assert_eq!(rd32(&request, 32), 7);
                for (i, value) in reply_words.iter().enumerate() {
                    wr32(&mut reply, i * 4, *value);
                }
                stream.write_all(&reply).unwrap();
                if reply_words != [CC_ERR_RELAY_FAULT, 0, 0] {
                    return;
                }
                stream.read_exact(&mut request).unwrap();
                assert_eq!(rd32(&request, 0), MSG_CC_LIST_GUESTS);
                wr32(&mut reply, 0, after);
                stream.write_all(&reply).unwrap();
            });
            let mut cc = CcClient::connect(&socket).unwrap();
            assert_eq!(x86_reject_oversized_create(&mut cc).is_ok(), accepted);
            server.join().unwrap();
        }
    }

    #[test]
    fn persistence_witness_requires_fresh_write_and_read_only_verification() {
        let directory = tempfile::tempdir().unwrap();
        let root = directory.path().join("guest-state");
        let run = |second| {
            let script = super::persistence_script("cold-boot-test", second)
                .unwrap()
                .replace("/var/lib/agentos", root.to_str().unwrap());
            std::process::Command::new("sh")
                .args(["-c", &script])
                .output()
                .unwrap()
        };
        assert!(!run(true).status.success());
        let written = run(false);
        assert!(written.status.success());
        assert_eq!(written.stdout, b"cold-boot-test\n");
        assert!(!run(false).status.success());
        let retained = run(true);
        assert!(retained.status.success());
        assert_eq!(retained.stdout, b"cold-boot-test\n");
        let file = root.join("persistence-proof");
        std::fs::write(&file, b"corrupted\n").unwrap();
        assert!(!run(true).status.success());
        assert_eq!(std::fs::read(file).unwrap(), b"corrupted\n");
        for token in ["", "unsafe'quote", "two\nlines", "$(command)"] {
            assert!(super::persistence_script(token, false).is_err());
        }
    }

    #[test]
    fn seeded_cold_boot_rejects_changed_profile_image_and_reinitialization() {
        let directory = tempfile::tempdir().unwrap();
        let image = directory.path().join("current.img");
        std::fs::write(&image, b"boot image").unwrap();
        super::seeded_boot_guard(directory.path(), "resolved profile", &image, false).unwrap();
        super::seeded_boot_guard(directory.path(), "resolved profile", &image, true).unwrap();
        assert!(super::seeded_boot_guard(directory.path(), "other profile", &image, true).is_err());
        assert!(
            super::seeded_boot_guard(directory.path(), "resolved profile", &image, false).is_err()
        );
        std::fs::write(&image, b"new! image").unwrap();
        assert!(
            super::seeded_boot_guard(directory.path(), "resolved profile", &image, true).is_err()
        );
        assert_eq!(
            std::fs::read(directory.path().join("seeded-agentos.img")).unwrap(),
            b"boot image"
        );
        assert_eq!(
            std::fs::read_to_string(directory.path().join("seeded-profile.txt")).unwrap(),
            "resolved profile"
        );
    }

    #[test]
    #[cfg(unix)]
    fn scenario_ssh_receipts_preserve_bytes_and_reject_wrong_output_or_exit() {
        use std::os::unix::process::ExitStatusExt;
        let dir = tempfile::tempdir().unwrap();
        for (attempt, code, bytes, expected) in [
            (1, 0, b"Linux\n".as_slice(), true),
            (2, 0, b"FreeBSD\n".as_slice(), false),
            (3, 1, b"Linux\n".as_slice(), false),
            (4, 0, b"Linux\xff\n".as_slice(), false),
        ] {
            let output = std::process::Output {
                status: std::process::ExitStatus::from_raw(code << 8),
                stdout: bytes.to_vec(),
                stderr: b"diagnostic\xff\n".to_vec(),
            };
            assert_eq!(
                super::retain_scenario_ssh_attempt(
                    dir.path(),
                    attempt,
                    "debian",
                    "debian",
                    12222,
                    "Linux",
                    &output
                )
                .unwrap(),
                expected
            );
            let prefix = dir.path().join(format!("attempt-{attempt}"));
            assert_eq!(
                std::fs::read(prefix.with_extension("stdout")).unwrap(),
                bytes
            );
            assert_eq!(
                std::fs::read(prefix.with_extension("stderr")).unwrap(),
                output.stderr
            );
            let receipt: serde_json::Value =
                serde_json::from_slice(&std::fs::read(prefix.with_extension("json")).unwrap())
                    .unwrap();
            assert_eq!(receipt["accepted"], expected);
            assert_eq!(receipt["exit_code"], code);
            assert_eq!(receipt["stdout_sha256"], super::sha256_bytes(bytes));
        }
        assert_eq!(std::fs::read_dir(dir.path()).unwrap().count(), 12);
    }

    #[test]
    fn seeded_recreation_requires_fresh_seed_and_excludes_other_lifecycle_modes() {
        use clap::Parser;
        #[derive(Parser)]
        struct Args {
            #[command(flatten)]
            test: crate::TestArgs,
        }
        assert!(Args::try_parse_from(["test", "--assert-seeded-recreation"]).is_err());
        assert!(
            Args::try_parse_from(["test", "--seed-profile", "--assert-seeded-recreation"]).is_ok()
        );
        for incompatible in [
            "--assert-seeded-cold-boots",
            "--assert-guest-teardown",
            "--keep-running",
            "--no-build",
            "--assert-guest-display",
        ] {
            assert!(
                Args::try_parse_from([
                    "test",
                    "--seed-profile",
                    "--assert-seeded-recreation",
                    incompatible
                ])
                .is_err(),
                "accepted {incompatible}"
            );
        }
    }

    #[test]
    fn authenticated_timing_config_compares_provisioning_paths_but_rejects_resource_changes() {
        use clap::Parser;
        #[derive(Parser)]
        struct Args {
            #[command(flatten)]
            test: crate::TestArgs,
        }
        let live = Args::parse_from([
            "test",
            "--board",
            "qemu_virt_aarch64",
            "--guest-os",
            "ubuntu-live",
            "--assert-live",
            "--assert-agentos-virtio",
        ])
        .test;
        let mut seeded = live.clone();
        seeded.assert_live = false;
        seeded.seeded_ssh_key = Some("identity".into());
        seeded.guest_os = "debian-arm64-nocloud".into();
        let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../guest-profiles");
        let plan = |alias| {
            let path = crate::cmd_guest_profile::resolve_alias(&root, alias).unwrap();
            crate::cmd_guest_profile::host_profile_plan(&root, &path).unwrap()
        };
        let ubuntu = plan("ubuntu-live");
        let mut debian = plan("debian-arm64-nocloud");
        let baseline = super::timing_qemu_config(&live, &ubuntu).unwrap();
        assert_eq!(
            baseline,
            super::timing_qemu_config(&seeded, &debian).unwrap()
        );
        debian.qemu.as_mut().unwrap().memory = "4G".into();
        assert_ne!(
            baseline,
            super::timing_qemu_config(&seeded, &debian).unwrap()
        );
    }

    #[test]
    fn intel_login_prompt_survives_interleaved_cloud_init_output() {
        assert!(super::x86_has_login_prompt(
            "agentos-debian login: ci-info: Authorized keys\r\n"
        ));
        assert!(super::x86_has_login_prompt("debian login:"));
        assert!(super::x86_has_login_prompt(
            "agentos-debian[  335.085526] cloud-init[494]: running 'modules:final'\r\n login: [  336.226354] cloud-init[494]: finished\r\n"
        ));
        for text in [
            "agentos-debian\n login:",
            "agentos-debian[cloud-init] finished\n login:",
            "agentos-debian[  335.085526] incomplete login:",
            "agentos-debian[  335.x] malformed\n login:",
            "[  335.085526] cloud-init: finished\n login:",
        ] {
            assert!(!super::x86_has_login_prompt(text), "{text:?}");
        }
        assert!(!super::x86_has_login_prompt(
            "[1.0] service awaiting login:"
        ));
        assert!(!super::x86_has_login_prompt(" login:"));
        assert!(!super::x86_has_login_prompt("agentos-debian logi"));
    }
    #[test]
    fn retained_host_key_binds_the_original_loopback_endpoint() {
        let dir = tempfile::tempdir().unwrap();
        let file = dir.path().join("known_hosts");
        std::fs::write(&file, "[127.0.0.1]:12224 ssh-ed25519 AAAA\n").unwrap();
        assert_eq!(
            super::x86_retained_host_key(&file, 12224).unwrap(),
            "ssh-ed25519 AAAA"
        );
        assert!(super::x86_retained_host_key(&file, 12225).is_err());
        std::fs::write(&file, "* ssh-ed25519 AAAA\n").unwrap();
        assert!(super::x86_retained_host_key(&file, 12224).is_err());
        std::fs::write(
            &file,
            "[127.0.0.1]:12224 ssh-ed25519 AAAA\n[127.0.0.1]:12224 ssh-ed25519 BBBB\n",
        )
        .unwrap();
        assert!(super::x86_retained_host_key(&file, 12224).is_err());
    }
    #[test]
    fn console_host_key_requires_complete_unambiguous_report() {
        use super::x86_console_host_key;
        assert_eq!(
            x86_console_host_key("ssh-ed25519 AAAA unrelated").unwrap(),
            None
        );
        assert_eq!(
            x86_console_host_key("-----BEGIN SSH HOST KEY KEYS-----\nssh-ed25519 AAAA").unwrap(),
            None
        );
        let report = "-----BEGIN SSH HOST KEY KEYS-----\r\nssh-ed25519 AAAA root@guest\r\n-----END SSH HOST KEY KEYS-----";
        assert_eq!(
            x86_console_host_key(report).unwrap(),
            Some("ssh-ed25519 AAAA".into())
        );
        assert!(x86_console_host_key(&report.replace("AAAA", "AA;AA")).is_err());
        assert!(x86_console_host_key(
            &report.replace("root@guest", "root@guest\nssh-ed25519 BBBB")
        )
        .is_err());
    }
    #[test]
    fn intel_login_requires_prompt_and_retains_console_failures() {
        use std::os::unix::net::UnixListener;
        for (bytes, expected) in [
            (
                b"Debian GNU/Linux 13 debian hvc0\r\ndebian login: ".as_slice(),
                true,
            ),
            (b"Debian GNU/Linux 13\r\ndebian log".as_slice(), false),
            (b"agentos-debian[  335.085526] cloud-init[494]: running\r\n login: ".as_slice(), true),
            (b"agentos-debian[  335.085526] worker: segfault at 7f1234\r\n login: ".as_slice(), false),
            (
                b"Kernel panic - not syncing\r\ndebian login: ".as_slice(),
                false,
            ),
            (
                b"Entering emergency mode\r\ndebian login: ".as_slice(),
                false,
            ),
            (b"reboot: Restarting system\r\n".as_slice(), false),
            (b"reboot: System halted\r\n".as_slice(), false),
            (b"[ 324.2] (udev-worker)[581]: segfault at 7f1234 likely on CPU 1\nagentos-debian login: ".as_slice(), false),
            (b"general protection fault\nagentos-debian login: ".as_slice(), false),
            (b"Oops: kernel fault\nagentos-debian login: ".as_slice(), false),
        ] {
            let temp = tempfile::tempdir().unwrap();
            let socket = temp.path().join("console.sock");
            let log = temp.path().join("qemu.log");
            std::fs::write(&log, "").unwrap();
            let listener = UnixListener::bind(&socket).unwrap();
            let sender = std::thread::spawn(move || {
                let (mut stream, _) = listener.accept().unwrap();
                std::io::Write::write_all(&mut stream, bytes).unwrap();
            });
            let result = super::x86_linux_login(&socket, &log, std::time::Duration::from_secs(2));
            sender.join().unwrap();
            assert_eq!(result.is_ok(), expected, "{result:?}");
            assert_eq!(
                std::fs::read(log.with_extension("console.log")).unwrap(),
                bytes
            );
        }
    }

    #[test]
    fn intel_recreated_console_keeps_original_target_log() {
        let temp = tempfile::tempdir().unwrap();
        let target = temp.path().join("qemu.log");
        let recreated = temp.path().join("qemu.recreated.log");
        std::fs::write(&target, "target running\n").unwrap();
        for (artifact, bytes) in [
            (&target, b"first-guest login: ".as_slice()),
            (&recreated, b"second-guest login: ".as_slice()),
        ] {
            super::x86_linux_login_reader_with_artifacts(
                |out| {
                    out[..bytes.len()].copy_from_slice(bytes);
                    Ok(bytes.len())
                },
                &target,
                artifact,
                std::time::Instant::now() + std::time::Duration::from_secs(2),
                None,
            )
            .unwrap();
            assert_eq!(
                std::fs::read(artifact.with_extension("console.log")).unwrap(),
                bytes
            );
        }
        assert!(!recreated.exists());
        assert_eq!(
            std::fs::read_to_string(&target).unwrap(),
            "target running\n"
        );
        std::fs::write(&target, "x86 VMX EPT proof FAILED\n").unwrap();
        assert!(super::x86_linux_login_reader_with_artifacts(
            |_| panic!("must reject target failure before accepting new console bytes"),
            &target,
            &temp.path().join("failed.log"),
            std::time::Instant::now() + std::time::Duration::from_secs(2),
            None,
        )
        .is_err());
    }

    use super::*;

    #[test]
    fn frame_readiness_wait_is_bounded_and_does_not_retry_errors() {
        let mut calls = 0;
        assert_eq!(
            wait_frame_ready(
                || {
                    calls += 1;
                    Ok((calls == 3, calls as u64))
                },
                Duration::from_secs(1),
                Duration::ZERO
            )
            .unwrap(),
            (3, 3)
        );
        assert_eq!(calls, 3);
        calls = 0;
        assert!(wait_frame_ready(
            || {
                calls += 1;
                anyhow::bail!("malformed reply")
            },
            Duration::from_secs(1),
            Duration::ZERO
        )
        .is_err());
        assert_eq!(calls, 1);
        calls = 0;
        assert!(wait_frame_ready(
            || {
                calls += 1;
                Ok((false, 7))
            },
            Duration::from_secs(1),
            Duration::ZERO
        )
        .is_err());
        assert_eq!(calls, 120);
        assert!(wait_frame_ready(
            || panic!("expired deadline must not probe"),
            Duration::ZERO,
            Duration::ZERO
        )
        .is_err());
    }

    #[test]
    fn frame_readiness_probes_exact_pixels_and_releases_failed_reads() {
        use std::os::unix::net::UnixListener;
        for mode in 0..6 {
            let directory = tempfile::tempdir().unwrap();
            let socket = directory.path().join("cc.sock");
            let listener = UnixListener::bind(&socket).unwrap();
            let server = std::thread::spawn(move || {
                let (mut stream, _) = listener.accept().unwrap();
                mock_cc_sync(&mut stream);
                for operation in 1..=3 {
                    let mut q = [0u8; CC_REQ_SIZE];
                    stream.read_exact(&mut q).unwrap();
                    assert_eq!(rd32(&q, 0), 0x261d);
                    assert_eq!(rd32(&q, 20), operation);
                    assert_eq!(rd32(&q, 4), if operation == 1 { 19 } else { 0 });
                    assert_eq!(rd32(&q, 40), 0);
                    assert_eq!(rd32(&q, 44), if operation == 2 { 8 } else { 0 });
                    assert_eq!(
                        u64::from_le_bytes(q[32..40].try_into().unwrap()),
                        if operation == 1 { 0 } else { 5 }
                    );
                    let mut p = [0u8; CC_REPLY_SIZE];
                    if mode >= 4 {
                        assert_eq!(operation, 1);
                        for (offset, value) in [(4, 40), (8, 3), (12, 1), (16, 1), (20, 3)] {
                            wr32(&mut p, offset, value);
                        }
                        if mode == 5 {
                            p[32] = 1;
                        } // malformed nonempty NO_FRAME
                        stream.write_all(&p).unwrap();
                        return;
                    }
                    let length = if operation == 2 { 8 } else { 0 };
                    for (offset, value) in [
                        (4, 40 + length),
                        (12, 1),
                        (16, 1),
                        (28, length),
                        (48, 2),
                        (52, 1),
                    ] {
                        wr32(&mut p, offset, value);
                    }
                    let cookie: u64 = if operation == 3 {
                        if mode == 3 {
                            5
                        } else {
                            0
                        }
                    } else if operation == 2 && mode == 2 {
                        6
                    } else {
                        5
                    };
                    p[32..40].copy_from_slice(&cookie.to_le_bytes());
                    p[40..48].copy_from_slice(&17u64.to_le_bytes());
                    if operation == 2 && mode != 0 {
                        p[56..64].copy_from_slice(&[17, 34, 51, 0, 68, 85, 102, 0]);
                    }
                    stream.write_all(&p).unwrap();
                }
            });
            let step = cmd_guest_profile::RecipeStep {
                action: "assert-frame-pixels".into(),
                args: [("x", "0"), ("y", "0"), ("rgb", "332211665544")]
                    .into_iter()
                    .map(|(k, v)| (k.into(), v.into()))
                    .collect(),
            };
            let mut cc = CcClient::connect(&socket).unwrap();
            let result = probe_guest_frame_pixels(&mut cc, 19, &[step]);
            if mode == 4 {
                assert_eq!(result.unwrap(), (false, 0));
            } else if mode < 2 {
                assert_eq!(result.unwrap(), (mode == 1, 17));
            } else {
                assert!(result.is_err());
            }
            server.join().unwrap();
        }
    }

    #[test]
    fn frame_pixel_proof_rejects_wrong_colors_bounds_and_malformed_expectations() {
        let mut step = cmd_guest_profile::RecipeStep {
            action: "assert-frame-pixels".into(),
            args: [("x", "0"), ("y", "1"), ("rgb", "332211665544")]
                .into_iter()
                .map(|(k, v)| (k.into(), v.into()))
                .collect(),
        };
        let mut pixels = vec![0; 16];
        pixels[8..].copy_from_slice(&[17, 34, 51, 0, 68, 85, 102, 255]);
        assert_eq!(
            verify_frame_pixels(&pixels, 2, 2, &[step.clone()]).unwrap(),
            2
        );
        pixels[8] = 18;
        assert!(verify_frame_pixels(&pixels, 2, 2, &[step.clone()]).is_err());
        pixels[8] = 17;
        assert!(verify_frame_pixels(&pixels[..15], 2, 2, &[step.clone()]).is_err());
        step.args.insert("x".into(), "1".into());
        assert!(verify_frame_pixels(&pixels, 2, 2, &[step.clone()]).is_err());
        step.args.insert("x".into(), "-1".into());
        assert!(cmd_guest_profile::frame_pixel_expectation(&step).is_err());
        step.args.insert("x".into(), "0".into());
        step.args.insert("rgb".into(), "zz2211".into());
        assert!(cmd_guest_profile::frame_pixel_expectation(&step).is_err());
    }

    #[test]
    fn profile_build_enables_selected_devices_for_single_and_secondary_guests() {
        let root = repo_root().unwrap();
        let profiles = root.join("guest-profiles");
        let load =
            |name: &str| cmd_guest_profile::host_profile_plan(&profiles, Path::new(name)).unwrap();
        let input = load("debian-input.toml");
        let gpu = load("debian-gpu.toml");
        let headless = load("debian.toml");
        assert_eq!(
            profile_device_build_args(None, None),
            ["GUEST_GRAPHICS=", "GUEST_INPUT="]
        );
        assert_eq!(
            profile_device_build_args(Some(&headless), None),
            ["GUEST_GRAPHICS=", "GUEST_INPUT="]
        );
        assert_eq!(
            profile_device_build_args(Some(&input), None),
            ["GUEST_GRAPHICS=", "GUEST_INPUT=1"]
        );
        assert_eq!(
            profile_device_build_args(Some(&gpu), None),
            ["GUEST_GRAPHICS=1", "GUEST_INPUT="]
        );
        let mut scenario =
            guest_scenario::resolve_alias(&root.join("guest-scenarios"), &profiles, "both")
                .unwrap();
        scenario.guests[0].profile = gpu;
        scenario.guests[1].profile = input;
        assert_eq!(
            profile_device_build_args(None, Some(&scenario)),
            ["GUEST_GRAPHICS=1", "GUEST_INPUT=1"]
        );
    }

    #[test]
    fn input_probe_output_is_bounded_and_requires_complete_utf8_lines() {
        assert_eq!(
            input_probe_line(&mut &b"AGENTOS_INPUT_READY\n"[..]).unwrap(),
            "AGENTOS_INPUT_READY"
        );
        assert!(input_probe_line(&mut &b"AGENTOS_INPUT_READY"[..]).is_err());
        assert!(input_probe_line(&mut &[b'x'; 129][..]).is_err());
        assert!(input_probe_line(&mut &b"\xff\n"[..]).is_err());
    }
    use std::os::unix::net::UnixListener;

    fn console_stress_fixture(newline: &[u8]) -> Vec<u8> {
        let mut stream = b"prior console output\nAOS_STRESS_BEGIN".to_vec();
        stream.extend_from_slice(newline);
        stream.extend(
            (0..CONSOLE_STRESS_BYTES)
                .map(|index| b'A' + ((index ^ (index >> 8) ^ (index >> 16)) & 15) as u8),
        );
        stream.extend_from_slice(newline);
        stream.extend_from_slice(b"AOS_STRESS_END\n");
        stream
    }

    #[test]
    fn console_stress_accepts_exact_payload_with_lf_or_crlf() {
        let lf = verify_console_stress_stream(&console_stress_fixture(b"\n")).unwrap();
        let crlf = verify_console_stress_stream(&console_stress_fixture(b"\r\n")).unwrap();
        assert_eq!(lf.len(), 64);
        assert_eq!(lf, crlf);
    }

    #[test]
    fn console_stress_rejects_truncation_duplication_and_corruption() {
        let stream = console_stress_fixture(b"\n");
        let mut truncated = stream.clone();
        truncated.remove(4096);
        assert!(verify_console_stress_stream(&truncated).is_err());
        let mut duplicated = stream.clone();
        duplicated.insert(4096, stream[4096]);
        assert!(verify_console_stress_stream(&duplicated).is_err());
        let mut corrupted = stream;
        corrupted[65536] ^= 1;
        assert!(verify_console_stress_stream(&corrupted).is_err());
    }

    #[test]
    fn console_stress_rejects_missing_or_malformed_framing() {
        for stream in [
            b"".as_slice(),
            b"AOS_STRESS_BEGIN",
            b"AOS_STRESS_BEGINjunk\nAOS_STRESS_END",
        ] {
            assert!(verify_console_stress_stream(stream).is_err());
        }
        let mut stream = console_stress_fixture(b"\n");
        stream.truncate(stream.len() - b"AOS_STRESS_END\n".len());
        assert!(verify_console_stress_stream(&stream).is_err());
    }

    fn test_profile(alias: &str) -> HostProfilePlan {
        let repo = Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap();
        let root = repo.join("guest-profiles");
        let path = cmd_guest_profile::resolve_alias(&root, alias).unwrap();
        cmd_guest_profile::host_profile_plan(&root, &path).unwrap()
    }

    fn test_provision_commands(alias: &str, key: &str) -> Vec<String> {
        profile_provision_commands(&test_profile(alias), key).unwrap()
    }

    #[test]
    fn profile_ssh_override_matches_qemu_forwarding_and_preserves_guest_identity() {
        let listener = std::net::TcpListener::bind(("127.0.0.1", 0)).unwrap();
        let port = listener.local_addr().unwrap().port();
        drop(listener);
        let mut profile = test_profile("freebsd");
        apply_profile_ssh_port(&mut profile, port);
        let ssh = profile.qemu.as_ref().unwrap().ssh.as_ref().unwrap();
        assert_eq!(ssh.host_port, port);
        assert_eq!(ssh.account, "root");
        assert_eq!(ssh.guest_address.as_deref(), Some("10.0.2.16"));
        let netdev = qemu_netdev_arg(ssh.host_port, Some(&profile), None).unwrap();
        assert_eq!(
            netdev,
            format!("user,id=net0,hostfwd=tcp:127.0.0.1:{port}-10.0.2.16:22")
        );
    }

    #[test]
    fn frame_reply_requires_exact_payload_and_bounded_geometry() {
        let mut reply = CcReply {
            mr: [CC_OK, 48, 0, 1],
            shmem: vec![0; 4096],
        };
        wr32(&mut reply.shmem, 0, 1);
        wr32(&mut reply.shmem, 12, 8);
        reply.shmem[16..24].copy_from_slice(&5u64.to_le_bytes());
        reply.shmem[24..32].copy_from_slice(&17u64.to_le_bytes());
        wr32(&mut reply.shmem, 32, 1024);
        wr32(&mut reply.shmem, 36, 768);
        assert_eq!(decode_frame_reply(&reply, 8).unwrap(), (5, 17, 1024, 768));
        assert!(decode_frame_reply(&reply, 4).is_err());
        wr32(&mut reply.shmem, 32, u32::MAX);
        assert!(decode_frame_reply(&reply, 8).is_err());
        wr32(&mut reply.shmem, 32, 1024);
        reply.shmem.truncate(47);
        assert!(decode_frame_reply(&reply, 8).is_err());
        reply.shmem.resize(4096, 0);
        reply.mr[2] = 3;
        assert!(decode_frame_reply(&reply, 8).is_err());
    }

    #[test]
    fn zero_ssh_override_preserves_profile_default() {
        let mut profile = test_profile("freebsd");
        apply_profile_ssh_port(&mut profile, 0);
        assert_eq!(
            profile
                .qemu
                .as_ref()
                .unwrap()
                .ssh
                .as_ref()
                .unwrap()
                .host_port,
            12223
        );
    }

    #[test]
    fn text_events_fit_the_narrowest_relay_and_reassemble() {
        let input = b"abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHI";
        let mut output = Vec::new();
        let mut frame_count = 0;
        for chunk in input.chunks(CC_INPUT_TEXT_CHUNK) {
            let event = cc_text_event(chunk);
            assert!(8 + event.len() <= VMM_RELAY_PAYLOAD_BYTES);
            assert_eq!(rd32(&event, 0), CC_INPUT_TEXT);
            assert_eq!(rd32(&event, 4) as usize, chunk.len());
            output.extend_from_slice(&event[24..]);
            frame_count += 1;
        }
        assert_eq!(frame_count, 3);
        assert_eq!(output, input);
    }

    #[test]
    fn input_session_wire_and_reply_contract() {
        let query = input_session_request(false, "pointer", &["2", "1", "-9"]).unwrap();
        assert_eq!(rd32(&query, 0), 1);
        assert_eq!(rd32(&query, 12), 1);
        assert_eq!(rd32(&query, 16), 2);
        assert_eq!(&query[32..40], &[2, 0, 1, 0, 247, 255, 255, 255]);
        assert!(query[40..].iter().all(|v| *v == 0));
        let mut reply = CcReply {
            mr: [CC_OK, 16, 0, 1],
            shmem: vec![0; 16],
        };
        wr32(&mut reply.shmem, 0, 1);
        wr32(&mut reply.shmem, 12, 2);
        assert_eq!(validate_input_session_reply(&reply, &query).unwrap(), 0);
        wr32(&mut reply.shmem, 12, 1);
        assert!(validate_input_session_reply(&reply, &query).is_err());
        reply.mr[2] = 3;
        wr32(&mut reply.shmem, 8, 3);
        wr32(&mut reply.shmem, 12, 0);
        assert_eq!(validate_input_session_reply(&reply, &query).unwrap(), 3);
        wr32(&mut reply.shmem, 4, 1);
        assert!(validate_input_session_reply(&reply, &query).is_err());
        let release = input_session_request(true, "keyboard", &[]).unwrap();
        assert_eq!(rd32(&release, 0), 2);
        assert!(release[4..].iter().all(|v| *v == 0));
        assert!(input_session_request(true, "keyboard", &["1", "183", "1"]).is_err());
        assert!(input_session_request(false, "keyboard", &[]).is_err());
        assert!(input_session_request(false, "pointer", &["1", "272", "2147483648"]).is_err());
    }

    #[test]
    fn cc_client_never_replays_after_lost_reply() {
        let dir = tempfile::tempdir().expect("temporary CC socket directory");
        let socket = dir.path().join("cc.sock");
        let listener = UnixListener::bind(&socket).expect("bind CC test socket");
        let server = std::thread::spawn(move || {
            let (mut first, _) = listener.accept().expect("accept first CC connection");
            mock_cc_sync(&mut first);
            let mut first_request = [0u8; CC_REQ_SIZE];
            first
                .read_exact(&mut first_request)
                .expect("read first CC request");
            drop(first);

            assert_eq!(rd32(&first_request, 0), MSG_CC_SEND_INPUT);
            listener.set_nonblocking(true).unwrap();
            let deadline = Instant::now() + Duration::from_millis(200);
            while Instant::now() < deadline {
                match listener.accept() {
                    Ok(_) => panic!("ambiguous request opened a replay connection"),
                    Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => {}
                    Err(error) => panic!("CC listener failed: {error}"),
                }
                std::thread::sleep(Duration::from_millis(1));
            }
        });

        let mut client = CcClient::connect(&socket).expect("connect CC test client");
        assert!(client
            .call(MSG_CC_SEND_INPUT, 7, 0, 0, b"lost-reply-regression")
            .is_err());
        assert!(client.is_closed());
        assert!(client
            .call(MSG_CC_SEND_INPUT, 7, 0, 0, b"second-call")
            .is_err());
        server.join().expect("join CC test server");
    }

    #[test]
    fn dual_host_forwards_have_distinct_guest_addresses() {
        let repo = Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap();
        let scenario = guest_scenario::resolve_alias(
            &repo.join("guest-scenarios"),
            &repo.join("guest-profiles"),
            "both",
        )
        .unwrap();
        let netdev = scenario_qemu_netdev_arg(&scenario);
        assert!(netdev.contains("127.0.0.1:12222-10.0.2.15:22"));
        assert!(netdev.contains("127.0.0.1:12223-10.0.2.16:22"));
    }

    #[test]
    fn seeded_followup_commands_keep_strict_host_identity() {
        let key = SshTestKey {
            _temporary_dir: None,
            private_key: "identity".into(),
            public_key: String::new(),
            known_hosts: Some("retained known_hosts".into()),
        };
        let mut command = std::process::Command::new("ssh");
        apply_test_ssh_identity(&mut command, &key);
        let args: Vec<_> = command.get_args().map(|s| s.to_str().unwrap()).collect();
        for option in [
            "StrictHostKeyChecking=yes",
            "IdentityAgent=none",
            "UserKnownHostsFile=retained known_hosts",
            "GlobalKnownHostsFile=/dev/null",
            "PasswordAuthentication=no",
            "KbdInteractiveAuthentication=no",
        ] {
            assert!(args.contains(&option));
        }
        assert!(!args.contains(&"StrictHostKeyChecking=no"));
    }

    #[test]
    fn ssh_provisioning_commands_are_posix_shell_syntax() {
        let key = "ssh-ed25519 AAAAC3NzaFocusedTest agentos-test";
        let ubuntu = test_provision_commands("ubuntu-live", key);
        let freebsd = test_provision_commands("freebsd", key);
        let commands = ubuntu.clone().into_iter().chain(freebsd.clone());
        for command in commands {
            let status = std::process::Command::new("sh")
                .args(["-n", "-c", &command])
                .status()
                .expect("run sh syntax check");
            assert!(status.success());
        }
        assert!(ubuntu.iter().any(|command| command.contains(key)));
        assert!(freebsd.iter().any(|command| command.contains(key)));
    }

    #[test]
    fn freebsd_provisioning_recovers_read_only_live_media_tmp() {
        let command =
            test_provision_commands("freebsd", "ssh-ed25519 AAAAC3NzaFocusedTest agentos-test")
                .join("; ");
        let probe = command
            .find("mkdir -p /tmp/agentos-ssh 2>/dev/null")
            .expect("writable directory probe");
        let mount = command
            .find("mount -t tmpfs tmpfs /tmp")
            .expect("tmpfs fallback");
        let keygen = command.find("ssh-keygen").expect("host key generation");
        assert!(probe < mount && mount < keygen);
    }

    #[test]
    fn provisioning_markers_cannot_match_command_echo() {
        let key = "ssh-ed25519 AAAAC3NzaFocusedTest agentos-test";
        for profile in [test_profile("ubuntu-live"), test_profile("freebsd")] {
            let command = profile_provision_commands(&profile, key)
                .unwrap()
                .join("; ");
            let wrapped = guest_provision_command(&command, &profile.id, "ready");
            assert!(!wrapped.contains(&format!("agentos-{}-ssh-ready", profile.id)));
            assert!(!wrapped.contains(&format!("agentos-{}-ssh-failed", profile.id)));
            assert!(wrapped.contains(&format!("agentos-{}-ssh-%s\\n' failed", profile.id)));
        }
    }

    #[test]
    fn live_network_probe_waits_for_post_probe_marker() {
        let profile = test_profile("ubuntu-live");
        let command = profile.console.probe_line.as_deref().unwrap();
        assert!(command.contains("ip link set eth0 up"));
        assert!(command.contains("ping -6 -c1 -W1 ff02::1%eth0"));
        assert!(command.contains("printf 'agentos-live-net-%s\\n' proof"));
        assert!(!command.contains("agentos-live-net-proof"));
        assert_eq!(
            profile.console.probe_marker.as_deref(),
            Some("agentos-live-net-proof")
        );
    }

    #[test]
    fn buildroot_network_probe_requires_successful_roundtrip() {
        let profile = test_profile("buildroot");
        let command = profile.console.probe_line.as_deref().unwrap();
        let marker = profile.console.probe_marker.as_deref().unwrap();
        assert!(
            !command.contains(marker),
            "command echo must not satisfy the proof"
        );
        for (configure, ping, success) in [(0, 0, true), (1, 0, false), (0, 1, false)] {
            let script = format!(
                "ifconfig() {{ return {configure}; }}; ping() {{ return {ping}; }}; {command}"
            );
            let output = std::process::Command::new("sh")
                .args(["-c", &script])
                .output()
                .unwrap();
            assert_eq!(output.status.success(), success);
            assert_eq!(
                String::from_utf8(output.stdout).unwrap().contains(marker),
                success
            );
        }
    }

    #[test]
    fn ssh_ready_markers_require_key_only_daemon_startup() {
        let key = "ssh-ed25519 AAAAC3NzaFocusedTest agentos-test";
        let ubuntu = test_provision_commands("ubuntu-live", key).join("; ");
        assert!(ubuntu.contains("/cdrom/pool/main/o/openssh/openssh-server_*.deb"));
        assert!(ubuntu.contains("dpkg-deb -x"));
        assert!(ubuntu.contains("ssh-keygen -q -t ed25519"));
        assert!(ubuntu.contains("HostKey=/run/agentos-ssh-host-key"));
        for command in [ubuntu, test_provision_commands("freebsd", key).join("; ")] {
            assert!(command.contains("PasswordAuthentication=no"));
            assert!(command.contains("KbdInteractiveAuthentication=no"));
            assert!(command.contains("PubkeyAuthentication=yes"));
            assert!(!command.contains("sshd || true"));
        }
    }

    #[test]
    fn ssh_probes_have_bounded_connection_and_session_liveness() {
        let auth = SSH_AUTH_OPTIONS.join(" ");
        let probe = SSH_PROBE_LIVENESS_OPTIONS.join(" ");
        let session = SSH_SESSION_LIVENESS_OPTIONS.join(" ");
        assert!(auth.contains("BatchMode=yes"));
        assert!(auth.contains("PreferredAuthentications=publickey"));
        assert!(auth.contains("ConnectionAttempts=1"));
        assert!(auth.contains("ConnectTimeout=30"));
        assert!(probe.contains("ServerAliveInterval=5"));
        assert!(probe.contains("ServerAliveCountMax=1"));
        assert!(session.contains("ServerAliveInterval=30"));
        assert!(session.contains("ServerAliveCountMax=20"));
    }

    #[test]
    fn desktop_proof_is_lightweight_and_confined_to_ssh() {
        let profile = test_profile("ubuntu-live");
        let desktop = profile.desktop.as_ref().unwrap();
        let script = desktop.provision_script.as_str();
        assert!(std::process::Command::new("sh")
            .args(["-n", "-c", script])
            .status()
            .expect("run desktop shell syntax check")
            .success());
        assert!(script.contains("tigervnc-standalone-server_1.15.0+dfsg-2build1_arm64.deb"));
        assert!(script.contains("30e536f05a504ad817b294abee51394143"));
        assert!(script.contains("sha256sum -c"));
        assert!(script.contains("dpkg-deb -x"));
        assert!(script.contains("Xtigervnc :1"));
        assert!(script.contains("-rendernode \"\""));
        assert!(script.contains("gnome-calculator"));
        assert!(!script.contains("apt-get"));
        assert!(script.contains("-rfbunixpath /tmp/agentos-vnc.sock"));
        assert!(script.contains("-SecurityTypes None"));
        assert!(script.contains("agentos-desktop-local-rfb-ready"));
        assert_eq!(
            desktop_tunnel_forward_spec(desktop),
            "127.0.0.1:15901:/tmp/agentos-vnc.sock"
        );
        assert_eq!(
            desktop_tunnel_forward_spec(test_profile("debian").desktop.as_ref().unwrap()),
            "127.0.0.1:15902:127.0.0.1:5901"
        );
    }

    #[test]
    fn desktop_script_templates_host_time_without_guest_policy() {
        let rendered =
            render_desktop_script("guest-clock-command {{host_unix_time}}", 1_789_056_000).unwrap();
        assert_eq!(rendered, "guest-clock-command 1789056000");
        assert!(render_desktop_script("{{guest_specific}}", 1).is_err());
    }

    #[test]
    fn manual_dual_ssh_commands_use_persistent_key_and_distinct_ports() {
        let repo = Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap();
        let scenario = guest_scenario::resolve_alias(
            &repo.join("guest-scenarios"),
            &repo.join("guest-profiles"),
            "both",
        )
        .unwrap();
        let commands =
            manual_ssh_commands(Path::new("build/tmp/dual-ssh/id_ed25519"), &scenario).unwrap();
        assert!(commands
            .iter()
            .any(|command| command.contains("-p 12222") && command.contains("debian@127.0.0.1")));
        assert!(commands
            .iter()
            .any(|command| command.contains("-p 12223") && command.contains("root@127.0.0.1")));
        for command in commands {
            assert!(command.contains("build/tmp/dual-ssh/id_ed25519"));
            assert!(command.contains("IdentitiesOnly=yes"));
            assert!(command.contains("StrictHostKeyChecking=no"));
            assert!(command.contains("UserKnownHostsFile=/dev/null"));
        }
    }

    #[test]
    fn console_recovery_and_rejection_policy_comes_from_profile() {
        let profile = test_profile("freebsd");
        let transcript = "ld-elf.so.1: /lib/libedit.so.8: Unsupported version 0 \
                          of Elf_Verneed entry\nEnter full pathname of shell \
                          or RETURN for /bin/sh:";
        let rescue = profile.console.interaction.iter().any(|rule| {
            rule.when.iter().all(|marker| transcript.contains(marker))
                && rule.send == "/rescue/sh\r"
        });
        assert!(rescue);
        assert!(reject_profile_console(Some(&profile.console), transcript).is_ok());
        assert!(reject_profile_console(
            Some(&profile.console),
            "mountroot>\nManual root filesystem specification:"
        )
        .is_err());
    }

    #[test]
    fn console_rules_require_all_markers_and_bound_retries() {
        let profile = test_profile("freebsd");
        let rescue = profile
            .console
            .interaction
            .iter()
            .find(|rule| {
                rule.when
                    .iter()
                    .any(|marker| marker == "Unsupported version")
            })
            .unwrap();
        assert_eq!(
            available_console_interactions(
                rescue,
                "Enter full pathname of shell",
                Duration::from_secs(60)
            ),
            0
        );
        assert_eq!(
            available_console_interactions(
                rescue,
                "Unsupported version\nEnter full pathname of shell",
                Duration::from_secs(60)
            ),
            1
        );

        let login = profile
            .console
            .interaction
            .iter()
            .find(|rule| rule.when == ["login:"])
            .unwrap();
        assert_eq!(
            available_console_interactions(
                login,
                "login: timed out\nlogin:",
                Duration::from_secs(60)
            ),
            2
        );
        assert_eq!(login.max_fires, 4);
    }

    #[test]
    fn cc_frame_deadline_bounds_lost_chardev_recovery() {
        assert_eq!(CC_FRAME_DEADLINE, Duration::from_secs(60));
    }

    #[test]
    fn framed_read_preserves_progress_across_socket_timeouts() {
        let (mut reader, mut writer) = UnixStream::pair().expect("create socket pair");
        reader
            .set_read_timeout(Some(Duration::from_millis(10)))
            .expect("set short test timeout");
        let expected: Vec<u8> = (0u8..32u8).collect();
        let send = expected.clone();
        let writer_thread = std::thread::spawn(move || {
            writer.write_all(&send[..7]).expect("write first fragment");
            std::thread::sleep(Duration::from_millis(30));
            writer.write_all(&send[7..]).expect("write second fragment");
        });

        let mut received = [0u8; 32];
        read_cc_frame(&mut reader, &mut received).expect("read fragmented frame");
        writer_thread.join().expect("join writer");
        assert_eq!(received.as_slice(), expected.as_slice());
    }
}
