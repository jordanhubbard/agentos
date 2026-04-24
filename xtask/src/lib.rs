// Public library surface — shared types and command implementations.
// The binary entry point (src/main.rs) re-uses everything from here.

pub mod cmd_ci_matrix;
pub mod cmd_fault_inject;
pub mod cmd_fetch_guest;
pub mod cmd_host_test;
pub mod cmd_release;
pub mod cmd_setup;
pub mod cmd_test;
pub mod cmd_test_api;

// ── Subcommand arg structs ──────────────────────────────────────────────────

#[derive(clap::Args)]
pub struct TestArgs {
    #[arg(long, default_value = "qemu_virt_aarch64")]
    pub board: String,
    #[arg(long, default_value = "buildroot")]
    pub guest_os: String,
    #[arg(long, default_value_t = 120)]
    pub timeout_secs: u64,
    #[arg(long)]
    pub no_build: bool,
}

#[derive(clap::Args)]
pub struct FaultInjectArgs {
    #[arg(long, default_value = "qemu_virt_aarch64")]
    pub board: String,
    #[arg(long, default_value_t = 60)]
    pub timeout_secs: u64,
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
    /// Destination directory; defaults to ~/.local/agentos-images
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
