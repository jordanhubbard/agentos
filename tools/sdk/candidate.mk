# Opt-in, isolated build for the approved CR2 dependency qualification.
# The source arguments name local upstream clones; only pinned commits are used.
SDK_CANDIDATE_DIR ?= $(HOME)/.cache/agentos/sdk-cr2-build
SDK_CANDIDATE_MICROKIT_SOURCE ?=
SDK_CANDIDATE_SEL4_SOURCE ?=
SDK_CANDIDATE_PYTHON ?= python3
SDK_CANDIDATE_REPO := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
SDK_CANDIDATE_VERSION := 2.3.1-agentos-e60776ac-cr2
SDK_CANDIDATE_ARCHIVE_SHA256 := 6a7db9fbb4b0480bad0d66ef2408d7ade71000ce894fac1323bb8397387d1eee
SDK_CANDIDATE_PACKAGE_DIR ?= $(SDK_CANDIDATE_REPO)/build/sdk-candidate-package

.PHONY: sdk-candidate sdk-candidate-check

# GNU tar normalizes archive metadata; gzip -n omits timestamps and filenames.
# Keep sources and the exact patch alongside the target-only SDK archive.
.PHONY: sdk-candidate-package
sdk-candidate-package: sdk-candidate-check
	@git -C "$(SDK_CANDIDATE_MICROKIT_SOURCE)" cat-file -e ec86afdcd662b5976d11d4994acf1b11a2979882^{commit}
	@git -C "$(SDK_CANDIDATE_SEL4_SOURCE)" cat-file -e e60776acc31097ca063806c257f07a3ec05eacf8^{commit}
	@mkdir -p "$$(dirname "$(SDK_CANDIDATE_PACKAGE_DIR)")"
	@mkdir "$(SDK_CANDIDATE_PACKAGE_DIR)" || \
		{ echo 'Package output must be fresh; existing results are preserved'; exit 1; }
	@mkdir "$(SDK_CANDIDATE_PACKAGE_DIR)/stage"
	cp -a "$(SEL4_SDK)" "$(SDK_CANDIDATE_PACKAGE_DIR)/stage/microkit-sdk-$(SDK_CANDIDATE_VERSION)"
	tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner --format=gnu \
		-cf "$(SDK_CANDIDATE_PACKAGE_DIR)/agentos-sdk-targets.tar" \
		-C "$(SDK_CANDIDATE_PACKAGE_DIR)/stage" "microkit-sdk-$(SDK_CANDIDATE_VERSION)"
	gzip -n "$(SDK_CANDIDATE_PACKAGE_DIR)/agentos-sdk-targets.tar"
	git -C "$(SDK_CANDIDATE_MICROKIT_SOURCE)" archive --format=tar --prefix=microkit/ \
		ec86afdcd662b5976d11d4994acf1b11a2979882 > "$(SDK_CANDIDATE_PACKAGE_DIR)/microkit-source.tar"
	gzip -n "$(SDK_CANDIDATE_PACKAGE_DIR)/microkit-source.tar"
	git -C "$(SDK_CANDIDATE_SEL4_SOURCE)" archive --format=tar --prefix=sel4/ \
		e60776acc31097ca063806c257f07a3ec05eacf8 > "$(SDK_CANDIDATE_PACKAGE_DIR)/sel4-source.tar"
	gzip -n "$(SDK_CANDIDATE_PACKAGE_DIR)/sel4-source.tar"
	cp "$(SDK_CANDIDATE_REPO)/tools/sdk/patches/sel4-e60776ac-cr2.patch" \
		"$(SDK_CANDIDATE_REPO)/tools/sdk/cr2-kernels.sha256" \
		"$(SDK_CANDIDATE_REPO)/tools/sdk/candidate.mk" \
		"$(SDK_CANDIDATE_REPO)/docs/x86-cr2-candidate.md" "$(SDK_CANDIDATE_PACKAGE_DIR)/"
	cd "$(SDK_CANDIDATE_PACKAGE_DIR)" && sha256sum *.tar.gz *.patch *.sha256 *.mk *.md > SHA256SUMS
	@echo 'Candidate artifacts packaged locally; publication and default adoption remain separate.'

# Check the selected installed candidate before accepting it as a build input.
# A directory name alone does not establish which kernel it contains.
sdk-candidate-check:
	@test "$$(cat "$(SEL4_SDK)/VERSION")" = "$(SDK_CANDIDATE_VERSION)" || \
		{ echo 'ERROR: candidate SDK VERSION does not match the qualified pin'; exit 1; }
	@cd "$(SEL4_SDK)" && sha256sum -c "$(SDK_CANDIDATE_REPO)/tools/sdk/cr2-kernels.sha256"
	@for board in qemu_virt_aarch64 x86_64_generic x86_64_generic_vtx; do \
		for header in sel4/sel4.h kernel/gen_config.h; do \
			test -s "$(SEL4_SDK)/board/$$board/release/include/$$header" || \
				{ echo "ERROR: candidate SDK missing $$board/$$header"; exit 1; }; \
		done; \
	done
	@echo 'Candidate version, kernel hashes and required header presence verified.'

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
	$(MAKE) sdk-candidate-check SEL4_SDK="$(SDK_CANDIDATE_DIR)/microkit/release/microkit-sdk-$(SDK_CANDIDATE_VERSION)"
	@echo 'Candidate built; runtime acceptance and default SDK adoption remain separate.'
	@echo 'SEL4_SDK=$(SDK_CANDIDATE_DIR)/microkit/release/microkit-sdk-$(SDK_CANDIDATE_VERSION)'
