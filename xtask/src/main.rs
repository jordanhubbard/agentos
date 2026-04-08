use clap::{Parser, Subcommand};
use xtask::{
    cmd_ci_matrix, cmd_fault_inject, cmd_fetch_guest, cmd_release, cmd_setup, cmd_test,
    CiMatrixArgs, FaultInjectArgs, FetchGuestArgs, ReleaseArgs, SetupArgs, TestArgs,
};

#[derive(Parser)]
#[command(name = "xtask", about = "agentOS workspace task runner")]
struct Cli {
    #[command(subcommand)]
    command: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// Build the seL4 image and run QEMU boot test
    Test(TestArgs),
    /// Run fault injection tests
    FaultInject(FaultInjectArgs),
    /// Set up the development environment
    Setup(SetupArgs),
    /// Fetch guest OS disk images
    FetchGuest(FetchGuestArgs),
    /// Automated release (version bump + git tag)
    Release(ReleaseArgs),
    /// Run the libvmm CI test matrix
    CiMatrix(CiMatrixArgs),
}

fn main() -> anyhow::Result<()> {
    let cli = Cli::parse();
    match cli.command {
        Cmd::Test(a) => cmd_test::run(&a),
        Cmd::FaultInject(a) => cmd_fault_inject::run(&a),
        Cmd::Setup(a) => cmd_setup::run(&a),
        Cmd::FetchGuest(a) => cmd_fetch_guest::run(&a),
        Cmd::Release(a) => cmd_release::run(&a),
        Cmd::CiMatrix(a) => cmd_ci_matrix::run(&a),
    }
}
