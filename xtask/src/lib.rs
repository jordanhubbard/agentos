// Public library surface — shared types and command implementations.
// The binary entry point (src/main.rs) re-uses everything from here.

pub mod cmd_ci_matrix;
pub mod cmd_fault_inject;
pub mod cmd_fetch_guest;
pub mod cmd_release;
pub mod cmd_setup;
pub mod cmd_test;

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
}

#[derive(clap::Args)]
pub struct FetchGuestArgs {
    #[arg(long, value_enum, default_value_t = GuestOs::Ubuntu)]
    pub os: GuestOs,
    #[arg(long, default_value = "guest-images")]
    pub output_dir: String,
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
}
