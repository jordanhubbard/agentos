use clap::{Parser, Subcommand};
use xtask::{
    cmd_ci_matrix, cmd_extract_freebsd_file, cmd_fault_inject, cmd_fetch_guest, cmd_gen_abi,
    cmd_gen_caps, cmd_gen_channels, cmd_gen_image, cmd_gen_pd_bundle, cmd_gen_policy,
    cmd_host_test, cmd_policy_check, cmd_release, cmd_run_tests, cmd_setup, cmd_test, cmd_test_api,
    CiMatrixArgs, ExtractFreebsdFileArgs, FaultInjectArgs, FetchGuestArgs, GenAbiArgs, GenCapsArgs,
    GenChannelsArgs, GenImageArgs, GenPdBundleArgs, GenPolicyArgs, HostTestArgs, PolicyCheckArgs,
    ReleaseArgs, RunTestsArgs, SetupArgs, TestApiArgs, TestArgs,
};

#[derive(Parser)]
#[command(name = "xtask", about = "agentOS workspace task runner")]
struct Cli {
    #[command(subcommand)]
    command: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// Compile and run host-side TAP test suites (API + integration)
    Test(HostTestArgs),
    /// Build the seL4 image and run a QEMU boot test
    #[command(name = "qemu-test")]
    QemuTest(TestArgs),
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
    /// Compile and run the API test suite only (TAP output)
    TestApi(TestApiArgs),
    /// Build and run the seL4-target TAP test image in QEMU
    #[command(name = "run-tests")]
    RunTests(RunTestsArgs),
    /// Generate (or validate) per-PD ABI tables; fails on opcode/channel collisions
    #[command(name = "gen-abi")]
    GenAbi(GenAbiArgs),
    /// Generate deterministic root-task capability slot layout constants
    #[command(name = "gen-caps")]
    GenCaps(GenCapsArgs),
    /// Generate typed channel IDs from the Microkit system XML
    #[command(name = "gen-channels")]
    GenChannels(GenChannelsArgs),
    /// Compile a text capability policy into its packed binary form
    #[command(name = "gen-policy")]
    GenPolicy(GenPolicyArgs),
    /// Extract one regular file from a FreeBSD UFS2 disk image
    #[command(name = "extract-freebsd-file")]
    ExtractFreebsdFile(ExtractFreebsdFileArgs),
    /// Reject repository-owned files forbidden by the project constitution
    #[command(name = "policy-check")]
    PolicyCheck(PolicyCheckArgs),
    /// Pack ELFs + cap init data into a bootable agentos.img (replaces microkit binary)
    GenImage(GenImageArgs),
    /// Pack PD ELFs into a .pd_bundle blob for embedding into root_task.elf
    #[command(name = "gen-pd-bundle")]
    GenPdBundle(GenPdBundleArgs),
}

fn main() -> anyhow::Result<()> {
    let cli = Cli::parse();
    match cli.command {
        Cmd::Test(a) => cmd_host_test::run(&a),
        Cmd::QemuTest(a) => cmd_test::run(&a),
        Cmd::FaultInject(a) => cmd_fault_inject::run(&a),
        Cmd::Setup(a) => cmd_setup::run(&a),
        Cmd::FetchGuest(a) => cmd_fetch_guest::run(&a),
        Cmd::Release(a) => cmd_release::run(&a),
        Cmd::CiMatrix(a) => cmd_ci_matrix::run(&a),
        Cmd::TestApi(a) => cmd_test_api::run(&a),
        Cmd::RunTests(a) => cmd_run_tests::run(&a),
        Cmd::GenAbi(a) => cmd_gen_abi::run(&a),
        Cmd::GenCaps(a) => cmd_gen_caps::run(&a),
        Cmd::GenChannels(a) => cmd_gen_channels::run(&a),
        Cmd::GenPolicy(a) => cmd_gen_policy::run(&a),
        Cmd::ExtractFreebsdFile(a) => cmd_extract_freebsd_file::run(&a),
        Cmd::PolicyCheck(a) => cmd_policy_check::run(&a),
        Cmd::GenImage(a) => cmd_gen_image::run(&a),
        Cmd::GenPdBundle(a) => cmd_gen_pd_bundle::run(&a),
    }
}
