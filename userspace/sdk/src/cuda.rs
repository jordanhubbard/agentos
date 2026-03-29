//! CUDA PTX compute offload for agentOS
//!
//! Agents can submit GPU kernels as WASM modules with an embedded PTX
//! payload in the `agentos.cuda` custom section.  The VibeEngine extracts
//! and validates the PTX; the gpu_scheduler PD binds it to a GPU slot.
//!
//! # Example
//! ```rust
//! use agentos_sdk::cuda::CudaKernel;
//!
//! let ptx = include_bytes!("kernels/matmul.ptx");
//! let kernel = CudaKernel::new(ptx.to_vec(), "matmul_kernel".to_string());
//! kernel.submit(0)?;  // Submit to GPU slot 0
//! ```

use alloc::string::String;
use alloc::vec::Vec;

/// Error type for CUDA offload operations.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum CudaError {
    /// Invalid PTX payload (missing `.version` directive or empty)
    InvalidPtx,
    /// All 4 GPU slots are busy
    NoSlotAvailable,
    /// PTX payload exceeds 2MB limit
    PtxTooLarge,
    /// IPC call to gpu_scheduler failed
    IpcError(u32),
}

impl core::fmt::Display for CudaError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            CudaError::InvalidPtx       => write!(f, "Invalid PTX: must start with .version"),
            CudaError::NoSlotAvailable  => write!(f, "All GPU slots busy"),
            CudaError::PtxTooLarge      => write!(f, "PTX payload exceeds 2MB"),
            CudaError::IpcError(code)   => write!(f, "IPC error: {}", code),
        }
    }
}

/// Maximum PTX payload size (mirrors kernel constant MAX_PTX_SIZE).
pub const MAX_PTX_BYTES: usize = 2 * 1024 * 1024;

/// WASM custom section name that carries the PTX payload.
pub const CUDA_SECTION_NAME: &str = "agentos.cuda";

/// IPC op codes for the gpu_scheduler PD.
pub const OP_GPU_SUBMIT:   u32 = 0x50;
pub const OP_GPU_COMPLETE: u32 = 0x51;
pub const OP_GPU_STATUS:   u32 = 0x52;

/// A CUDA compute kernel for submission to an agentOS GPU slot.
///
/// Wraps a PTX source payload and an entry-point function name.
/// The kernel can be submitted to any free GPU slot via [`CudaKernel::submit`].
///
/// On real hardware (Sparky GB10 Blackwell), the gpu_scheduler PD will
/// JIT-compile the PTX via nvrtc and bind it to the slot's CUDA context.
/// On QEMU (no GPU), submission is accepted and tracked but no execution occurs.
#[derive(Debug, Clone)]
pub struct CudaKernel {
    /// Raw PTX source bytes. Must start with `.version`.
    pub ptx: Vec<u8>,
    /// Entry-point function name (e.g. `"my_kernel"`).
    pub entry: String,
}

impl CudaKernel {
    /// Create a new `CudaKernel`.
    ///
    /// # Arguments
    /// * `ptx`   - Raw PTX source bytes (must begin with `.version`)
    /// * `entry` - Entry-point function name
    pub fn new(ptx: Vec<u8>, entry: String) -> Self {
        Self { ptx, entry }
    }

    /// Validate the PTX payload without submitting.
    ///
    /// Checks that the PTX is non-empty, within size limits, and starts
    /// with the `.version` directive required by NVRTC.
    pub fn validate(&self) -> Result<(), CudaError> {
        if self.ptx.is_empty() {
            return Err(CudaError::InvalidPtx);
        }
        if self.ptx.len() > MAX_PTX_BYTES {
            return Err(CudaError::PtxTooLarge);
        }
        let magic = b".version";
        if self.ptx.len() < magic.len() || &self.ptx[..magic.len()] != magic {
            return Err(CudaError::InvalidPtx);
        }
        Ok(())
    }

    /// Submit this kernel to a specific GPU slot.
    ///
    /// # Arguments
    /// * `slot` - GPU slot index (0–3)
    ///
    /// In the current implementation this is a stub that validates the PTX
    /// and returns `Ok(())`.  A full implementation would IPC into the
    /// gpu_scheduler PD with `OP_GPU_SUBMIT`.
    ///
    /// # Errors
    /// Returns `CudaError` if validation fails or the IPC call fails.
    pub fn submit(&self, slot: u8) -> Result<(), CudaError> {
        self.validate()?;
        // IPC stub — on bare-metal this would be:
        //   microkit_mr_set(0, OP_GPU_SUBMIT);
        //   microkit_mr_set(1, slot as u32);
        //   microkit_mr_set(2, ptx_shm_offset);
        //   microkit_mr_set(3, self.ptx.len() as u32);
        //   microkit_ppcall(CH_GPU_SCHEDULER, microkit_msginfo_new(0, 4));
        let _ = slot;
        Ok(())
    }

    /// Query the busy-slot bitmask from gpu_scheduler.
    ///
    /// Returns a 4-bit mask where bit N is set if slot N is busy.
    /// Stub implementation always returns 0 (all free).
    pub fn query_slots() -> u32 {
        // IPC stub — on bare-metal:
        //   microkit_mr_set(0, OP_GPU_STATUS);
        //   let info = microkit_ppcall(CH_GPU_SCHEDULER, microkit_msginfo_new(0, 1));
        //   microkit_mr_get(1) as u32
        0
    }

    /// Release a GPU slot when kernel execution is complete.
    ///
    /// # Arguments
    /// * `slot` - GPU slot index to release (0–3)
    pub fn complete(slot: u8) -> Result<(), CudaError> {
        let _ = slot;
        // IPC stub — on bare-metal:
        //   microkit_mr_set(0, OP_GPU_COMPLETE);
        //   microkit_mr_set(1, slot as u32);
        //   microkit_ppcall(CH_GPU_SCHEDULER, microkit_msginfo_new(0, 2));
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;

    #[test]
    fn valid_ptx_passes_validation() {
        let ptx = b".version 7.5\n.target sm_90\n.address_size 64\n".to_vec();
        let k = CudaKernel::new(ptx, "test_kernel".into());
        assert!(k.validate().is_ok());
    }

    #[test]
    fn missing_version_directive_fails() {
        let ptx = b"// no version\n.target sm_90\n".to_vec();
        let k = CudaKernel::new(ptx, "test_kernel".into());
        assert_eq!(k.validate(), Err(CudaError::InvalidPtx));
    }

    #[test]
    fn empty_ptx_fails() {
        let k = CudaKernel::new(vec![], "entry".into());
        assert_eq!(k.validate(), Err(CudaError::InvalidPtx));
    }

    #[test]
    fn submit_stub_ok() {
        let ptx = b".version 7.5\n.target sm_90\n".to_vec();
        let k = CudaKernel::new(ptx, "kern".into());
        assert!(k.submit(0).is_ok());
    }
}
