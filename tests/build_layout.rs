//! Exercise the real cleanup recipes in an isolated checkout, without Cargo/SDK.
use std::{fs, path::Path, process::Command};

fn put(root: &Path, name: &str) {
    let path = root.join(name);
    fs::create_dir_all(path.parent().unwrap()).unwrap();
    fs::write(path, b"retained bytes\n").unwrap();
}

fn make(root: &Path, goal: &str) {
    let result = Command::new("/usr/bin/make")
        .current_dir(root)
        .env("PATH", "/usr/bin:/bin")
        .env_remove("MAKEFLAGS")
        .env_remove("MFLAGS")
        .args([
            goal,
            "GUEST_PROFILE=does-not-exist.toml",
            "BOARD_NAME=does-not-exist",
            "BUILD_DIR=source",
            "CLEAN_BUILD_ROOT=source",
            "CLEAN_REPO_ROOT=source",
        ])
        .output()
        .unwrap();
    assert!(
        result.status.success(),
        "{goal}: {}\n{}",
        String::from_utf8_lossy(&result.stdout),
        String::from_utf8_lossy(&result.stderr)
    );
}

#[test]
fn cleanup_removes_all_outputs_and_preserves_inputs_without_build_dependencies() {
    let root = std::env::current_dir()
        .unwrap()
        .join(format!("_build/tmp/cleanup-fixture-{}", std::process::id()));
    fs::create_dir_all(root.join("mk")).unwrap();
    fs::write(root.join("Makefile"), include_str!("../Makefile")).unwrap();
    fs::write(root.join("mk/clean.mk"), include_str!("../mk/clean.mk")).unwrap();
    put(&root, "source/keep.c");
    for goal in ["clean", "clean-all"] {
        for artifact in [
            "_build/board-a/object.o",
            "_build/board-b/library.a",
            "_build/board-b/agentos.img",
            "_build/cargo/debug/xtask",
            "_build/cargo/.rustc_info.json",
            "_build/tools/agentctl/agentctl",
            "_build/guest-images/disk.raw",
            "_build/tmp/.hidden",
        ] {
            put(&root, artifact);
        }
        make(&root, goal);
        assert!(!root.join("_build").exists());
        assert_eq!(
            fs::read(root.join("source/keep.c")).unwrap(),
            b"retained bytes\n"
        );
        make(&root, goal); // Missing output tree is also a successful cleanup.
    }
    for artifact in [
        "build/board/object.o",
        "target/debug/xtask",
        "kernel/agentos-root-task/build/board/library.a",
        "kernel/loader/build/board/loader.elf",
        "libvmm.a",
        "libsddf_util_debug.a",
        "libvmm/arch/old.o",
        "tools/agentctl/agentctl",
        "tests/ipc_bench/ipc_bench_host",
    ] {
        put(&root, artifact);
    }
    put(&root, "_build/keep-until-clean");
    make(&root, "clean-legacy");
    assert!(!root.join("build").exists());
    assert!(!root.join("target").exists());
    assert!(!root.join("kernel/agentos-root-task/build").exists());
    assert!(!root.join("kernel/loader/build").exists());
    assert!(!root.join("libvmm.a").exists());
    assert!(!root.join("libsddf_util_debug.a").exists());
    assert!(!root.join("libvmm/arch").exists());
    assert!(!root.join("tools/agentctl/agentctl").exists());
    assert!(!root.join("tests/ipc_bench/ipc_bench_host").exists());
    assert!(root.join("_build/keep-until-clean").exists());
    assert!(root.join("source/keep.c").exists());
    fs::remove_dir_all(root).unwrap();
}
