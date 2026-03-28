;; agentOS Test WASM Module: Echo Service
;;
;; A minimal service that demonstrates the vibe-swap pipeline:
;;   - init() logs "Echo service initialized" via aos_log
;;   - handle_ppc(label, mr0, mr1, mr2, mr3) echoes back with mr0+1
;;   - health_check() returns 1 (healthy)
;;
;; Compile with: wat2wasm echo_service.wat -o echo_service.wasm
;;
;; Memory layout:
;;   0x000000 - 0x0000FF: String constants
;;   0x200000 - 0x20002F: PPC result buffer (5 × i64 = 40 bytes)
;;                         Written by handle_ppc for the host to read

(module
  ;; Import host functions from the "aos" module
  (import "aos" "aos_log" (func $aos_log (param i32 i32 i32)))
  (import "aos" "aos_time_us" (func $aos_time_us (result i64)))
  (import "aos" "aos_mem_read" (func $aos_mem_read (param i32 i32 i32) (result i32)))
  (import "aos" "aos_mem_write" (func $aos_mem_write (param i32 i32 i32) (result i32)))

  ;; Linear memory: 64 pages = 4MB (needed for PPC result buffer at 0x200000)
  (memory (export "memory") 33)  ;; 33 pages = 2.0625 MB (covers 0x200000 + 40 bytes)

  ;; String data for init message
  (data (i32.const 0) "Echo service initialized via agentOS vibe-swap!")

  ;; === Exported functions ===

  ;; init() — called when the service is loaded into a swap slot
  (func (export "init")
    ;; Log the init message: aos_log(level=0, msg_ptr=0, msg_len=47)
    (call $aos_log
      (i32.const 0)    ;; level: INFO
      (i32.const 0)    ;; msg_ptr: start of data segment
      (i32.const 47)   ;; msg_len: length of init string
    )
  )

  ;; handle_ppc(label: i64, mr0: i64, mr1: i64, mr2: i64, mr3: i64)
  ;; Writes results to memory at PPC_RESULT_OFFSET (0x200000)
  ;; Echo service: returns (label, mr0+1, mr1, mr2, mr3)
  (func (export "handle_ppc") (param $label i64) (param $mr0 i64) (param $mr1 i64) (param $mr2 i64) (param $mr3 i64)
    ;; Write result label at offset 0x200000
    (i64.store (i32.const 0x200000) (local.get $label))
    ;; Write mr0+1 at offset 0x200008
    (i64.store (i32.const 0x200008) (i64.add (local.get $mr0) (i64.const 1)))
    ;; Write mr1 at offset 0x200010
    (i64.store (i32.const 0x200010) (local.get $mr1))
    ;; Write mr2 at offset 0x200018
    (i64.store (i32.const 0x200018) (local.get $mr2))
    ;; Write mr3 at offset 0x200020
    (i64.store (i32.const 0x200020) (local.get $mr3))
  )

  ;; health_check() -> i32
  ;; Returns 1 if healthy, 0 if not
  (func (export "health_check") (result i32)
    (i32.const 1)
  )
)
