## Summary

Describe the user-visible behavior or API contract changed by this PR.

## Validation

- [ ] `make test TARGET_ARCH=aarch64` passes, or the reason it was not run is documented
- [ ] `make test TARGET_ARCH=x86_64` passes, or the reason it was not run is documented
- [ ] New or changed IPC opcodes have contract tests
- [ ] New PD code has a contract header under `kernel/agentos-root-task/include/contracts/`

## Repository Boundary

- [ ] No UI code was added to this repository
- [ ] No `.html`, `.css`, `.js`, `.mjs`, `.jsx`, `.tsx`, `.vue`, or `.svelte` files were added
- [ ] No `package.json`, `node_modules`, `yarn.lock`, or `bun.lockb` files were added
- [ ] First-party target code, host tools, tests, generators, and skill helpers use only C, Rust, or Assembly
- [ ] No forbidden interpreted-language code was added anywhere in the repository

## Device Waiver

- [ ] This PR does not add a guest-specific device driver for a class already served by a generic device PD
- [ ] If a guest-specific driver is required, an approved `[device-waiver] <guest_os> requires <device_class>` issue is linked here:
