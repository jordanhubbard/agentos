//! Functional guest acceptance over an actual SSH login shell and terminal.
use crate::cmd_guest_profile::{self, HostProfilePlan};
use anyhow::{ensure, Context, Result};
use std::fs::{self, File};
use std::io::Write;
use std::os::fd::{AsRawFd, FromRawFd};
use std::path::{Path, PathBuf};
#[cfg(test)]
use std::process::Stdio;
use std::process::{Child, Command};
use std::time::{Duration, Instant};

#[derive(Clone, clap::Args)]
pub struct GuestSessionArgs {
    #[arg(long)]
    pub profile: PathBuf,
    #[arg(long)]
    pub key: PathBuf,
    #[arg(long)]
    pub port: u16,
    /// Pinned host identity, required for seeded profiles.
    #[arg(long)]
    pub known_hosts: Option<PathBuf>,
    #[arg(long, default_value_t = 600)]
    pub timeout_secs: u64,
}

pub fn run(args: &GuestSessionArgs) -> Result<()> {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).parent().unwrap();
    let profile =
        cmd_guest_profile::host_profile_plan(&root.join("guest-profiles"), &args.profile)?;
    prove(
        &profile,
        &args.key,
        args.port,
        args.known_hosts.as_deref(),
        Duration::from_secs(args.timeout_secs),
    )
}

fn quote(value: &str) -> String {
    format!("'{}'", value.replace('\'', "'\\''"))
}

fn steps(profile: &HostProfilePlan) -> Result<Vec<(String, String)>> {
    let ssh = profile
        .test
        .iter()
        .find(|s| s.action == "wait-ssh")
        .context("functional session requires wait-ssh account and kernel marker")?;
    let account = &ssh.args["account"];
    let kernel = ssh
        .args
        .get("marker")
        .filter(|s| !s.is_empty())
        .context("functional session requires an expected kernel")?;
    let mut steps = vec![
        ("terminal".into(), "test -t 0; test -t 1; tty; stty -a".into()),
        ("identity".into(), format!("test \"$(id -un)\" = {}; test \"$(uname -s)\" = {}; id; uname -a", quote(account), quote(kernel))),
        ("files-processes".into(), "d=$(mktemp -d /tmp/agentos-session.XXXXXX); trap 'rm -rf \"$d\"' EXIT; printf '%s\\n' alpha beta gamma >\"$d/in\"; cp \"$d/in\" \"$d/copy\"; cmp \"$d/in\" \"$d/copy\"; test \"$(wc -l <\"$d/in\" | tr -d ' ')\" = 3; (cat \"$d/in\" >\"$d/out\") & p=$!; wait \"$p\"; cmp \"$d/in\" \"$d/out\"; rm \"$d/copy\"; test ! -e \"$d/copy\"; df -h / /tmp; ps -p $$".into()),
    ];
    for name in ["network", "packages"] {
        let checks: Vec<_> = profile
            .test
            .iter()
            .filter(|s| {
                s.action == "ssh-check" && s.args.get("name").map(String::as_str) == Some(name)
            })
            .collect();
        ensure!(
            checks.len() == 1,
            "profile {} requires exactly one {name} ssh-check",
            profile.id
        );
        steps.push((name.into(), checks[0].args["script"].clone()));
    }
    Ok(steps)
}

fn has_marker(output: &str, marker: &str) -> bool {
    output
        .lines()
        .any(|line| line.trim_end_matches('\r') == marker)
}

fn check_diagnostics(output: &str) -> Result<()> {
    for error in [
        "PTY allocation request failed",
        "Failed to create stream fd",
        "Permission denied (publickey)",
    ] {
        ensure!(!output.contains(error), "guest session diagnostic: {error}");
    }
    Ok(())
}

// A file avoids pipe deadlocks. Bound both elapsed time and output size even
// when the SSH server stays responsive but the command never finishes.
struct Session {
    child: Child,
    input: Box<dyn Write>,
    transcript: PathBuf,
    deadline: Instant,
}

impl Drop for Session {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

impl Session {
    fn output(&self) -> Result<String> {
        ensure!(
            fs::metadata(&self.transcript)?.len() <= 1024 * 1024,
            "SSH transcript exceeded 1 MiB"
        );
        let output = fs::read_to_string(&self.transcript)?;
        check_diagnostics(&output)?;
        Ok(output)
    }

    fn send(&mut self, command: &str) -> Result<()> {
        // Stay below the guest terminal's canonical input limit.
        ensure!(
            command.len() < 1000 && !command.contains(['\n', '\r']),
            "SSH acceptance command exceeds terminal line bound"
        );
        let stdin = &mut self.input;
        writeln!(stdin, "{command}")?;
        stdin.flush()?;
        Ok(())
    }

    fn wait_marker(&mut self, marker: &str) -> Result<()> {
        loop {
            let output = self.output()?;
            if has_marker(&output, marker) {
                return Ok(());
            }
            if let Some(status) = self.child.try_wait()? {
                let output = self.output()?;
                if has_marker(&output, marker) {
                    return Ok(());
                }
                anyhow::bail!("SSH exited {status} before {marker}: {output}");
            }
            ensure!(
                Instant::now() < self.deadline,
                "SSH session deadline waiting for {marker}"
            );
            std::thread::sleep(Duration::from_millis(50));
        }
    }

    fn finish(&mut self) -> Result<()> {
        loop {
            self.output()?;
            if let Some(status) = self.child.try_wait()? {
                self.output()?;
                ensure!(status.success(), "SSH session exited {status}");
                return Ok(());
            }
            ensure!(
                Instant::now() < self.deadline,
                "SSH session failed to exit before deadline"
            );
            std::thread::sleep(Duration::from_millis(50));
        }
    }

    fn wait_login(&mut self, prompt: &str) -> Result<()> {
        loop {
            if self.output()?.ends_with(prompt) {
                return Ok(());
            }
            if let Some(status) = self.child.try_wait()? {
                anyhow::bail!("SSH login exited {status} before prompt {prompt:?}");
            }
            ensure!(Instant::now() < self.deadline, "SSH login prompt deadline");
            std::thread::sleep(Duration::from_millis(50));
        }
    }
}

fn terminal_input() -> Result<(File, File)> {
    let mut master = -1;
    let mut slave = -1;
    let mut size = libc::winsize {
        ws_row: 24,
        ws_col: 80,
        ws_xpixel: 0,
        ws_ypixel: 0,
    };
    // openpty returns two owned descriptors. Passing a winsize lets OpenSSH
    // advertise a real terminal, including to FreeBSD's login resizewin -z.
    let result = unsafe {
        libc::openpty(
            &mut master,
            &mut slave,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            &mut size,
        )
    };
    if result != 0 {
        return Err(std::io::Error::last_os_error()).context("allocate SSH input PTY");
    }
    let pair = unsafe { (File::from_raw_fd(master), File::from_raw_fd(slave)) };
    for file in [&pair.0, &pair.1] {
        if unsafe { libc::fcntl(file.as_raw_fd(), libc::F_SETFD, libc::FD_CLOEXEC) } == -1 {
            return Err(std::io::Error::last_os_error()).context("set PTY close-on-exec");
        }
    }
    Ok(pair)
}

pub(crate) fn prove(
    profile: &HostProfilePlan,
    key: &Path,
    port: u16,
    known_hosts: Option<&Path>,
    timeout: Duration,
) -> Result<()> {
    ensure!(
        profile.seed.is_none() || known_hosts.is_some(),
        "seeded session requires pinned host identity"
    );
    let steps = steps(profile)?;
    let account = &profile
        .test
        .iter()
        .find(|s| s.action == "wait-ssh")
        .unwrap()
        .args["account"];
    let evidence = Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .unwrap()
        .join("_build/evidence");
    fs::create_dir_all(&evidence)?;
    let directory = tempfile::Builder::new()
        .prefix("guest-session-")
        .tempdir_in(evidence)?
        .keep();
    let nonce = directory.file_name().unwrap().to_str().unwrap();
    println!(
        "[xtask:test] {} functional SSH evidence: {}",
        profile.id,
        directory.display()
    );
    fs::write(
        directory.join("profile.txt"),
        format!("profile={}\nport={port}\naccount={account}\n", profile.id),
    )?;
    let transcript = directory.join("terminal.log");
    let output = File::create(&transcript)?;
    let (input, terminal) = terminal_input()?;
    let prompt = profile
        .test
        .iter()
        .find(|s| s.action == "wait-ssh")
        .unwrap()
        .args
        .get("prompt")
        .filter(|p| !p.is_empty())
        .context("functional session requires a login prompt")?;
    let mut command = Command::new("ssh");
    crate::cmd_test::apply_ssh_identity(&mut command, known_hosts);
    let child = command
        .arg("-tt")
        .arg("-i")
        .arg(key)
        .args(["-p", &port.to_string()])
        // Login/PAM and package work can stall server replies under emulation.
        // Use session keepalives; our own deadline still bounds all progress.
        .args(crate::cmd_test::SSH_SESSION_LIVENESS_OPTIONS)
        // No remote command: sshd must start the account's login shell.
        .arg(format!("{account}@127.0.0.1"))
        .env("TERM", "dumb")
        .stdin(terminal)
        .stdout(output.try_clone()?)
        .stderr(output)
        .spawn()?;
    let mut session = Session {
        child,
        input: Box::new(input),
        transcript,
        deadline: Instant::now() + timeout,
    };
    let result = (|| -> Result<()> {
        session.wait_login(prompt)?;
        // The complete marker never appears in input, so terminal echo cannot pass.
        session.send(&format!("stty -echo rows 24 columns 80 && printf '\\nagentos-%s-%s\\n' {nonce} ready || exit 91"))?;
        session.wait_marker(&format!("agentos-{nonce}-ready"))?;
        for (name, script) in &steps {
            fs::write(directory.join(format!("{name}.command")), script)?;
            let script = quote(&format!("set -eu; {script}"));
            session.send(&format!(
                "/bin/sh -c {script} && printf '\\nagentos-%s-%s\\n' {nonce} {name} || exit 91"
            ))?;
            session.wait_marker(&format!("agentos-{nonce}-{name}"))?;
            println!("[xtask:test] {} SSH {name}: PASS", profile.id);
        }
        session.send("exit 0")?;
        session.finish()
    })();
    fs::write(
        directory.join("result.txt"),
        match &result {
            Ok(()) => "PASS\n".into(),
            Err(error) => format!("FAIL: {error:#}\n"),
        },
    )?;
    result.with_context(|| {
        format!(
            "functional SSH failed for {}; evidence {}",
            profile.id,
            directory.display()
        )
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn echoed_commands_and_wrong_challenges_are_not_results() {
        let marker = "agentos-unique-terminal";
        assert!(!has_marker(
            "printf 'agentos-%s-%s' unique terminal\r\n",
            marker
        ));
        assert!(!has_marker("prompt$ agentos-unique-terminal\r\n", marker));
        assert!(!has_marker("agentos-old-terminal\r\n", marker));
        assert!(has_marker("\r\nagentos-unique-terminal\r\n", marker));
        assert!(
            check_diagnostics("Failed to create stream fd: No such file or directory").is_err()
        );
        assert!(check_diagnostics("PTY allocation request failed on channel 0").is_err());
    }

    fn local_session(command: &str, timeout: Duration) -> (tempfile::TempDir, Session) {
        let directory = tempfile::tempdir().unwrap();
        let transcript = directory.path().join("output");
        let output = File::create(&transcript).unwrap();
        let mut child = Command::new("sh")
            .args(["-c", command])
            .stdin(Stdio::piped())
            .stdout(output.try_clone().unwrap())
            .stderr(output)
            .spawn()
            .unwrap();
        (
            directory,
            Session {
                input: Box::new(child.stdin.take().unwrap()),
                child,
                transcript,
                deadline: Instant::now() + timeout,
            },
        )
    }

    #[test]
    fn ssh_input_has_real_terminal_dimensions() {
        let (_master, slave) = terminal_input().unwrap();
        let output = Command::new("stty")
            .arg("size")
            .stdin(slave)
            .output()
            .unwrap();
        assert!(output.status.success());
        assert_eq!(String::from_utf8(output.stdout).unwrap().trim(), "24 80");
    }

    #[test]
    fn failed_commands_and_missing_results_fail_acceptance() {
        let (_directory, mut session) = local_session("exit 7", Duration::from_secs(2));
        assert!(session
            .wait_marker("missing")
            .unwrap_err()
            .to_string()
            .contains("exited"));
        let (_directory, mut session) =
            local_session("printf 'proof\\n'; exit 7", Duration::from_secs(2));
        session.wait_marker("proof").unwrap();
        assert!(session.finish().is_err());
        let (_directory, mut session) = local_session(
            "sh -c 'set -eu; false; printf unexpected' && printf proof || exit 91",
            Duration::from_secs(2),
        );
        assert!(session.wait_marker("proof").is_err());
        assert!(!session.output().unwrap().contains("unexpected"));
    }

    #[test]
    fn login_requires_prompt_before_sending_commands() {
        let (_directory, mut session) =
            local_session("printf 'banner\\n'; read line", Duration::from_millis(100));
        assert!(session.wait_login("$ ").is_err());
        let (_directory, mut session) =
            local_session("printf 'user$ '; read line", Duration::from_secs(2));
        session.wait_login("$ ").unwrap();
    }

    #[test]
    fn responsive_process_cannot_outlive_wall_clock_deadline() {
        let (_directory, mut session) = local_session("read line", Duration::from_millis(100));
        assert!(session
            .wait_marker("missing")
            .unwrap_err()
            .to_string()
            .contains("deadline"));
    }

    #[test]
    fn successful_command_requires_result_and_exit_status() {
        let (_directory, mut session) = local_session("printf 'proof\\n'", Duration::from_secs(2));
        session.wait_marker("proof").unwrap();
        session.finish().unwrap();
    }

    #[test]
    fn every_runtime_ssh_profile_has_bounded_functional_recipes() {
        let root = Path::new(env!("CARGO_MANIFEST_DIR"))
            .parent()
            .unwrap()
            .join("guest-profiles");
        for name in [
            "ubuntu-live.toml",
            "freebsd.toml",
            "debian.toml",
            "debian-scenario.toml",
            "debian-amd64.toml",
            "arch-amd64.toml",
            "arch-amd64-desktop.toml",
        ] {
            let profile = cmd_guest_profile::host_profile_plan(&root, Path::new(name)).unwrap();
            let checks = steps(&profile).unwrap();
            assert_eq!(checks.len(), 5);
            for (_, script) in checks {
                // Allow the SSH marker wrapper and POSIX quoting expansion.
                assert!(
                    quote(&format!("set -eu; {script}")).len() + 140 < 1000,
                    "{name}"
                );
                assert!(Command::new("sh")
                    .args(["-n", "-c", &script])
                    .status()
                    .unwrap()
                    .success());
            }
            let mut incomplete = profile;
            incomplete.test.retain(|s| s.action != "ssh-check");
            assert!(steps(&incomplete).is_err());
        }
    }
}
