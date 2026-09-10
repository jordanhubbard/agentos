use crate::cmd_guest_profile::{self, DesktopPlan, HostProfilePlan};
use crate::guest_scenario::{self, HostScenarioPlan, ScenarioGuestPlan};
use crate::{rfb, QemuLaunchArgs, TestArgs};
use anyhow::Context;
use std::io::{Read, Seek, SeekFrom, Write};
use std::net::{SocketAddr, TcpStream};
use std::ops::{Deref, DerefMut};
use std::os::unix::net::UnixStream;
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Child, Stdio};
use std::time::{Duration, Instant};

const SSH_AUTH_OPTIONS: &[&str] = &[
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
const SSH_PROBE_LIVENESS_OPTIONS: &[&str] =
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
 * dynamic guest.  The 24-byte event plus a 4-byte Vibe handle and a 4-byte VM
 * slot must all fit the 48-byte seL4 payload, leaving 16 text bytes per frame.
 */
const CC_INPUT_TEXT_CHUNK: usize = 16;
const CC_REQ_SIZE: usize = 4 + 12 + CC_WIRE_SHMEM_SIZE;
const CC_REPLY_SIZE: usize = 16 + CC_WIRE_SHMEM_SIZE;
const CC_IO_TIMEOUT: Duration = Duration::from_secs(5);
/*
 * A console drain crosses the host virtconsole, CC-PD, vibe_engine,
 * vm_manager, and a running VMM. Those target components now have a strictly
 * ascending priority chain, so a full minute without frame progress means the
 * QEMU chardev lost the request or reply. Reconnect and replay the identical
 * request; CC-PD's retry cache makes completed state changes exactly-once.
 */
const CC_FRAME_DEADLINE: Duration = Duration::from_secs(60);
const CC_INPUT_RETRY_DEADLINE: Duration = Duration::from_secs(120);
const CC_OK: u32 = 0;
const CC_ERR_RELAY_FAULT: u32 = 8;
#[cfg(test)]
const VMM_RELAY_PAYLOAD_BYTES: usize = 48;
const MSG_CC_LOG_STREAM: u32 = 0x2610;
const MSG_CC_CREATE_GUEST: u32 = 0x2611;
const MSG_CC_SEND_INPUT: u32 = 0x260d;
const MSG_CC_SUSPEND_GUEST: u32 = 0x2613;
const MSG_CC_RESUME_GUEST: u32 = 0x2614;
const MSG_CC_DESTROY_GUEST: u32 = 0x2615;
const CC_INPUT_KEY_DOWN: u32 = 0x01;
const CC_INPUT_RAW_BYTE_BASE: u32 = 0x100;
const GUEST_DESTROY_NORMAL: u32 = 0;
const VIBEOS_ARCH_AARCH64: u8 = 0x01;
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
    if args.assert_agentos_virtio || args.assert_live || args.assert_desktop {
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
                        "emulated virtio-net: backend TX accepted by net_pd",
                        "emulated virtio-net: backend RX delivered from net_pd",
                        "via host-backed net_pd",
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
                        "emulated virtio-blk: agentOS host media",
                        "emulated virtio-blk: host-media read",
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
                }
            }
            _ => {}
        }
    }
    required
}

pub fn run(args: &TestArgs) -> anyhow::Result<()> {
    let repo_root = repo_root()?;
    let profile_root = repo_root.join("guest-profiles");
    let scenario_plan = if args.guest_os == "both" {
        Some(guest_scenario::resolve_alias(
            &repo_root.join("guest-scenarios"),
            &profile_root,
            &args.guest_os,
        )?)
    } else {
        None
    };
    let profile_plan = if matches!(args.guest_os.as_str(), "none" | "both") {
        None
    } else {
        let path = cmd_guest_profile::resolve_alias(&profile_root, &args.guest_os)?;
        Some(cmd_guest_profile::host_profile_plan(&profile_root, &path)?)
    };
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
        !args.keep_running || args.guest_os == "both" || args.assert_desktop,
        "--keep-running requires --guest-os both or --assert-desktop"
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
            profile_plan
                .as_ref()
                .is_some_and(|profile| profile.media_initrd_path.is_some()),
            "live-media assertions require a profile with boot.media_initrd_path"
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
        if scenario_plan.is_some() {
            make_args.push(format!("GUEST_SCENARIO={}", args.guest_os));
        } else if let Some(profile) = &profile_plan {
            make_args.push(format!("GUEST_PROFILE={}", profile.path.display()));
        }
        let make_arg_refs = make_args.iter().map(String::as_str).collect::<Vec<_>>();
        run_make(&make_arg_refs, &repo_root).context("profile-driven build step failed")?;
    }

    let tmp_dir = repo_root.join("build/tmp");
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
    let ssh_key = if scenario_plan.is_some() || args.assert_desktop {
        Some(generate_ssh_test_key(&repo_root, args.keep_running)?)
    } else {
        None
    };

    // Every runtime profile reaches its emulated NIC through the slot's
    // net_pd bridge. Stimulate RX even for the focused "emulated" assertion;
    // otherwise a quiet guest can negotiate the device correctly and then
    // wait forever without exercising a queue.
    let needs_host_net_stimulus = virtio_assertion
        .as_ref()
        .is_some_and(|assertion| assertion.devices.iter().any(|device| device == "net"));
    let ssh_port = if needs_host_net_stimulus || args.assert_desktop || scenario_plan.is_some() {
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
    )?);
    if needs_host_net_stimulus {
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

    let mut result = if args.assert_emulated_net {
        println!(
            "[xtask:test] Waiting for emulated virtio-net guest proof in {}...",
            log_path.display()
        );
        wait_for_emulated_net(&log_path, Duration::from_secs(args.timeout_secs), &mut qemu)
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
            wait_for_dual_guest_consoles_via_cc(
                &cc_sock,
                scenario_plan.as_ref().unwrap(),
                Duration::from_secs(args.timeout_secs),
                &mut qemu,
                ssh_key.as_ref().context("dual SSH key was not generated")?,
                args.keep_running,
            )
        } else if profile_plan
            .as_ref()
            .is_some_and(|profile| !profile_console_markers(profile).is_empty())
        {
            let profile = profile_plan.as_ref().unwrap();
            println!(
                "[xtask:test] Waiting for profile {} console evidence via CC-PD API ({})...",
                profile.id,
                cc_sock.display()
            );
            wait_for_guest_console_login_via_cc(
                &cc_sock,
                0,
                args.guest_os.as_str(),
                Some(profile),
                Duration::from_secs(args.timeout_secs),
                &mut qemu,
            )
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

    /*
     * Start the authenticated desktop path before checking host-backed network
     * markers.  Its SSH connection is the RX stimulus.  Connecting to the
     * forwarded port before sshd exists leaves a stale user-net flow that can
     * accept later host sockets without ever completing an SSH banner.
     */
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
        } else {
            wait_for_manual_dual_ssh(
                key,
                scenario_plan
                    .as_ref()
                    .context("manual scenario requires a resolved plan")?,
                &mut qemu,
            )
            .map(|()| String::from("manual dual SSH session completed"))
        };
    }

    if let Some(mut tunnel) = desktop_tunnel {
        let _ = tunnel.kill();
        let _ = tunnel.wait();
    }
    let _ = qemu.kill();
    let _ = qemu.wait();

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

pub fn launch(args: &QemuLaunchArgs) -> anyhow::Result<()> {
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

    let selection = if let Some(profile) = &profile_plan {
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
    if let Some(profile) = &profile_plan {
        make_args.push(format!("GUEST_PROFILE={}", profile.path.display()));
    } else if let Some(alias) = &args.scenario {
        make_args.push(format!("GUEST_SCENARIO={alias}"));
    }
    let make_arg_refs = make_args.iter().map(String::as_str).collect::<Vec<_>>();
    run_make(&make_arg_refs, &repo_root).context("profile-driven build step failed")?;

    let tmp_dir = repo_root.join("build/tmp");
    std::fs::create_dir_all(&tmp_dir)
        .with_context(|| format!("failed to create {}", tmp_dir.display()))?;
    let log_path = tmp_dir.join("agentos-run.log");
    let cc_sock = repo_root.join("build/cc_pd.sock");
    let ssh_port = profile_plan
        .as_ref()
        .and_then(|profile| profile.qemu.as_ref())
        .and_then(|qemu| qemu.ssh.as_ref())
        .map(|ssh| ssh.host_port)
        .unwrap_or(0);
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
    )?;
    let status = qemu.wait().context("failed to wait for QEMU")?;
    anyhow::ensure!(status.success(), "QEMU exited with {status}");
    Ok(())
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
    let tmp_dir = repo_root.join("build/tmp");
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
    })
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

pub(crate) fn sel4_sdk_path() -> anyhow::Result<PathBuf> {
    if let Some(path) = std::env::var_os("SEL4_SDK") {
        return Ok(PathBuf::from(path));
    }

    let home = std::env::var_os("HOME").context(
        "SEL4_SDK is unset and HOME is unavailable; set SEL4_SDK to the external Microkit SDK",
    )?;
    Ok(PathBuf::from(home).join(".cache/agentos/microkit-sdk-2.1.0"))
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
                    "file={},format=raw,id={},if=none,readonly=on,file.locking=off",
                    media_path.display(),
                    media.drive_id
                ),
            ]);
        }
    }
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
) -> anyhow::Result<std::process::Child> {
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
            let cpu = if use_kvm {
                "host"
            } else if fast {
                "max"
            } else {
                "cortex-a57"
            };
            let serial = if interactive_serial {
                String::from("stdio")
            } else {
                format!("file:{}", log_path.display())
            };
            let mut c = std::process::Command::new("qemu-system-aarch64");
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
                .arg("virtconsole,bus=vser0.0,chardev=cc_pd_char,name=cc.0")
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
            .process_group(0)
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

fn host_kvm_available(board: &str) -> bool {
    cfg!(all(target_os = "linux", target_arch = "aarch64"))
        && board == "qemu_virt_aarch64"
        && Path::new("/dev/kvm").exists()
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

pub struct CcReply {
    pub mr: [u32; 4],
    pub shmem: Vec<u8>,
}

struct CcClient {
    stream: Option<UnixStream>,
    socket_path: PathBuf,
}

impl CcClient {
    fn connect(cc_sock: &Path) -> anyhow::Result<Self> {
        let stream = Self::connect_stream(cc_sock)?;
        Ok(Self {
            stream: Some(stream),
            socket_path: cc_sock.to_path_buf(),
        })
    }

    fn connect_stream(cc_sock: &Path) -> anyhow::Result<UnixStream> {
        let stream = UnixStream::connect(cc_sock)
            .with_context(|| format!("failed to connect to {}", cc_sock.display()))?;
        stream
            .set_read_timeout(Some(CC_IO_TIMEOUT))
            .context("failed to set CC socket read timeout")?;
        stream
            .set_write_timeout(Some(CC_IO_TIMEOUT))
            .context("failed to set CC socket write timeout")?;
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

        let first_error = match self.call_frame(&req) {
            Ok(reply) => return Ok(reply),
            Err(error) => error,
        };
        self.stream.take();

        /*
         * A CC operation can complete before the socket-backed VirtIO reply
         * becomes writable.  Dropping the old frontend lets QEMU accept a new
         * connection; resending the identical frame is safe because CC-PD
         * records state-changing replies before resetting a stalled TX queue.
         */
        let stream = Self::connect_stream(&self.socket_path).with_context(|| {
            format!("CC reconnect after incomplete reply failed; first error: {first_error:#}")
        })?;
        self.stream = Some(stream);
        self.call_frame(&req).with_context(|| {
            format!("CC replay after incomplete reply failed; first error: {first_error:#}")
        })
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
    Ok(format!(
        "CC console API saw {guest_os} handle {guest_handle} prompt {:?} and {proof}",
        prompt
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
        cc_send_console_line(cc, guest_handle, probe.as_bytes())?;
    } else {
        cc_send_raw_bytes(cc, guest_handle, probe.as_bytes())?;
    }

    let mut echo = String::new();
    let start = Instant::now();
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
    ram_mb: u32,
) -> anyhow::Result<Result<u32, (u32, u32)>> {
    let mut shmem = [0u8; 52];
    shmem[0] = os_type;
    shmem[1] = VIBEOS_ARCH_AARCH64;
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

        match try_create_guest_via_cc(cc, os_type, ram_mb) {
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
    let mut child = std::process::Command::new("ssh");
    child
        .arg("-i")
        .arg(private_key)
        .args(["-p", &port.to_string()])
        .args(SSH_AUTH_OPTIONS)
        .args(SSH_SESSION_LIVENESS_OPTIONS)
        .args([
            &format!("{account}@127.0.0.1"),
            &format!("sudo -n timeout {} sh -s", timeout.as_secs()),
        ])
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
        .context("desktop profile has no host.qemu.ssh plan")?;
    let (account, marker) = profile_ssh_expectation(profile)?;
    let start = Instant::now();
    let mut last = String::from("no SSH attempt completed");
    while start.elapsed() < timeout {
        ensure_qemu_running(qemu, "waiting for profile desktop SSH")?;
        let probe = spawn_ssh_probe(&ssh_key.private_key, ssh.host_port, account)?;
        let output = probe
            .wait_with_output()
            .context("failed to wait for profile desktop SSH probe")?;
        if output.status.success()
            && (marker.is_empty() || String::from_utf8_lossy(&output.stdout).trim() == marker)
        {
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
    anyhow::bail!("profile desktop SSH did not become ready: {last}")
}

fn desktop_tunnel_forward_spec(desktop: &DesktopPlan) -> String {
    format!(
        "127.0.0.1:{}:127.0.0.1:{}",
        desktop.local_port, desktop.guest_port
    )
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
    let provisioning = run_ssh_script(
        &ssh_key.private_key,
        ssh.host_port,
        &ssh.account,
        Duration::from_secs(desktop.provision_timeout_secs),
        &desktop.provision_script,
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
) -> anyhow::Result<std::process::Child> {
    std::process::Command::new("ssh")
        .args([
            "-i",
            private_key
                .to_str()
                .context("SSH private key path is not UTF-8")?,
            "-p",
            &port.to_string(),
        ])
        .args(SSH_AUTH_OPTIONS)
        .args(SSH_PROBE_LIVENESS_OPTIONS)
        .args([&format!("{user}@127.0.0.1"), "uname -s"])
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .with_context(|| format!("failed to launch SSH probe for {user} on port {port}"))
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

fn wait_for_scenario_guest_ssh(
    guest: &ScenarioGuestPlan,
    ssh_key: &SshTestKey,
    timeout: Duration,
    qemu: &mut Child,
) -> anyhow::Result<()> {
    let (account, marker) = profile_ssh_expectation(&guest.profile)?;
    let start = Instant::now();
    let mut last = String::from("no SSH attempt completed");
    while start.elapsed() < timeout {
        ensure_qemu_running(qemu, "waiting for profile authenticated SSH")?;
        let probe = spawn_ssh_probe(&ssh_key.private_key, guest.ssh_host_port, account)?;
        let output = probe
            .wait_with_output()
            .with_context(|| format!("failed to wait for {} SSH probe", guest.profile.id))?;
        let stdout = String::from_utf8_lossy(&output.stdout);
        if output.status.success() && (marker.is_empty() || stdout.trim() == marker) {
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
) -> anyhow::Result<String> {
    let start = Instant::now();
    while start.elapsed() < timeout {
        ensure_qemu_running(qemu, "waiting for scenario authenticated SSH")?;
        let mut all_ready = true;
        for guest in &scenario.guests {
            if wait_for_scenario_guest_ssh(guest, ssh_key, Duration::from_secs(35), qemu).is_err() {
                all_ready = false;
                break;
            }
        }
        if all_ready {
            return Ok(format!(
                "scenario {} has concurrent authenticated SSH for {} profiles",
                scenario.id,
                scenario.guests.len()
            ));
        }
        std::thread::sleep(Duration::from_secs(2));
    }
    anyhow::bail!(
        "scenario {} authenticated SSH did not become ready",
        scenario.id
    )
}

fn wait_for_dual_guest_consoles_via_cc(
    cc_sock: &Path,
    scenario: &HostScenarioPlan,
    timeout: Duration,
    qemu: &mut Child,
    ssh_key: &SshTestKey,
    keep_running: bool,
) -> anyhow::Result<String> {
    let start = Instant::now();
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
        lead.ram_mb,
        &lead.profile.id,
        create_timeout,
        qemu,
    )
    .with_context(|| format!("failed to create {} through vm_manager", lead.profile.id))?;

    let deferred_handle = create_guest_via_cc_wait(
        &mut boot_cc,
        deferred.profile.control_type as u8,
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
    let deferred_boot_suspend = suspend_guest_via_cc(&mut boot_cc, deferred_handle)
        .with_context(|| format!("failed to defer {} boot", deferred.profile.id))?;
    println!(
        "[xtask:test] suspended {} handle={} state={} while {} boots",
        deferred.profile.id, deferred_handle, deferred_boot_suspend, lead.profile.id
    );

    let lead_console = wait_for_guest_console_login_on_cc(
        cc_sock,
        &mut boot_cc,
        lead_handle,
        &lead.profile.id,
        Some(&lead.profile),
        timeout.saturating_sub(start.elapsed()),
        qemu,
    )?;
    let lead_provision = profile_provision_commands(&lead.profile, &ssh_key.public_key)?;
    run_guest_console_commands(
        cc_sock,
        &mut boot_cc,
        lead_handle,
        &lead.profile.id,
        Some(&lead.profile),
        &lead_provision,
        Duration::from_secs(600),
        qemu,
    )?;
    wait_for_scenario_guest_ssh(lead, ssh_key, Duration::from_secs(180), qemu)
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
    wait_for_scenario_guest_ssh(lead, ssh_key, Duration::from_secs(180), qemu)
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

    let deferred_console = wait_for_guest_console_login_on_cc(
        cc_sock,
        &mut boot_cc,
        deferred_handle,
        &deferred.profile.id,
        Some(&deferred.profile),
        timeout.saturating_sub(start.elapsed()),
        qemu,
    )?;
    let deferred_provision = profile_provision_commands(&deferred.profile, &ssh_key.public_key)?;
    run_guest_console_commands(
        cc_sock,
        &mut boot_cc,
        deferred_handle,
        &deferred.profile.id,
        Some(&deferred.profile),
        &deferred_provision,
        Duration::from_secs(600),
        qemu,
    )?;
    resume_guest_via_cc(&mut boot_cc, lead_handle)
        .with_context(|| format!("failed to resume provisioned {}", lead.profile.id))?;
    let ssh = wait_for_scenario_ssh(scenario, ssh_key, Duration::from_secs(600), qemu)?;

    if !keep_running {
        destroy_guest_via_cc(&mut boot_cc, deferred_handle)
            .with_context(|| format!("failed to destroy {}", deferred.profile.id))?;
        destroy_guest_via_cc(&mut boot_cc, lead_handle)
            .with_context(|| format!("failed to destroy {}", lead.profile.id))?;
    }

    Ok(format!(
        "scenario {} consoles ready: {} handle={} ({}); {} handle={} ({}); {ssh}; {}",
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

fn cc_log_stream_for_handle(
    cc: &mut CcClient,
    guest_handle: u32,
    profile: Option<&HostProfilePlan>,
) -> anyhow::Result<String> {
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
    let reply = cc
        .call(MSG_CC_LOG_STREAM, guest_handle, pd_id, 0, &[])
        .context("MSG_CC_LOG_STREAM failed")?;
    anyhow::ensure!(
        reply.mr[0] == CC_OK,
        "MSG_CC_LOG_STREAM returned ok={}",
        reply.mr[0]
    );
    let len = (reply.mr[1] as usize).min(reply.shmem.len());
    Ok(String::from_utf8_lossy(&reply.shmem[..len]).into_owned())
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

fn destroy_guest_via_cc(cc: &mut CcClient, guest_handle: u32) -> anyhow::Result<()> {
    let reply = cc
        .call(
            MSG_CC_DESTROY_GUEST,
            guest_handle,
            GUEST_DESTROY_NORMAL,
            0,
            &[],
        )
        .context("MSG_CC_DESTROY_GUEST failed")?;
    anyhow::ensure!(
        reply.mr[0] == CC_OK,
        "MSG_CC_DESTROY_GUEST returned ok={}",
        reply.mr[0]
    );
    Ok(())
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
    use super::*;
    use std::os::unix::net::UnixListener;

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
    fn cc_client_replays_identical_frame_after_lost_reply() {
        let dir = tempfile::tempdir().expect("temporary CC socket directory");
        let socket = dir.path().join("cc.sock");
        let listener = UnixListener::bind(&socket).expect("bind CC test socket");
        let server = std::thread::spawn(move || {
            let (mut first, _) = listener.accept().expect("accept first CC connection");
            let mut first_request = [0u8; CC_REQ_SIZE];
            first
                .read_exact(&mut first_request)
                .expect("read first CC request");
            drop(first);

            let (mut replay, _) = listener.accept().expect("accept replay CC connection");
            let mut replay_request = [0u8; CC_REQ_SIZE];
            replay
                .read_exact(&mut replay_request)
                .expect("read replayed CC request");
            assert_eq!(replay_request, first_request);

            let mut reply = [0u8; CC_REPLY_SIZE];
            wr32(&mut reply, 0, CC_OK);
            wr32(&mut reply, 4, 0x51a7e);
            replay.write_all(&reply).expect("write replayed CC reply");
        });

        let mut client = CcClient::connect(&socket).expect("connect CC test client");
        let reply = client
            .call(MSG_CC_SEND_INPUT, 7, 0, 0, b"lost-reply-regression")
            .expect("replay CC call after lost reply");
        assert_eq!(reply.mr[0], CC_OK);
        assert_eq!(reply.mr[1], 0x51a7e);
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
        assert!(script.contains("-localhost yes"));
        assert!(script.contains("-SecurityTypes None"));
        assert!(script.contains("agentos-desktop-local-rfb-ready"));
        assert!(script.contains("/dev/tcp/127.0.0.1/5901"));
        assert_eq!(
            desktop_tunnel_forward_spec(desktop),
            "127.0.0.1:15901:127.0.0.1:5901"
        );
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
            .any(|command| command.contains("-p 12222") && command.contains("ubuntu@127.0.0.1")));
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
