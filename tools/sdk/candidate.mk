# Opt-in, isolated build for the approved CR2 dependency qualification.
#
# The sources are VENDORED: each dependency is a git submodule under vendor/
# tracking an agentOS-controlled full-history mirror of the upstream
# repository, pinned by gitlink to the exact upstream commit. Upstream is not
# contacted at build time. See docs/sdk-provenance.md for why (an upstream was
# observed deleting a commit another project referenced) and for what the
# mechanism does and does not establish.
#
# tools/sdk/vendor.manifest is the single declaration of what is vendored;
# `make sdk-provenance` cross-checks it against the recorded gitlinks, the
# mirror contents and this file's pins. Overriding the *_SOURCE variables with
# a different clone is still supported for local experiments, but the pinned
# commits below are what is built and hashed.
SDK_CANDIDATE_DIR ?= $(SDK_CANDIDATE_REPO)/_build/sdk-candidate
SDK_CANDIDATE_MICROKIT_SOURCE ?= $(SDK_CANDIDATE_REPO)/vendor/microkit
SDK_CANDIDATE_SEL4_SOURCE ?= $(SDK_CANDIDATE_REPO)/vendor/sel4
SDK_CANDIDATE_PYTHON ?= python3
SDK_CANDIDATE_REPO := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
SDK_CANDIDATE_VERSION := 2.3.1-agentos-e60776ac-cr2
# Digest of the PUBLISHED release asset that `make sdk` downloads, NOT of
# whatever the pipeline currently builds. Makefile's `sdk` recipe checks a
# fresh download against it, so changing this value breaks `make sdk` for
# everyone until the matching asset is actually published.
#
# Adding a board to SDK_CANDIDATE_BOARDS necessarily changes what
# sdk-candidate-package produces, so this pin and the build diverge from the
# moment the board lands until the new archive is published and this value
# updated — in that order, in one change. Until then the sdk-candidate
# workflow reports the divergence and prints the digest to publish, rather
# than asserting an equality that is knowingly false or silently dropping the
# check.
SDK_CANDIDATE_ARCHIVE_SHA256 := fb4290f10c2e59a0baa4d85d477726c3713dec5c497e0d232968bcb6675d566b
SDK_CANDIDATE_PACKAGE_DIR ?= $(SDK_CANDIDATE_REPO)/_build/sdk-candidate-package

# The boards this SDK is qualified for, in ONE place.  This list was repeated
# four times (build, CMake cache seeding, package staging, acceptance check)
# and adding a board meant editing all four; missing one produced an SDK that
# built a kernel nobody hashed, or hashed a kernel nobody packaged.
#
# qemu_virt_riscv64 is here because riscv64 is a first-class target of this
# tree (kernel/agentos-root-task/src/system_desc_riscv64.c, make test-riscv64)
# and was previously unbuildable in CI for exactly one reason: no riscv64
# kernel was ever produced.  Its toolchain prefix is passed to build_sdk.py
# below; nothing forked is introduced, the pinned commits and the single
# tools/sdk/patches/ patch are unchanged.
SDK_CANDIDATE_COMMA := ,
SDK_CANDIDATE_EMPTY :=
SDK_CANDIDATE_SPACE := $(SDK_CANDIDATE_EMPTY) $(SDK_CANDIDATE_EMPTY)
SDK_CANDIDATE_BOARDS := qemu_virt_aarch64 x86_64_generic x86_64_generic_vtx qemu_virt_riscv64
SDK_CANDIDATE_BOARDS_CSV := \
	$(subst $(SDK_CANDIDATE_SPACE),$(SDK_CANDIDATE_COMMA),$(strip $(SDK_CANDIDATE_BOARDS)))

.PHONY: sdk-candidate sdk-candidate-check sdk-provenance

# One-line wrapper over the Rust implementation (xtask/src/cmd_sdk_provenance.rs).
# This is the artifact that replaces "go check the SHA against upstream
# yourself": it reports, per dependency, the upstream repo and commit, whether
# the pin is still reachable upstream (plainly saying so when it is not --
# an expected, non-fatal state once sources are vendored), and the diff stat of
# the agentOS delta.
sdk-provenance:
	@cargo xtask sdk-provenance

# Enforced form of the same checks, minus the network query, so the build
# refuses to proceed if the recorded gitlink, the manifest and the commits
# pinned in this file have drifted apart.
.PHONY: sdk-vendor-check
sdk-vendor-check:
	@test -e "$(SDK_CANDIDATE_SEL4_SOURCE)/.git" -a -e "$(SDK_CANDIDATE_MICROKIT_SOURCE)/.git" || \
		{ echo 'ERROR: vendored sources are not checked out; run: make submodules'; exit 1; }
	@cargo xtask sdk-provenance --offline

# GNU tar normalizes archive metadata; gzip -n omits timestamps and filenames.
# Keep sources and the exact patch alongside the target-only SDK archive.
.PHONY: sdk-candidate-package
sdk-candidate-package: sdk-vendor-check sdk-candidate-check
	@git -C "$(SDK_CANDIDATE_MICROKIT_SOURCE)" cat-file -e ec86afdcd662b5976d11d4994acf1b11a2979882^{commit}
	@git -C "$(SDK_CANDIDATE_SEL4_SOURCE)" cat-file -e e60776acc31097ca063806c257f07a3ec05eacf8^{commit}
	@mkdir -p "$$(dirname "$(SDK_CANDIDATE_PACKAGE_DIR)")"
	@mkdir "$(SDK_CANDIDATE_PACKAGE_DIR)" || \
		{ echo 'Package output must be fresh; existing results are preserved'; exit 1; }
	@mkdir "$(SDK_CANDIDATE_PACKAGE_DIR)/stage"
	cc -std=c11 -Wall -Wextra -Werror "$(SDK_CANDIDATE_REPO)/tools/sdk/normalize-header.c" \
		-o "$(SDK_CANDIDATE_PACKAGE_DIR)/normalize-header"
	@set -eu; stage="$(SDK_CANDIDATE_PACKAGE_DIR)/stage/microkit-sdk-$(SDK_CANDIDATE_VERSION)"; \
		mkdir "$$stage"; \
		cp -a "$(SEL4_SDK)/VERSION" "$(SEL4_SDK)/LICENSE.md" "$(SEL4_SDK)/LICENSES" "$$stage/"; \
		for board in $(SDK_CANDIDATE_BOARDS); do \
			mkdir -p "$$stage/board/$$board/release/elf"; \
			mkdir -p "$$stage/board/$$board/release/lib"; \
			cp -a "$(SEL4_SDK)/board/$$board/release/lib/microkit.ld" "$$stage/board/$$board/release/lib/"; \
			cp -a "$(SEL4_SDK)/board/$$board/release/elf/sel4.elf" "$$stage/board/$$board/release/elf/"; \
			case "$$board" in x86_64_*) \
				cp -a "$(SEL4_SDK)/board/$$board/release/elf/sel4_32.elf" "$$stage/board/$$board/release/elf/" ;; esac; \
			cp -a "$(SEL4_SDK)/board/$$board/release/include" "$$stage/board/$$board/release/"; \
			for header in sel4/shared_types_gen.h sel4/sel4_arch/types_gen.h; do \
				file="$$stage/board/$$board/release/include/$$header"; \
				"$(SDK_CANDIDATE_PACKAGE_DIR)/normalize-header" "$$file" "$$file.tmp"; \
				mv "$$file.tmp" "$$file"; \
			done; \
		done
	tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner --format=gnu --mode='u=rwX,go=rX' \
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
		"$(SDK_CANDIDATE_REPO)/tools/sdk/vendor.manifest" \
		"$(SDK_CANDIDATE_REPO)/tools/sdk/python-requirements.txt" \
		"$(SDK_CANDIDATE_REPO)/tools/sdk/normalize-header.c" \
		"$(SDK_CANDIDATE_REPO)/docs/x86-cr2-candidate.md" "$(SDK_CANDIDATE_PACKAGE_DIR)/"
	cd "$(SDK_CANDIDATE_PACKAGE_DIR)" && sha256sum *.tar.gz *.patch *.sha256 *.mk *.manifest *.md *.txt *.c > SHA256SUMS
	@# Print the archive digest. It is the value SDK_CANDIDATE_ARCHIVE_SHA256
	@# must carry once this archive is published, and it was previously only
	@# written to a file inside a build directory -- so the one number a
	@# publisher needs never appeared in the log they were reading.
	@set -eu; d="$$(sha256sum "$(SDK_CANDIDATE_PACKAGE_DIR)/agentos-sdk-targets.tar.gz" | cut -d' ' -f1)"; \
	echo "Archive digest: $$d  (agentos-sdk-targets.tar.gz)"; \
	if [ "$$d" = "$(SDK_CANDIDATE_ARCHIVE_SHA256)" ]; then \
		echo 'Matches SDK_CANDIDATE_ARCHIVE_SHA256: this archive reproduces the published asset.'; \
	else \
		echo "::warning title=SDK archive differs from the published pin::This build" \
		     "produced $$d but SDK_CANDIDATE_ARCHIVE_SHA256 names" \
		     "$(SDK_CANDIDATE_ARCHIVE_SHA256), which is the asset make sdk downloads" \
		     "today. Expected while a board is being added. To close it: publish this" \
		     "agentos-sdk-targets.tar.gz as the release asset AND set" \
		     "SDK_CANDIDATE_ARCHIVE_SHA256 to $$d, in that order. Until both are done," \
		     "make sdk still installs the older board set."; \
		echo "SDK archive digest differs from the published pin (see the warning above)."; \
	fi
	@echo 'Candidate artifacts packaged locally; publication and default adoption remain separate.'

# Check the selected installed candidate before accepting it as a build input.
# A directory name alone does not establish which kernel it contains.
sdk-candidate-check:
	@test "$$(cat "$(SEL4_SDK)/VERSION")" = "$(SDK_CANDIDATE_VERSION)" || \
		{ echo 'ERROR: candidate SDK VERSION does not match the qualified pin'; exit 1; }
	@# --ignore-missing, paired with the coverage check below.  cr2-kernels.sha256
	@# is the manifest for the FULL board set; an SDK artifact published before a
	@# board was added simply does not contain that board's files, and without
	@# this flag sha256sum fails on them and blocks every build on every
	@# architecture.  The flag alone would be a hole -- it would also pass an SDK
	@# missing everything -- which is why the loop below separately requires each
	@# board that IS present to be listed here, and requires at least one.
	@# Together: listed and present must match; present and unlisted is refused;
	@# listed and absent is the only thing skipped.
	@cd "$(SEL4_SDK)" && sha256sum -c --ignore-missing \
		"$(SDK_CANDIDATE_REPO)/tools/sdk/cr2-kernels.sha256"
	@# `sha256sum -c` only checks the lines it is given.  A board built into
	@# the SDK but absent from cr2-kernels.sha256 would therefore sail through
	@# the line above with its kernel completely unhashed -- the exact silent
	@# gap this pipeline exists to close.  So: every board that is PRESENT in
	@# the SDK under test must have a recorded kernel hash, and when one does
	@# not, print the computed hashes so recording them is a paste, not a guess.
	@#
	@# The presence guard is load-bearing and is not a loophole.  This target
	@# is a prerequisite of every ordinary `make build` (Makefile's sdk-check),
	@# so it runs against the PUBLISHED SDK artifact as well as against a
	@# freshly built candidate.  A board newly added to SDK_CANDIDATE_BOARDS
	@# does not exist in an artifact published before it was added, and failing
	@# there would block all builds on all architectures for a kernel that is
	@# not in the tree being checked.  What must never pass is a kernel that IS
	@# there and is unhashed, and that is exactly what this rejects.  The
	@# sdk-candidate build produces the board, so the check fires for real.
	@set -eu; hashes="$(SDK_CANDIDATE_REPO)/tools/sdk/cr2-kernels.sha256"; \
	missing=0; present=0; \
	for board in $(SDK_CANDIDATE_BOARDS); do \
		rel="board/$$board/release/elf/sel4.elf"; \
		test -s "$(SEL4_SDK)/$$rel" || continue; \
		present=$$((present + 1)); \
		if ! grep -q "  $$rel\$$" "$$hashes"; then \
			missing=1; \
			echo "ERROR: $$rel is present in this SDK but has no recorded hash"; \
			echo "       in tools/sdk/cr2-kernels.sha256. It produced:"; \
			(cd "$(SEL4_SDK)" && sha256sum "$$rel") | sed 's/^/       /'; \
			(cd "$(SEL4_SDK)" && sha256sum "board/$$board/release/lib/microkit.ld") | sed 's/^/       /'; \
		fi; \
	done; \
	test "$$missing" = 0 || \
		{ echo 'Record the lines above, then re-run; do not drop the board instead.'; exit 1; }; \
	test "$$present" -ge 1 || \
		{ echo 'ERROR: this SDK contains no kernel for any board in SDK_CANDIDATE_BOARDS,'; \
		  echo '       so --ignore-missing above verified nothing at all.'; exit 1; }
	@for board in $(SDK_CANDIDATE_BOARDS); do \
		test -d "$(SEL4_SDK)/board/$$board" || continue; \
		for header in sel4/sel4.h kernel/gen_config.h; do \
			test -s "$(SEL4_SDK)/board/$$board/release/include/$$header" || \
				{ echo "ERROR: candidate SDK missing $$board/$$header"; exit 1; }; \
		done; \
	done
	@echo 'Candidate version, kernel hashes and required header presence verified.'

sdk-candidate: sdk-vendor-check
	@test -d "$(SDK_CANDIDATE_MICROKIT_SOURCE)" -a -d "$(SDK_CANDIDATE_SEL4_SOURCE)" || \
		{ echo 'Vendored sources missing. Run: make submodules'; \
		echo '(or set SDK_CANDIDATE_MICROKIT_SOURCE / SDK_CANDIDATE_SEL4_SOURCE to other clones)'; exit 1; }
	@case "$$(realpath -m -- "$(SDK_CANDIDATE_DIR)")" in \
		"$(SDK_CANDIDATE_REPO)/_build/"*) ;; \
		"$(SDK_CANDIDATE_REPO)"|"$(SDK_CANDIDATE_REPO)"/*) \
			echo 'SDK_CANDIDATE_DIR inside the checkout must be under _build'; exit 1 ;; \
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
	@for board in $(SDK_CANDIDATE_BOARDS); do \
		cache="$(SDK_CANDIDATE_DIR)/microkit/build/$$board/release/sel4/build"; \
		mkdir -p "$$cache" || exit 1; \
		printf '%s\n' 'KernelVerificationBuild:BOOL=OFF' 'KernelDebugBuild:BOOL=OFF' \
			'KernelPrinting:BOOL=OFF' 'KernelIRQReporting:BOOL=OFF' \
			'KernelColourPrinting:BOOL=OFF' > "$$cache/CMakeCache.txt" || exit 1; \
	done
	cd "$(SDK_CANDIDATE_DIR)/microkit" && "$(SDK_CANDIDATE_PYTHON)" build_sdk.py \
		--sel4 ../sel4 --boards $(SDK_CANDIDATE_BOARDS_CSV) \
		--configs release --gcc-toolchain-prefix-aarch64 aarch64-linux-gnu \
		--skip-tool --skip-initialiser --skip-docs --skip-tar --version $(SDK_CANDIDATE_VERSION)
# No --gcc-toolchain-prefix-riscv64: build_sdk.py's default for RISC-V is the
# bare-metal riscv64-unknown-elf triple, and that is the right one. Overriding
# it to riscv64-linux-gnu the way aarch64 is overridden fails: Ubuntu's
# riscv64-linux-gnu GCC defaults to PIE, so Microkit's own loader crt0.S links
# with "dangerous relocation: The addend isn't allowed for R_RISCV_GOT_HI20".
# The seL4 kernel itself builds either way; the loader does not.
	$(MAKE) sdk-candidate-check SEL4_SDK="$(SDK_CANDIDATE_DIR)/microkit/release/microkit-sdk-$(SDK_CANDIDATE_VERSION)"
	@echo 'Candidate built; runtime acceptance and default SDK adoption remain separate.'
	@echo 'SEL4_SDK=$(SDK_CANDIDATE_DIR)/microkit/release/microkit-sdk-$(SDK_CANDIDATE_VERSION)'
