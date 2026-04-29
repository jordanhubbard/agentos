# agentOS E2E Test Makefile
#
# Targets:
#   make e2e           — full E2E suite (QEMU + guests + SSH)
#   make e2e-guest     — guest SSH tests only (skips contract API tests)
#   make e2e-contract  — CC contract tests only (requires running agentOS)
#   make e2e-ssh       — SSH suite inside running guest
#   make e2e-dual-os   — concurrent Linux/Ubuntu + FreeBSD SSH/df proof
#   make e2e-gen-key   — generate test SSH key pair

SCRIPT_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
REPO_ROOT  := $(abspath $(SCRIPT_DIR)../..)

# Pass through any E2E_* overrides from the environment
E2E_ENV := E2E_TIMEOUT=$(E2E_TIMEOUT) \
           E2E_CC_PORT=$(E2E_CC_PORT) \
           E2E_SSH_PORT=$(E2E_SSH_PORT)

.PHONY: e2e e2e-guest e2e-contract e2e-ssh e2e-dual-os e2e-gen-key help

e2e: ## Full E2E suite — QEMU, guest VMs, SSH, and all contract tests
	@chmod +x $(SCRIPT_DIR)*.sh
	@cd $(REPO_ROOT) && $(E2E_ENV) bash $(SCRIPT_DIR)run_e2e.sh

e2e-guest: ## Guest SSH command suite only (guest must already be running)
	@chmod +x $(SCRIPT_DIR)suite_common.sh
	@cd $(REPO_ROOT) && $(E2E_ENV) bash $(SCRIPT_DIR)suite_common.sh

e2e-contract: ## CC contract API tests only (agentOS must be running at CC_PORT)
	@chmod +x $(SCRIPT_DIR)test_cc_contract.sh
	@cd $(REPO_ROOT) && BRIDGE_AVAILABLE=1 $(E2E_ENV) \
		bash $(SCRIPT_DIR)test_cc_contract.sh

e2e-ssh: ## SSH-in and run the portable Unix command suite
	@chmod +x $(SCRIPT_DIR)suite_common.sh
	@cd $(REPO_ROOT) && $(E2E_ENV) HAVE_SSH_TOOLS=1 \
		bash $(SCRIPT_DIR)suite_common.sh

e2e-dual-os: ## Run Ubuntu and FreeBSD agentOS E2E instances concurrently
	@chmod +x $(SCRIPT_DIR)run_dual_os_e2e.sh
	@cd $(REPO_ROOT) && $(E2E_ENV) bash $(SCRIPT_DIR)run_dual_os_e2e.sh

e2e-gen-key: ## Generate test SSH key (tests/e2e/id_ed25519)
	@if [ ! -f "$(SCRIPT_DIR)id_ed25519" ]; then \
		ssh-keygen -t ed25519 -N "" \
			-f "$(SCRIPT_DIR)id_ed25519" \
			-C "agentos-e2e-test" && \
		chmod 600 "$(SCRIPT_DIR)id_ed25519" && \
		echo "[INFO] Key generated: $(SCRIPT_DIR)id_ed25519" && \
		echo "[INFO] Add the public key to your guest image:"; \
		echo "         cat $(SCRIPT_DIR)id_ed25519.pub"; \
	else \
		echo "[INFO] Key already exists: $(SCRIPT_DIR)id_ed25519"; \
	fi

help: ## Show this help
	@grep -E '^[a-zA-Z_-]+:.*?## .*$$' $(MAKEFILE_LIST) | \
		awk 'BEGIN {FS = ":.*?## "}; {printf "  %-20s %s\n", $$1, $$2}'
