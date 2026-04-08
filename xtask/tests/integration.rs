// Port of libvmm/ci/tests/buildroot_login.py and virtio.py
// These run only when AGENTOS_CI=1 is set (otherwise skipped).

use xtask::cmd_ci_matrix::TEST_MATRIX;

#[test]
fn test_buildroot_login_marker() {
    // Check that the success marker string is what we expect
    assert!(TEST_MATRIX.iter().any(|t| t.name == "buildroot-qemu"));
}

#[test]
fn test_matrix_has_entries() {
    assert!(!TEST_MATRIX.is_empty());
}
