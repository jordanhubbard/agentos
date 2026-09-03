// Public library surface — shared types and command implementations.
// The binary entry point (src/main.rs) re-uses everything from here.

pub mod cmd_ci_matrix;
pub mod cmd_fault_inject;
pub mod cmd_fetch_guest;
pub mod cmd_gen_abi;
pub mod cmd_gen_caps;
pub mod cmd_gen_image;
pub mod cmd_gen_pd_bundle;
pub mod cmd_host_test;
pub mod cmd_release;
pub mod cmd_run_tests;
pub mod cmd_setup;
pub mod cmd_test;
pub mod cmd_test_api;

// ── Re-exports for main.rs ────────────────────────────────────────────────
pub use cmd_gen_image::GenImageArgs;
pub use cmd_gen_pd_bundle::GenPdBundleArgs;

// ── Subcommand arg structs ──────────────────────────────────────────────────

#[derive(clap::Args)]
pub struct TestArgs {
    #[arg(long, default_value = "qemu_virt_aarch64")]
    pub board: String,
    #[arg(long, default_value = "buildroot")]
    pub guest_os: String,
    /// Host TCP port forwarded to guest SSH; 0 disables SSH forwarding.
    #[arg(long, env = "AGENTOS_TEST_SSH_PORT", default_value_t = 0)]
    pub ssh_port: u16,
    #[arg(long, default_value_t = 120)]
    pub timeout_secs: u64,
    #[arg(long)]
    pub no_build: bool,
    /// Require emulated virtio-net probe + DRIVER_OK + a pumped frame.
    /// Host tests are not this proof. GUEST_OS=none is a stub VMM.
    #[arg(long)]
    pub assert_emulated_net: bool,
    /// Require emulated virtio-blk probe + DRIVER_OK + a pumped request.
    #[arg(long)]
    pub assert_emulated_blk: bool,
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
    #[arg(long)]
    pub sdk_only: bool,
    /// Install missing tools automatically (macOS: brew, Linux: apt-get)
    #[arg(long)]
    pub install: bool,
}

#[derive(clap::Args)]
pub struct FetchGuestArgs {
    #[arg(long, value_enum, default_value_t = GuestOs::Ubuntu)]
    pub os: GuestOs,
    /// Destination directory; defaults to build/guest-images
    #[arg(long)]
    pub output_dir: Option<String>,
}

#[derive(clap::ValueEnum, Clone)]
pub enum GuestOs {
    Ubuntu,
    Freebsd,
}

#[derive(clap::Args)]
pub struct ReleaseArgs {
    #[arg(long, value_enum, default_value_t = BumpKind::Patch)]
    pub bump: BumpKind,
    #[arg(long)]
    pub dry_run: bool,
}

#[derive(clap::ValueEnum, Clone, Debug)]
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
