# Opt-in, isolated build for the approved CR2 dependency qualification.
# The source arguments name local upstream clones; only pinned commits are used.
SDK_CANDIDATE_DIR ?= $(HOME)/.cache/agentos/sdk-cr2-build
SDK_CANDIDATE_MICROKIT_SOURCE ?=
SDK_CANDIDATE_SEL4_SOURCE ?=
SDK_CANDIDATE_PYTHON ?= python3
SDK_CANDIDATE_REPO := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
SDK_CANDIDATE_VERSION := 2.3.1-agentos-e60776ac-cr2

.PHONY: sdk-candidate
sdk-candidate:
	@test -d "$(SDK_CANDIDATE_MICROKIT_SOURCE)" -a -d "$(SDK_CANDIDATE_SEL4_SOURCE)" || \
		{ echo 'Set SDK_CANDIDATE_MICROKIT_SOURCE and SDK_CANDIDATE_SEL4_SOURCE to upstream clones'; exit 1; }
	@case "$$(realpath -m -- "$(SDK_CANDIDATE_DIR)")" in \
		"$(SDK_CANDIDATE_REPO)"|"$(SDK_CANDIDATE_REPO)"/*) \
			echo 'SDK_CANDIDATE_DIR must be outside the agentOS checkout'; exit 1 ;; \
	esac
	@test ! -e "$(SDK_CANDIDATE_DIR)" || \
		{ echo 'SDK_CANDIDATE_DIR must be a fresh directory; previous results are preserved'; exit 1; }
	@mkdir -p "$$(dirname "$(SDK_CANDIDATE_DIR)")"
	@mkdir "$(SDK_CANDIDATE_DIR)"
	git clone --no-hardlinks --no-checkout -- "$(SDK_CANDIDATE_MICROKIT_SOURCE)" "$(SDK_CANDIDATE_DIR)/microkit"
	git -C "$(SDK_CANDIDATE_DIR)/microkit" checkout --detach ec86afdcd662b5976d11d4994acf1b11a2979882
	git clone --no-hardlinks --no-checkout -- "$(SDK_CANDIDATE_SEL4_SOURCE)" "$(SDK_CANDIDATE_DIR)/sel4"
	git -C "$(SDK_CANDIDATE_DIR)/sel4" checkout --detach e60776acc31097ca063806c257f07a3ec05eacf8
	git -C "$(SDK_CANDIDATE_DIR)/sel4" apply "$(SDK_CANDIDATE_REPO)/tools/sdk/patches/sel4-e60776ac-cr2.patch"
	@for board in qemu_virt_aarch64 x86_64_generic x86_64_generic_vtx; do \
		cache="$(SDK_CANDIDATE_DIR)/microkit/build/$$board/release/sel4/build"; \
		mkdir -p "$$cache" || exit 1; \
		printf '%s\n' 'KernelVerificationBuild:BOOL=OFF' 'KernelDebugBuild:BOOL=OFF' \
			'KernelPrinting:BOOL=OFF' 'KernelIRQReporting:BOOL=OFF' \
			'KernelColourPrinting:BOOL=OFF' > "$$cache/CMakeCache.txt" || exit 1; \
	done
	cd "$(SDK_CANDIDATE_DIR)/microkit" && "$(SDK_CANDIDATE_PYTHON)" build_sdk.py \
		--sel4 ../sel4 --boards qemu_virt_aarch64,x86_64_generic,x86_64_generic_vtx \
		--configs release --gcc-toolchain-prefix-aarch64 aarch64-linux-gnu \
		--skip-tool --skip-initialiser --skip-docs --skip-tar --version $(SDK_CANDIDATE_VERSION)
	cd "$(SDK_CANDIDATE_DIR)/microkit/release/microkit-sdk-$(SDK_CANDIDATE_VERSION)" && \
		sha256sum -c "$(SDK_CANDIDATE_REPO)/tools/sdk/cr2-kernels.sha256"
	@echo 'Candidate built; runtime acceptance and default SDK adoption remain separate.'
	@echo 'SEL4_SDK=$(SDK_CANDIDATE_DIR)/microkit/release/microkit-sdk-$(SDK_CANDIDATE_VERSION)'
