/*
 * agentOS EventBus Protection Domain
 *
 * The EventBus is the communication backbone of agentOS.
 * It runs as a passive server (only executes when called via PPC or IPC).
 *
 * Implementation:
 *   - Shared ring buffer in memory region 'eventbus_ring'
 *   - Subscribers read directly from the ring (zero-copy)
 *   - Publishers write via PPC (serialized through this PD)
 *   - Notifications sent to subscribers when new events arrive
 *
 * This is v0.1 - synchronous, single-producer-many-consumer.
 * Future versions will support async multi-producer with priority queues.
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"

/* The ring buffer lives in shared memory - Microkit sets this via setvar_vaddr */
uintptr_t eventbus_ring_vaddr;
#define EVENTBUS_RING ((volatile agentos_ring_header_t *)eventbus_ring_vaddr)

/* Subscription table */
#define MAX_SUBSCRIBERS 64

typedef struct {
    bool active;
    microkit_channel notify_ch;   /* channel to notify on new events */
    uint32_t topic_mask;          /* event kind bitmask (0 = all events) */
    uint64_t last_seq;            /* last seen sequence number */
} subscriber_t;

static subscriber_t subscribers[MAX_SUBSCRIBERS];
static uint32_t sub_count = 0;
static uint64_t event_seq = 0;

/* Initialize the ring buffer */
static void eventbus_init_ring(void) {
    volatile agentos_ring_header_t *ring = EVENTBUS_RING;
    ring->magic    = AGENTOS_RING_MAGIC;
    ring->version  = 1;
    ring->capacity = (0x40000 - sizeof(agentos_ring_header_t)) / sizeof(agentos_event_t);
    ring->head     = 0;
    ring->tail     = 0;
}

/* Write an event to the ring */
static bool eventbus_write(uint32_t kind, uint32_t source_pd, 
                            const uint8_t *payload, uint32_t payload_len) {
    volatile agentos_ring_header_t *ring = EVENTBUS_RING;
    volatile agentos_event_t *events = (volatile agentos_event_t *)
        ((uint8_t *)EVENTBUS_RING + sizeof(agentos_ring_header_t));
    
    uint64_t next_head = (ring->head + 1) % ring->capacity;
    if (next_head == ring->tail) {
        /* Ring full - drop event (in production: apply backpressure) */
        microkit_dbg_puts("[event_bus] WARNING: ring buffer full, dropping event\n");
        return false;
    }
    
    volatile agentos_event_t *slot = &events[ring->head];
    slot->seq          = event_seq++;
    slot->kind         = kind;
    slot->source_pd    = source_pd;
    slot->payload_len  = payload_len < 64 ? payload_len : 64;
    
    /* Copy payload (truncated to 64 bytes inline) */
    for (uint32_t i = 0; i < slot->payload_len; i++) {
        slot->payload[i] = payload ? payload[i] : 0;
    }
    
    /* Memory barrier before updating head */
    __asm__ volatile("" ::: "memory");
    ring->head = next_head;
    
    return true;
}

/* Notify all active subscribers */
static void eventbus_notify_all(void) {
    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        if (subscribers[i].active) {
            microkit_notify(subscribers[i].notify_ch);
        }
    }
}

/*
 * init() - EventBus is passive but still gets init() called
 */
void init(void) {
    microkit_dbg_puts("[event_bus] Initializing...\n");
    
    /* Clear subscriber table */
    for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
        subscribers[i].active = false;
    }
    
    /* Wait to be initialized by the monitor via PPC */
    microkit_dbg_puts("[event_bus] Waiting for monitor init call...\n");
}

/*
 * notified() - EventBus got a notification
 * This happens when a publisher fires a notify instead of a PPC.
 * For high-priority events only.
 */
void notified(microkit_channel ch) {
    agentos_log_channel("event_bus", ch);
    
    /* Check for pending events from publishers */
    /* In v0.1, all publishing goes through PPC - notifications are for subscribers */
    microkit_dbg_puts("[event_bus] Spurious notification (check publisher)\n");
}

/*
 * protected() - EventBus PPC handler
 *
 * This is the main EventBus API:
 *   MSG_EVENTBUS_INIT      -> Initialize ring buffer
 *   MSG_EVENTBUS_SUBSCRIBE -> Add a subscriber
 *   MSG_EVENTBUS_STATUS    -> Return status info
 */
microkit_msginfo protected(microkit_channel ch, microkit_msginfo msg) {
    uint64_t tag = microkit_msginfo_get_label(msg);
    
    switch (tag) {
        case MSG_EVENTBUS_INIT: {
            microkit_dbg_puts("[event_bus] Init from monitor\n");
            eventbus_init_ring();
            microkit_dbg_puts("[event_bus] Ring buffer initialized\n");
            
            /* Announce we're ready */
            eventbus_write(MSG_EVENT_SYSTEM_READY, 0, NULL, 0);
            
            microkit_dbg_puts("[event_bus] READY\n");
            return microkit_msginfo_new(MSG_EVENTBUS_READY, 0);
        }
        
        case MSG_EVENTBUS_SUBSCRIBE: {
            /* mr[0] = notify channel, mr[1] = topic mask */
            uint32_t notify_ch = (uint32_t)microkit_mr_get(0);
            uint32_t topic_mask = (uint32_t)microkit_mr_get(1);
            
            /* Find a free subscriber slot */
            for (int i = 0; i < MAX_SUBSCRIBERS; i++) {
                if (!subscribers[i].active) {
                    subscribers[i].active     = true;
                    subscribers[i].notify_ch  = notify_ch;
                    subscribers[i].topic_mask = topic_mask;
                    subscribers[i].last_seq   = event_seq;
                    sub_count++;
                    
                    microkit_dbg_puts("[event_bus] New subscriber registered\n");
                    microkit_mr_set(0, i); /* return subscription handle */
                    return microkit_msginfo_new(0, 1);
                }
            }
            
            /* No free slots */
            microkit_dbg_puts("[event_bus] ERROR: subscriber table full\n");
            return microkit_msginfo_new(0xFFFF, 0);
        }
        
        case MSG_EVENTBUS_UNSUBSCRIBE: {
            uint32_t handle = (uint32_t)microkit_mr_get(0);
            if (handle < MAX_SUBSCRIBERS && subscribers[handle].active) {
                subscribers[handle].active = false;
                sub_count--;
                microkit_dbg_puts("[event_bus] Subscriber removed\n");
                return microkit_msginfo_new(0, 0);
            }
            return microkit_msginfo_new(0xFFFF, 0);
        }
        
        case MSG_EVENTBUS_STATUS: {
            microkit_mr_set(0, event_seq);
            microkit_mr_set(1, sub_count);
            return microkit_msginfo_new(0, 2);
        }
        
        default: {
            /* Unknown tag - check if it's a publish request */
            /* In v0.1, publishing is done by writing to ring directly */
            /* (publishers have write capability to the ring region) */
            uint32_t kind       = (uint32_t)(tag & 0xFFFF);
            uint32_t payload_len = (uint32_t)microkit_msginfo_get_count(msg);
            
            /* Write event to ring */
            eventbus_write(kind, ch, NULL, 0);
            
            /* Notify subscribers */
            eventbus_notify_all();
            
            return microkit_msginfo_new(0, 0);
        }
    }
}
