# Guest session acceptance

## Why the old SSH gate was insufficient

The old `spawn_ssh_probe` ran `ssh ... account@127.0.0.1 'uname -s'`.
Both the standalone and dual-guest callers required exit status zero and the
expected kernel name (`Linux` or `FreeBSD`). This was a real authenticated SSH
exec request, but it did not request a terminal or start a login shell. Success
discarded stdout/stderr instead of retaining them. Consequently the historical
dual-guest receipts qualify that narrow operation, not interactive usability.

On 2026-09-25 the unmodified probe/profile path at `69489f251826cabb9153f0872ee6d1cc66ed2db9`
was reproduced on macOS AArch64. Ubuntu returned `Linux` and status 0 for
`ssh -T ... 'uname -s'`; a PTY login failed with status 255. `/dev/pts` was
unmounted, `/dev/pts/ptmx` and the journal stream socket were absent, and PID 1
was still `/bin/sh /init noprompt`. FreeBSD returned UID 0, a read-only DVD
root, and `pkg: Insufficient privileges to add packages` with status 1.
The image SHA-256 was
`d03bb6581d11295c67e7b2ddc656e12cbdb9f5aaefc3a21b7057ce3840ac6e3e`.
Diagnostic transcripts and the image are retained in
`build/evidence/ssh-audit-baseline/`. The source tree had roadmap edits at
launch; this is a reproduction record, not a clean-revision release receipt.

Ubuntu's live profile stops Casper in a chroot before normal init startup.
Its previous provisioning did not mount devpts or start a journal service.
An SSH exec request can therefore work while PTY allocation and login startup
fail. FreeBSD's live profile mounts its DVD as a read-only cd9660 root; UID 0
cannot make the package database or installation prefix writable there.

MAC tracks this audit in `task_e49d2ed2bb3c49339cfb4f6657cf0a62`, Ubuntu repair
in `task_dfc0f3dc0b8e4275b36cfe4e74e92fcc`, and FreeBSD writable package state
in `task_3086b8c0ea594264a8fea160d57fcef7`.

## Required functional session

`make demo-test` and standalone live-profile proofs must pass the original
remote-exec readiness check followed by `guest_session::prove`. The latter
opens `ssh -tt` with **no remote command**, exercising the account's login
shell startup. A local PTY supplies a 24-by-80 window; the runner waits for
the profile's login prompt before sending input. This avoids FreeBSD's
`resizewin -z` consuming early commands while querying a zero-sized terminal.
Each check runs through `/bin/sh -c` inside that login session for portable
commands. One session then checks:

1. stdin/stdout are terminals; `tty` and `stty` work.
2. Account identity and kernel match the profile.
3. A temporary directory supports create, copy, exact byte comparison, line
   count, background child execution/wait, readback, and deletion; `df` and
   `ps` execute successfully.
4. The profile's network commands reach the host gateway and resolve a
   distribution repository hostname.
5. The profile's package commands install a package, query its database entry,
   execute its binary, remove it, and verify removal.

FreeBSD uses the DVD's `cmark` package. Ubuntu and Debian build a disposable
`.deb` containing the guest's own `getconf` executable, install it with `dpkg`,
verify its output, and purge it. This is a package-manager/storage proof; it
does not qualify downloading dependencies from a remote package repository.

Every step must produce a fresh exact-line challenge response absent from
the input command, so terminal echo cannot satisfy acceptance. Nonzero exit,
missing response, PTY failure, journal stream errors, excessive output, or the
host wall-clock deadline fail the test. SSH keepalives alone are insufficient
because a responsive server can still run a hung command.

Commands, terminal output, profile identity, and pass/fail status are retained
under `build/evidence/guest-session-*`. The outer QEMU test retains its normal
image/serial evidence. A host test is not guest execution evidence.

To rerun against a retained demonstration:

```sh
make test-guest-session SESSION_PROFILE=ubuntu-live.toml SESSION_PORT=12222
make test-guest-session SESSION_PROFILE=freebsd.toml SESSION_PORT=12223
```

`SESSION_KEY` defaults to `build/tmp/dual-ssh/id_ed25519`; `SESSION_TIMEOUT`
sets the total session deadline in seconds. These tests install and remove
their named test package and are intended for disposable qualification guests.

Current v0.4 main uses pinned Debian/FreeBSD for the default demonstration.
Its `debian-scenario.toml` profile has the same functional checks, using the
`debian` account and the seed's pinned SSH host identity. For a retained seeded
guest, supply `SESSION_KNOWN_HOSTS` with the runner's retained known-hosts file;
the functional runner rejects seeded profiles without it. The older Ubuntu
scenario and standalone Ubuntu live test retain the functional requirement.
Other seeded lifecycle/display proofs keep their existing specialized checks;
their receipts alone do not qualify this login-session contract.

## Live storage and runtime limits

Ubuntu provisioning establishes devpts, cgroup2 and a working journal stream
before starting SSH. Without cgroup2, journald can create sockets and then
exit; a successful `systemd-cat` write is required, not socket existence alone.
This remains a Casper live chroot, not a claim that normal
multi-user systemd boot completed.

FreeBSD preserves the DVD contents of `/etc`, `/var`, and `/usr/local` in
bounded tmpfs mounts and configures DNS. Packages and configuration are
**ephemeral** and consume guest RAM. The DVD root remains read-only; neither
`mount -u -w /` nor changing user identity converts it into persistent storage.
Persistent installation and large package workloads require a writable disk
profile and their own boot/storage qualification.
The DVD's `/etc/resolv.conf` is an installer-only symlink to
`/tmp/bsdinstall_etc/resolv.conf`; provisioning replaces it in the writable
copy of `/etc`.
