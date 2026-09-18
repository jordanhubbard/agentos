// Public library surface — shared types and command implementations.
// The binary entry point (src/main.rs) re-uses everything from here.

pub mod cmd_ci_matrix;
pub mod cmd_extract_freebsd_file;
pub mod cmd_fault_inject;
pub mod cmd_fetch_guest;
pub mod cmd_gen_abi;
pub mod cmd_gen_caps;
pub mod cmd_gen_channels;
pub mod cmd_gen_image;
pub mod cmd_gen_pd_bundle;
pub mod cmd_gen_policy;
pub mod cmd_guest_profile;
pub mod cmd_guest_timing;
pub mod cmd_host_test;
pub mod cmd_policy_check;
pub mod cmd_release;
pub mod cmd_render_deck;
pub mod cmd_run_tests;
pub mod cmd_seed_guest;
pub mod cmd_setup;
pub mod cmd_test;
pub mod cmd_test_api;
pub mod guest_scenario;
mod persistent_media;
pub mod rfb;

// ── Re-exports for main.rs ────────────────────────────────────────────────
pub use cmd_gen_image::GenImageArgs;
pub use cmd_gen_pd_bundle::GenPdBundleArgs;
pub use cmd_guest_profile::GuestProfileArgs;
pub use cmd_guest_timing::GuestTimingCompareArgs;
pub use guest_scenario::GuestScenarioArgs;

// ── Subcommand arg structs ──────────────────────────────────────────────────

#[derive(Clone, clap::Args)]
pub struct TestArgs {
    /// Prove a disk witness survives two fresh QEMU boots of one live profile.
    #[arg(long, requires = "assert_live", conflicts_with_all = ["no_build", "keep_running", "assert_desktop"])]
    pub assert_persistent_boots: bool,
    #[arg(skip)]
    pub persistent_directory: Option<std::path::PathBuf>,
    #[arg(skip)]
    pub persistent_second_boot: bool,
    #[arg(skip)]
    pub persistent_token: String,
    /// Query the root-provisioned boot snapshot through CC and agentctl.
    #[arg(long)]
    pub assert_inspect: bool,
    /// Exercise the native read-only operator protocol through serial_virt.
    #[arg(long)]
    pub assert_operator_session: bool,
    /// Verify native generic logs through root-provisioned rings and serial_pd.
    #[arg(long, conflicts_with_all = ["operator_isolation_probe", "inspect_write_probe"])]
    pub assert_log_rings: bool,
    /// Native log client access faults: server ring read/write, config write.
    #[arg(long, value_parser = clap::value_parser!(u8).range(1..=3), conflicts_with_all = ["assert_log_rings", "operator_isolation_probe", "inspect_write_probe", "assert_operator_session", "assert_native_rust", "assert_native_guest", "serial_isolation_probe", "network_isolation_probe", "block_isolation_probe"])]
    pub log_isolation_probe: Option<u8>,
    /// Native operator denied reads/writes of guest and frontend pages, and snapshot writes.
    #[arg(long, conflicts_with_all = ["assert_operator_session", "assert_inspect", "inspect_write_probe", "assert_native_rust", "assert_native_guest", "serial_isolation_probe", "network_isolation_probe", "block_isolation_probe", "virtualizer_authority_probe"], value_parser = clap::value_parser!(u8).range(1..=7))]
    pub operator_isolation_probe: Option<u8>,
    /// Verify CC faults when attempting to write its read-only boot snapshot.
    #[arg(long, conflicts_with_all = ["assert_inspect", "assert_native_rust", "assert_native_guest", "serial_isolation_probe", "network_isolation_probe", "block_isolation_probe", "virtualizer_authority_probe"])]
    pub inspect_write_probe: bool,
    /// Boot a no_std Rust PD and verify its IPC contract from a separate C PD.
    #[arg(long, conflicts_with_all = ["serial_isolation_probe", "network_isolation_probe", "block_isolation_probe", "virtualizer_authority_probe"])]
    pub assert_native_rust: bool,
    /// Qualify framebuffer queue transactions from two isolated native clients.
    #[arg(long, conflicts_with_all = ["assert_native_rust", "assert_native_guest", "assert_inspect", "inspect_write_probe", "assert_operator_session", "operator_isolation_probe", "assert_log_rings", "log_isolation_probe", "serial_isolation_probe", "network_isolation_probe", "block_isolation_probe", "virtualizer_authority_probe", "assert_vmx_exit"])]
    pub assert_framebuffer: bool,
    /// Configure QEMU ramfb through the dedicated display driver.
    #[arg(
        long,
        requires = "assert_framebuffer",
        conflicts_with = "framebuffer_isolation_probe"
    )]
    pub assert_display: bool,
    /// Compare a graphics guest's exported frame against QEMU RAMFB scanout.
    #[arg(long, conflicts_with = "assert_display")]
    pub assert_guest_display: bool,
    /// Verify framebuffer clients cannot access peer queues or private/observer storage.
    #[arg(long, requires = "assert_framebuffer", value_parser = clap::value_parser!(u8).range(1..=16))]
    pub framebuffer_isolation_probe: Option<u8>,
    /// Qualify fresh native NIC traffic interleaved with a live Ubuntu guest.
    #[arg(long, conflicts_with_all = ["assert_native_rust", "serial_isolation_probe", "network_isolation_probe", "block_isolation_probe", "virtualizer_authority_probe"])]
    pub assert_native_guest: bool,
    /// Native client denied mappings: read/write guest pages, driver page, MMIO, DMA.
    #[arg(long, requires = "assert_native_rust", value_parser = clap::value_parser!(u8).range(1..=10))]
    pub native_network_isolation_probe: Option<u8>,
    /// Test serial mapping isolation: primary cases 1..4, secondary cases 5..8.
    #[arg(long, conflicts_with_all = ["network_isolation_probe", "block_isolation_probe", "virtualizer_authority_probe"], value_parser = clap::value_parser!(u8).range(1..=8))]
    pub serial_isolation_probe: Option<u8>,
    /// Test network mapping isolation: primary cases 1..4, secondary cases 5..8.
    #[arg(long, conflicts_with_all = ["block_isolation_probe", "virtualizer_authority_probe"], value_parser = clap::value_parser!(u8).range(1..=8))]
    pub network_isolation_probe: Option<u8>,
    /// Test VMM attachment authority: 1 primary, 2 secondary.
    #[arg(long, conflicts_with = "block_isolation_probe", value_parser = clap::value_parser!(u8).range(1..=2))]
    pub virtualizer_authority_probe: Option<u8>,
    /// Test-only VMM fault probe: 1..4 primary foreign/disk read/write; 5..8 secondary.
    #[arg(long, value_parser = clap::value_parser!(u8).range(1..=8))]
    pub block_isolation_probe: Option<u8>,
    /// Test-only root failure: 1 missing GIC frame, 2 map failure, 3 copy failure.
    #[arg(long, value_parser = clap::value_parser!(u8).range(1..=3), conflicts_with_all = ["no_build", "keep_running"])]
    pub guest_gic_failure_probe: Option<u8>,
    #[arg(long, default_value = "qemu_virt_aarch64")]
    pub board: String,
    #[arg(long, default_value = "buildroot")]
    pub guest_os: String,
    /// Host TCP port forwarded to guest SSH; 0 disables SSH forwarding.
    #[arg(long, env = "AGENTOS_TEST_SSH_PORT", default_value_t = 0)]
    pub ssh_port: u16,
    /// Authenticate a preseeded ARM guest using a host key reported through CC-PD.
    #[arg(long, conflicts_with_all = ["assert_live", "assert_desktop", "x86_ssh_key"])]
    pub seeded_ssh_key: Option<std::path::PathBuf>,
    /// Create fresh NoCloud media and a retained SSH identity from host.seed.
    #[arg(long, conflicts_with_all = ["seeded_ssh_key", "seeded_directory", "seeded_ssh_known_hosts", "assert_live", "assert_desktop", "x86_ssh_key", "assert_persistent_boots", "no_build"])]
    pub seed_profile: bool,
    /// Seed once, then authenticate two cold boots with the same disk and host key.
    #[arg(long, requires = "seed_profile")]
    pub assert_seeded_cold_boots: bool,
    #[arg(skip)]
    pub seeded_source: Option<std::path::PathBuf>,
    /// Pin the original host identity on a subsequent boot of the seeded disk.
    #[arg(long, requires_all = ["seeded_ssh_key", "seeded_directory"])]
    pub seeded_ssh_known_hosts: Option<std::path::PathBuf>,
    /// Retain a managed writable disk copy and reuse it for the cold boot.
    #[arg(long, requires = "seeded_ssh_key")]
    pub seeded_directory: Option<std::path::PathBuf>,
    #[arg(long, default_value_t = 120)]
    pub timeout_secs: u64,
    #[arg(long)]
    pub no_build: bool,
    /// After successful live-guest qualification, print client connection details and
    /// keep QEMU running until Enter is pressed.
    #[arg(long)]
    pub keep_running: bool,
    /// Require emulated virtio-net probe + DRIVER_OK + a pumped frame.
    /// Host tests are not this proof. GUEST_OS=none is a stub VMM.
    #[arg(long)]
    pub assert_emulated_net: bool,
    /// Require emulated virtio-blk probe + DRIVER_OK + a pumped request.
    #[arg(long)]
    pub assert_emulated_blk: bool,
    /// Recycle RAM and restore embedded images twice before the selected guest I/O proof.
    #[arg(long)]
    pub assert_guest_ram_recycle: bool,
    /// After destruction, retype private queue pools and verify zero pages and deleted caps.
    #[arg(long, requires = "assert_guest_teardown")]
    pub assert_guest_queue_recycle: bool,
    /// Stop block admission with a pending response and require complete drain.
    #[arg(long, requires = "assert_emulated_blk")]
    pub assert_guest_block_drain: bool,
    /// Require Ubuntu login and bidirectional I/O through emulated virtio-console.
    #[arg(long)]
    pub assert_emulated_console: bool,
    /// Create, boot and destroy the console guest explicitly through vm_manager.
    #[arg(long, requires = "assert_emulated_console")]
    pub assert_managed_guest: bool,
    /// Stall console consumption, then verify the deterministic probe stream.
    #[arg(long, requires = "assert_emulated_console")]
    pub assert_console_backpressure: bool,
    /// Destroy a qualified console-proof or seeded guest and require revocation.
    #[arg(long, conflicts_with_all = ["keep_running", "assert_live", "assert_desktop", "no_build", "assert_seeded_cold_boots"])]
    pub assert_guest_teardown: bool,
    /// Require Ubuntu login plus real I/O through agentOS net, blk, and console.
    #[arg(long)]
    pub assert_agentos_virtio: bool,
    /// Require a live-media profile to reach userspace and its profile proof.
    #[arg(long, visible_alias = "assert-ubuntu-live")]
    pub assert_live: bool,
    /// Require the dedicated x86 VMX/EPT one-instruction HLT-exit proof.
    #[arg(long)]
    pub assert_vmx_exit: bool,
    /// Require guest RDMSR/WRMSR faults, handler assertions and IRET recovery.
    #[arg(long, requires = "assert_vmx_exit", conflicts_with_all = ["assert_firmware_modes", "assert_firmware_reset"])]
    pub assert_guest_faults: bool,
    /// Also require real-address and unpaged protected VM-entry qualification.
    #[arg(long, requires = "assert_vmx_exit")]
    pub assert_firmware_modes: bool,
    /// Execute a hash-checked OVMF image from the architectural reset vector.
    #[arg(
        long,
        requires = "assert_vmx_exit",
        conflicts_with = "assert_firmware_modes"
    )]
    pub assert_firmware_reset: bool,
    /// Require the Linux initramfs syscall proof from guest ring 3.
    #[arg(
        long,
        requires = "assert_firmware_reset",
        conflicts_with = "assert_guest_faults"
    )]
    pub assert_x86_userspace: bool,
    /// Require a Linux login prompt over the canonical Intel virtio console.
    #[arg(long, requires = "assert_firmware_reset", requires = "x86_block_image",
          conflicts_with_all = ["assert_x86_userspace", "assert_guest_faults"])]
    pub assert_x86_linux_login: bool,
    /// Create Linux through binary CC and qualify its console and destruction.
    #[arg(long, requires = "assert_x86_linux_login")]
    pub assert_x86_cc: bool,
    /// Acquire and verify an x86 UEFI boot profile instead of separate artifact arguments.
    #[arg(long, requires = "assert_x86_linux_login")]
    pub x86_boot_profile: Option<std::path::PathBuf>,
    /// Prove Debian key-only SSH after login, pinning the host key from its console.
    #[arg(long, requires = "assert_x86_linux_login")]
    pub x86_ssh_key: Option<std::path::PathBuf>,
    /// Reuse a first-boot gate's known_hosts receipt for a cold-boot identity check.
    #[arg(long, requires = "x86_ssh_key")]
    pub x86_ssh_known_hosts: Option<std::path::PathBuf>,
    /// Reuse a root or qualification disk; writable only with --x86-block-write.
    #[arg(long, requires = "assert_firmware_reset")]
    pub x86_block_image: Option<std::path::PathBuf>,
    #[arg(long, requires = "x86_block_image")]
    pub x86_block_write: bool,
    /// Start the profile-defined desktop and verify one raw RFB frame
    /// through a key-authenticated SSH tunnel.
    #[arg(long)]
    pub assert_desktop: bool,
}

#[derive(clap::Args)]
pub struct QemuLaunchArgs {
    #[arg(long, default_value = "qemu_virt_aarch64")]
    pub board: String,
    /// Guest-profile-root-relative TOML to launch.
    #[arg(long, conflicts_with = "scenario")]
    pub profile: Option<std::path::PathBuf>,
    /// Data-defined guest scenario alias to launch.
    #[arg(long, conflicts_with = "profile")]
    pub scenario: Option<String>,
    /// Use the faster multi-threaded TCG development configuration.
    #[arg(long)]
    pub fast: bool,
}

#[derive(clap::Args)]
pub struct FaultInjectArgs {
    #[arg(long, default_value = "qemu_virt_aarch64")]
    pub board: String,
    #[arg(long, default_value_t = 60)]
    pub timeout_secs: u64,
}

#[derive(clap::Args)]
pub struct GenAbiArgs {
    /// TOML ABI spec (source of truth) consumed by gen-abi.
    #[arg(long, default_value = "tools/abi_spec.toml")]
    pub spec: std::path::PathBuf,
    /// Output header path.
    #[arg(long, default_value = "kernel/agentos-root-task/include/agentos_abi.h")]
    pub out: std::path::PathBuf,
    /// Validate the spec and exit without writing the header.
    #[arg(long)]
    pub check: bool,
}

#[derive(clap::Args)]
pub struct GenCapsArgs {
    /// Base system descriptor TOML, usually kernel/agentos-root-task/agentos.toml
    #[arg(long, default_value = "kernel/agentos-root-task/agentos.toml")]
    pub system: std::path::PathBuf,
    /// Board override TOML. The first existing non-empty path replaces --system.
    #[arg(long = "board-system")]
    pub board_system: Vec<std::path::PathBuf>,
    /// Output header path.
    #[arg(long)]
    pub out: std::path::PathBuf,
}

#[derive(clap::Args)]
pub struct GenChannelsArgs {
    #[arg(long, default_value = "kernel/agentos-root-task/agentos.system")]
    pub system: std::path::PathBuf,
    #[arg(
        long,
        default_value = "kernel/agentos-root-task/include/channels_generated.h"
    )]
    pub output: std::path::PathBuf,
}

#[derive(clap::Args)]
pub struct GenPolicyArgs {
    pub input: std::path::PathBuf,
    #[arg(long)]
    pub output: std::path::PathBuf,
}

#[derive(clap::Args)]
pub struct ExtractFreebsdFileArgs {
    pub image: std::path::PathBuf,
    pub guest_path: String,
    pub output: std::path::PathBuf,
}

#[derive(clap::Args)]
pub struct PolicyCheckArgs {}

#[derive(clap::Args)]
pub struct RunTestsArgs {
    #[arg(long, default_value = "qemu_virt_aarch64")]
    pub board: String,
    #[arg(long, default_value_t = 120)]
    pub timeout_secs: u64,
    #[arg(long)]
    pub no_build: bool,
    /// Parse an existing serial log instead of launching QEMU.
    #[arg(long)]
    pub input_log: Option<std::path::PathBuf>,
}

#[derive(clap::Args)]
pub struct SetupArgs {
    /// Check only the shared Microkit SDK; with --install, download it via make sdk.
    #[arg(long)]
    pub sdk_only: bool,
    /// Install platform prerequisites and the SDK via make setup.
    #[arg(long)]
    pub install: bool,
}

#[derive(clap::Args)]
pub struct RenderDeckArgs {
    /// Editable Markdown slide source.
    #[arg(
        long,
        default_value = "docs/presentations/agentos-systems-security/deck.md"
    )]
    pub input: std::path::PathBuf,
    /// Factual claim ledger bound into the QA receipt.
    #[arg(
        long,
        default_value = "docs/presentations/agentos-systems-security/FACTS.md"
    )]
    pub facts: std::path::PathBuf,
    /// Generated PDF path. The QA receipt is written beside it.
    #[arg(long)]
    pub output: std::path::PathBuf,
    /// Release or presentation edition recorded in the PDF footer and receipt.
    #[arg(long)]
    pub edition: String,
    /// Record that the rendered contact sheet and representative full-size pages were reviewed.
    #[arg(long)]
    pub visual_review: bool,
}

#[derive(clap::Args)]
pub struct FetchGuestArgs {
    /// Guest profile path, relative to --profile-root.
    #[arg(long)]
    pub profile: std::path::PathBuf,
    /// Directory containing guest profile TOML files.
    #[arg(long, default_value = "guest-profiles")]
    pub profile_root: std::path::PathBuf,
    /// Destination directory; defaults to host.build.acquire_dir from the profile.
    #[arg(long)]
    pub output_dir: Option<String>,
}

#[derive(clap::Args)]
pub struct ReleaseArgs {
    #[command(subcommand)]
    pub action: ReleaseAction,
}

#[derive(clap::Subcommand)]
pub enum ReleaseAction {
    /// Print a read-only release plan.
    Plan(ReleasePlanArgs),
    /// Update declared versions and CHANGELOG on a release branch.
    Prepare(ReleasePrepareArgs),
    /// Run exact-revision gates and write an ignored receipt.
    Check(ReleaseCheckArgs),
    /// Publish an already checked main revision.
    Publish(ReleasePublishArgs),
    /// Verify the remote tag and GitHub release without mutation.
    Verify(ReleaseVerifyArgs),
}

#[derive(clap::Args)]
pub struct ReleasePlanArgs {
    #[arg(long, value_enum, default_value_t = BumpKind::Patch)]
    pub bump: BumpKind,
    #[arg(long, value_enum, default_value_t = ReleaseClaim::Os)]
    pub claim: ReleaseClaim,
    /// Repository-relative artifact paths expected from the release gates.
    #[arg(long = "artifact")]
    pub artifacts: Vec<std::path::PathBuf>,
}

#[derive(clap::Args)]
pub struct ReleasePrepareArgs {
    #[arg(long)]
    pub version: String,
    /// Reviewed release date in YYYY-MM-DD form.
    #[arg(long)]
    pub date: String,
}

#[derive(clap::Args)]
pub struct ReleaseCheckArgs {
    #[arg(long)]
    pub version: String,
    #[arg(long, value_enum, default_value_t = ReleaseClaim::Os)]
    pub claim: ReleaseClaim,
    #[arg(long = "artifact")]
    pub artifacts: Vec<std::path::PathBuf>,
}

#[derive(clap::Args)]
pub struct ReleasePublishArgs {
    #[arg(long)]
    pub version: String,
    /// Must exactly equal `publish-vMAJOR.MINOR.PATCH`.
    #[arg(long)]
    pub authorize: String,
}

#[derive(clap::Args)]
pub struct ReleaseVerifyArgs {
    #[arg(long)]
    pub version: String,
}

#[derive(clap::ValueEnum, Clone, Copy, Debug, serde::Serialize, serde::Deserialize, PartialEq)]
#[serde(rename_all = "kebab-case")]
pub enum ReleaseClaim {
    Tooling,
    Os,
    Guests,
    Desktop,
}

#[derive(clap::ValueEnum, Clone, Copy, Debug)]
pub enum BumpKind {
    Patch,
    Minor,
    Major,
}

#[derive(clap::Args)]
pub struct CiMatrixArgs {
    #[arg(long)]
    pub list_only: bool,
    #[arg(long)]
    pub filter: Option<String>,
    /// Only run test cases for this board (e.g. qemu_virt_aarch64)
    #[arg(long, default_value = "")]
    pub board: String,
    /// Skip the build step and use whatever image is already present
    #[arg(long)]
    pub no_build: bool,
}

#[derive(clap::Args)]
pub struct TestApiArgs {
    /// Print every TAP line even for passing suites
    #[arg(long, short = 'v')]
    pub verbose: bool,
    /// C compiler to use (overrides CC env var)
    #[arg(long, env = "CC")]
    pub cc: Option<String>,
}

/// Arguments for the `test` subcommand (host-side TAP test runner).
#[derive(clap::Args)]
pub struct HostTestArgs {
    /// Run only the named suite (e.g. test_vibeos, test_msgbus)
    #[arg(long)]
    pub suite: Option<String>,
    /// C compiler to use (overrides the CC environment variable; default: cc)
    #[arg(long)]
    pub compiler: Option<String>,
    /// Print full TAP output for every suite, not just failures
    #[arg(long, short = 'v')]
    pub verbose: bool,
    /// Also launch QEMU and run the hardware test suite (requires a built image)
    #[arg(long)]
    pub hardware: bool,
}
