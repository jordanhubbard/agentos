/**
 * WASM Validator — validates WASM binary structure and agentOS service contracts.
 *
 * Checks:
 *   1. Magic bytes (0x00 0x61 0x73 0x6D = "\0asm")
 *   2. WASM version (must be 1)
 *   3. Required exports: init, handle_ppc, health_check
 *   4. Required imports: "aos" module (host bindings)
 *   5. Memory export exists
 *   6. Size limits (max 1MB for swap slot)
 *
 * Copyright (c) 2026 The agentOS Project — Bullwinkle 🫎
 */

const WASM_MAGIC = Buffer.from([0x00, 0x61, 0x73, 0x6D]);
const WASM_VERSION = Buffer.from([0x01, 0x00, 0x00, 0x00]);
const MAX_WASM_SIZE = 1 * 1024 * 1024; // 1MB — swap slot code region limit

// Required exports for an agentOS service
const REQUIRED_EXPORTS = ['init', 'handle_ppc', 'health_check', 'memory'];

/**
 * Validate a WASM binary for agentOS service compatibility.
 *
 * @param {Buffer|Uint8Array} wasmBytes - The WASM binary
 * @returns {{ valid: boolean, errors: string[], warnings: string[], exports: string[], imports: string[] }}
 */
export function validateWasm(wasmBytes) {
  const errors = [];
  const warnings = [];
  const exports = [];
  const imports = [];

  // 1. Size check
  if (wasmBytes.length > MAX_WASM_SIZE) {
    errors.push(`WASM binary too large: ${wasmBytes.length} bytes (max ${MAX_WASM_SIZE})`);
  }
  if (wasmBytes.length < 8) {
    errors.push(`WASM binary too small: ${wasmBytes.length} bytes (minimum 8 for header)`);
    return { valid: false, errors, warnings, exports, imports };
  }

  // 2. Magic bytes
  const magic = wasmBytes.slice(0, 4);
  if (!Buffer.from(magic).equals(WASM_MAGIC)) {
    errors.push(`Invalid WASM magic: got ${Buffer.from(magic).toString('hex')}, expected ${WASM_MAGIC.toString('hex')}`);
    return { valid: false, errors, warnings, exports, imports };
  }

  // 3. Version
  const version = wasmBytes.slice(4, 8);
  if (!Buffer.from(version).equals(WASM_VERSION)) {
    errors.push(`Unsupported WASM version: ${Buffer.from(version).toString('hex')}`);
  }

  // 4. Try to compile and inspect via WebAssembly API
  try {
    const mod = new WebAssembly.Module(wasmBytes);

    // Inspect exports
    const modExports = WebAssembly.Module.exports(mod);
    for (const exp of modExports) {
      exports.push(`${exp.name}:${exp.kind}`);
    }

    // Check required exports
    const exportNames = modExports.map(e => e.name);
    for (const req of REQUIRED_EXPORTS) {
      if (!exportNames.includes(req)) {
        if (req === 'memory') {
          warnings.push(`Missing "memory" export — host will provide linear memory`);
        } else {
          errors.push(`Missing required export: "${req}"`);
        }
      }
    }

    // Check init is a function
    const initExport = modExports.find(e => e.name === 'init');
    if (initExport && initExport.kind !== 'function') {
      errors.push(`"init" export must be a function, got ${initExport.kind}`);
    }

    // Check handle_ppc is a function
    const ppcExport = modExports.find(e => e.name === 'handle_ppc');
    if (ppcExport && ppcExport.kind !== 'function') {
      errors.push(`"handle_ppc" export must be a function, got ${ppcExport.kind}`);
    }

    // Inspect imports
    const modImports = WebAssembly.Module.imports(mod);
    for (const imp of modImports) {
      imports.push(`${imp.module}.${imp.name}:${imp.kind}`);
    }

    // Check for "aos" module imports (expected)
    const aosImports = modImports.filter(i => i.module === 'aos');
    if (aosImports.length === 0) {
      warnings.push('No "aos" module imports found — this module does not use agentOS host bindings');
    }

    // Warn about unknown import modules
    const importModules = [...new Set(modImports.map(i => i.module))];
    for (const mod of importModules) {
      if (mod !== 'aos' && mod !== 'env') {
        warnings.push(`Unknown import module "${mod}" — only "aos" and "env" are provided`);
      }
    }

  } catch (err) {
    errors.push(`WASM compilation failed: ${err.message}`);
  }

  return {
    valid: errors.length === 0,
    errors,
    warnings,
    exports,
    imports,
  };
}

/**
 * Quick check: is this buffer a valid WASM binary?
 */
export function isWasm(bytes) {
  return bytes.length >= 4 && Buffer.from(bytes.slice(0, 4)).equals(WASM_MAGIC);
}
