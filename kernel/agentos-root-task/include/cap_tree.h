/* cap_tree.h — Per-PD capability accounting tree
 *
 * Tracks every capability derived from the root task's initial capabilities,
 * enabling clean VOS_DESTROY: walk all caps owned by a PD, call
 * seL4_CNode_Revoke on each, then seL4_CNode_Delete, and reclaim the
 * backing untyped memory.
 *
 * The tree is stored in a statically-sized slab (cap_tree_t) to avoid
 * dynamic allocation in the root task.  Each node records the derivation
 * parent so the tree can be traversed bottom-up during teardown.
 *
 * Version: 1
 * Contract location: (root task internal — no public IPC contract)
 */

#pragma once

/* Freestanding-safe stdint — works in no_std / freestanding C environments */
#ifdef __has_include
#  if __has_include(<stdint.h>)
#    include <stdint.h>
#  else
     typedef unsigned char      uint8_t;
     typedef unsigned short     uint16_t;
     typedef unsigned int       uint32_t;
     typedef unsigned long long uint64_t;
#  endif
#else
#  include <stdint.h>
#endif

/* ─── Limits and sentinels ───────────────────────────────────────────────── */

#define CAP_TREE_MAX_NODES  4096u  /* static slab — enough for full PD set */
#define CAP_TREE_NAME_MAX   48u

/* Sentinel value meaning "no node" — used in parent/child/sibling fields */
#define CAP_NODE_NONE    0xFFFFFFFFu

/* ─── cap_node_t flags ───────────────────────────────────────────────────── */

#define CAP_FLAG_ENDPOINT   0x01u  /* cap is a seL4 endpoint object */
#define CAP_FLAG_REVOKED    0x02u  /* seL4_CNode_Revoke has been called */
#define CAP_FLAG_DELETED    0x04u  /* seL4_CNode_Delete has been called */

/* ─── cap_node_t — one node in the capability derivation tree ────────────── */

/*
 * The tree mirrors seL4's internal derivation hierarchy.  When the root task
 * mints a derived cap (e.g. a badged endpoint, a smaller untyped, a frame
 * cap derived from a larger untyped), it inserts a child node here.
 *
 *   cap              — the seL4 capability slot value (CPtr)
 *   obj_type         — seL4 object type constant (seL4_TCBObject,
 *                      seL4_EndpointObject, seL4_UntypedObject, …)
 *   pd_owner         — PD index (into system_desc_t.pds[]) that owns this
 *                      cap; 0 is reserved for the root task itself
 *   parent_idx       — index of the parent node in cap_tree_t.nodes[];
 *                      CAP_NODE_NONE for root-level (initial) caps
 *   first_child_idx  — index of the first child node; CAP_NODE_NONE if leaf
 *   next_sibling_idx — index of the next sibling node; CAP_NODE_NONE if last
 *   flags            — CAP_FLAG_* bitmask
 *   name             — optional NUL-terminated debug label (e.g. "linux_vmm/tcb")
 */
typedef struct cap_node {
    uint64_t  cap;               /* seL4 capability slot (CPtr) */
    uint32_t  obj_type;          /* seL4 object type constant */
    uint32_t  pd_owner;          /* PD index (0 = root task) */
    uint32_t  parent_idx;        /* CAP_NODE_NONE if root-level */
    uint32_t  first_child_idx;   /* CAP_NODE_NONE if leaf */
    uint32_t  next_sibling_idx;  /* CAP_NODE_NONE if last sibling */
    uint32_t  flags;             /* CAP_FLAG_* bitmask */
    char      name[CAP_TREE_NAME_MAX]; /* optional debug label */
} cap_node_t;

_Static_assert(sizeof(cap_node_t) == 80u,
               "cap_node_t size changed — rebuild callers and update docs");

/* ─── cap_tree_t — global slab allocator state ───────────────────────────── */

/*
 * One cap_tree_t exists for the entire system, owned by the root task.
 * Nodes are allocated from a slab and threaded into a free list via
 * next_sibling_idx.  The tree itself uses the parent/child/sibling indices.
 */
typedef struct {
    cap_node_t nodes[CAP_TREE_MAX_NODES];
    uint32_t   free_head;   /* head of free-list (index into nodes[]) */
    uint32_t   used_count;  /* number of currently allocated nodes */
} cap_tree_t;

/* ─── Visitor callback ───────────────────────────────────────────────────── */

/*
 * Callback signature for cap_tree_walk_pd.
 * The visitor receives a pointer to the live node and a caller-supplied
 * context pointer.  It must not call cap_tree_remove on the current node;
 * call it after cap_tree_walk_pd returns.
 */
typedef void (*cap_node_visitor_t)(cap_node_t *node, void *ctx);

/* ─── API ────────────────────────────────────────────────────────────────── */

/*
 * cap_tree_init — initialize the cap tree
 *
 * Zero-fills all nodes and builds the free list.  Call once at root task
 * startup before any other cap_tree_* function.
 */
void cap_tree_init(cap_tree_t *tree);

/*
 * cap_tree_insert — insert a new cap as a child of parent_idx
 *
 * parent_idx: index of an existing node, or CAP_NODE_NONE to insert a
 *             root-level cap (one derived directly from the initial caps).
 * cap:        seL4 CPtr of the new capability.
 * obj_type:   seL4 object type constant.
 * pd_owner:   PD index that owns this cap.
 * name:       optional NUL-terminated label, or NULL.
 *
 * Returns the index of the new node in tree->nodes[], or CAP_NODE_NONE if
 * the slab is exhausted.
 */
uint32_t cap_tree_insert(cap_tree_t *tree, uint32_t parent_idx,
                          uint64_t cap, uint32_t obj_type, uint32_t pd_owner,
                          const char *name);

/*
 * cap_tree_remove — remove a leaf node from the tree and return it to the slab
 *
 * The node must have no children (first_child_idx == CAP_NODE_NONE).  The
 * caller is responsible for walking children first (e.g. during VOS_DESTROY
 * post-order traversal) before removing each node bottom-up.
 *
 * Calling cap_tree_remove on a non-leaf node is a programming error; the
 * function does nothing in that case.
 */
void cap_tree_remove(cap_tree_t *tree, uint32_t node_idx);

/*
 * cap_tree_walk_pd — walk all nodes owned by pd_id, invoking visitor
 *
 * Iterates every node whose pd_owner == pd_id and calls visitor(node, ctx).
 * Traversal order is unspecified.  The visitor must not insert or remove
 * nodes during traversal.
 *
 * Typical use: collect all node indices owned by a PD into a local array,
 * then revoke+delete+remove them after the walk completes.
 */
void cap_tree_walk_pd(cap_tree_t *tree, uint32_t pd_id,
                      cap_node_visitor_t visitor, void *ctx);

/*
 * cap_tree_find_cap — find a node by its seL4 CPtr value
 *
 * Returns the node index if found, or CAP_NODE_NONE if the cap is not in
 * the tree.  Linear scan; intended for diagnostic and integrity-check paths,
 * not the fast revocation path.
 */
uint32_t cap_tree_find_cap(cap_tree_t *tree, uint64_t cap);

/*
 * cap_tree_node — look up a node by index (bounds-checked)
 *
 * Returns a pointer to tree->nodes[idx], or NULL if idx is CAP_NODE_NONE
 * or out of bounds.  Inlined for use in tight loops.
 */
static inline cap_node_t *cap_tree_node(cap_tree_t *tree, uint32_t idx)
{
    if (idx == CAP_NODE_NONE || idx >= CAP_TREE_MAX_NODES)
        return (cap_node_t *)0;
    return &tree->nodes[idx];
}
