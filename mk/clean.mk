# Fixed repository output root: cleanup never follows BUILD_DIR overrides.
override CLEAN_REPO_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
override CLEAN_BUILD_ROOT := $(CLEAN_REPO_ROOT)/_build

.PHONY: clean clean-all clean-legacy
clean:
	rm -rf -- "$(CLEAN_BUILD_ROOT)"

clean-all: clean

# One-time cleanup for checkouts built before the _build layout.
clean-legacy:
	rm -rf -- "$(CLEAN_REPO_ROOT)/build" "$(CLEAN_REPO_ROOT)/target" \
		"$(CLEAN_REPO_ROOT)/kernel/build" \
		"$(CLEAN_REPO_ROOT)/kernel/loader/build" \
		"$(CLEAN_REPO_ROOT)/kernel/agentos-root-task/build" \
		"$(CLEAN_REPO_ROOT)/libvmm/arch" "$(CLEAN_REPO_ROOT)/libvmm/util" \
		"$(CLEAN_REPO_ROOT)/libvmm/virtio" "$(CLEAN_REPO_ROOT)/util"
	rm -f -- "$(CLEAN_REPO_ROOT)/libvmm/guest.o" "$(CLEAN_REPO_ROOT)/libvmm/guest.d" \
		"$(CLEAN_REPO_ROOT)/libvmm.a" "$(CLEAN_REPO_ROOT)/libsddf_util_debug.a" \
		"$(CLEAN_REPO_ROOT)/kernel/agentos-root-task/report.txt" \
		"$(CLEAN_REPO_ROOT)/tools/agentctl/agentctl" \
		"$(CLEAN_REPO_ROOT)/tests/ipc_bench/ipc_bench_host" \
		"$(CLEAN_REPO_ROOT)/".libvmm_cflags.*
