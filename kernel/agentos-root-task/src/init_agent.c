/*
 * agentOS InitAgent Protection Domain
 * 
 * Priority 100. First real agent. Bootstraps the agent ecosystem.
 * Receives start notification from controller, subscribes to EventBus,
 * prints the boot banner.
 */

#define AGENTOS_DEBUG 1
#include "agentos.h"
#include <stdint.h>

/* Channel IDs (from init_agent's perspective, matching agentos.system) */
#define CH_CONTROLLER 1   /* id="1" in controller<->initagent channel, init_agent end */
#define CH_EVENTBUS   2   /* id="2" in eventbus<->initagent channel, init_agent end */

static struct {
    bool started;
    bool eventbus_subscribed;
    uint32_t event_count;
    uint32_t query_count;
} state = { false, false, 0, 0 };

static void print_banner(void) {
    microkit_dbg_puts("\n");
    microkit_dbg_puts("╔══════════════════════════════════════════════════╗\n");
    microkit_dbg_puts("║                                                  ║\n");
    microkit_dbg_puts("║          agentOS v0.1.0-alpha                    ║\n");
    microkit_dbg_puts("║   The World's First OS for AI Agents             ║\n");
    microkit_dbg_puts("║                                                  ║\n");
    microkit_dbg_puts("║   Built on: seL4 Microkernel (formally proved)   ║\n");
#if defined(__aarch64__)
    microkit_dbg_puts("║   Arch:     seL4 Microkit / AArch64              ║\n");
#elif defined(__riscv)
    microkit_dbg_puts("║   Arch:     seL4 Microkit / RISC-V RV64          ║\n");
#else
    microkit_dbg_puts("║   Arch:     seL4 Microkit                        ║\n");
#endif
    microkit_dbg_puts("║                                                  ║\n");
    microkit_dbg_puts("║   Protection Domains:                            ║\n");
    microkit_dbg_puts("║     [*] controller  (prio  50) - system ctrl     ║\n");
    microkit_dbg_puts("║     [*] event_bus   (prio 200) - pub/sub bus     ║\n");
    microkit_dbg_puts("║     [*] init_agent  (prio 100) - bootstrapper    ║\n");
    microkit_dbg_puts("║                                                  ║\n");
    microkit_dbg_puts("║   Ready for agents. The future is running.       ║\n");
    microkit_dbg_puts("║                                                  ║\n");
    microkit_dbg_puts("║   Designed by Natasha on 2026-03-28              ║\n");
    microkit_dbg_puts("║   github.com/jordanhubbard/agentos               ║\n");
    microkit_dbg_puts("╚══════════════════════════════════════════════════╝\n");
    microkit_dbg_puts("\n");
}

/* Print a decimal number */
static void put_dec(uint32_t v) {
    if (v == 0) { microkit_dbg_puts("0"); return; }
    char buf[12];
    int i = 11;
    buf[i] = '\0';
    while (v > 0 && i > 0) {
        buf[--i] = '0' + (v % 10);
        v /= 10;
    }
    microkit_dbg_puts(&buf[i]);
}

/* Print a hex word */
static void put_hex32(uint32_t v) {
    static const char hex[] = "0123456789abcdef";
    char buf[11] = "0x00000000";
    for (int i = 9; i >= 2; i--) {
        buf[i] = hex[v & 0xf];
        v >>= 4;
    }
    microkit_dbg_puts(buf);
}

/*
 * Query EventBus status via PPC and print audit summary.
 * init_agent PPCs into the passive EventBus (higher priority)
 * to get the current event count and subscriber count.
 * This demonstrates real IPC between PDs for status queries.
 */
static void query_eventbus_status(void) {
    state.query_count++;

    microkit_dbg_puts("[init_agent] Querying EventBus status via PPC...\n");

    microkit_ppcall(CH_EVENTBUS, microkit_msginfo_new(MSG_EVENTBUS_STATUS, 0));

    uint64_t total_events = (uint64_t)microkit_mr_get(0);
    uint32_t subscribers  = (uint32_t)microkit_mr_get(1);

    microkit_dbg_puts("\n");
    microkit_dbg_puts("[init_agent] ── EventBus Audit Report ───────────────────\n");
    microkit_dbg_puts("[init_agent]   Total events published: ");
    put_dec((uint32_t)total_events);
    microkit_dbg_puts("\n");
    microkit_dbg_puts("[init_agent]   Active subscribers: ");
    put_dec(subscribers);
    microkit_dbg_puts("\n");
    microkit_dbg_puts("[init_agent]   Events since last query: ");
    uint32_t new_events = (uint32_t)total_events - state.event_count;
    put_dec(new_events);
    microkit_dbg_puts("\n");

    if (total_events > 0) {
        microkit_dbg_puts("[init_agent]   Data flow confirmed: agents exchanging messages\n");
    }

    microkit_dbg_puts("[init_agent] ────────────────────────────────────────────\n");

    state.event_count = (uint32_t)total_events;
}

static void subscribe_to_eventbus(void) {
    microkit_dbg_puts("[init_agent] Subscribing to EventBus...\n");
    
    /* PPC into event_bus (passive, higher priority) */
    microkit_mr_set(0, CH_EVENTBUS);  /* notify me on this channel */
    microkit_mr_set(1, 0);             /* topic_mask=0: all events */
    
    microkit_msginfo result = microkit_ppcall(
        CH_EVENTBUS,
        microkit_msginfo_new(MSG_EVENTBUS_SUBSCRIBE, 2)
    );
    
    if (microkit_msginfo_get_label(result) == 0) {
        state.eventbus_subscribed = true;
        microkit_dbg_puts("[init_agent] EventBus subscription: OK\n");
    } else {
        microkit_dbg_puts("[init_agent] EventBus subscription: FAILED\n");
    }
}

void init(void) {
    microkit_dbg_puts("[init_agent] Starting up...\n");
    
    /* Subscribe to EventBus right away */
    subscribe_to_eventbus();
    
    /* Notify controller we're ready */
    microkit_dbg_puts("[init_agent] Notifying controller: ready\n");
    microkit_notify(CH_CONTROLLER);
    
    /* Print the boot banner */
    print_banner();
    
    microkit_dbg_puts("[init_agent] Entering event loop. agentOS is ALIVE.\n");
    state.started = true;
}

void notified(microkit_channel ch) {
    switch (ch) {
        case CH_CONTROLLER:
            /* Controller notified us - could be a command or startup trigger */
            if (!state.started) {
                microkit_dbg_puts("[init_agent] Start signal from controller\n");
            } else {
                microkit_dbg_puts("[init_agent] Controller notification — querying EventBus\n");
                query_eventbus_status();
            }
            break;
            
        case CH_EVENTBUS:
            microkit_dbg_puts("[init_agent] EventBus notification\n");
            query_eventbus_status();
            break;
            
        default:
            microkit_dbg_puts("[init_agent] Unknown notification channel\n");
            break;
    }
}

microkit_msginfo protected(microkit_channel ch, microkit_msginfo msg) {
    (void)ch;
    uint64_t tag = microkit_msginfo_get_label(msg);
    
    switch (tag) {
        case MSG_INITAGENT_STATUS:
            microkit_mr_set(0, state.event_count);
            microkit_mr_set(1, state.eventbus_subscribed ? 1 : 0);
            return microkit_msginfo_new(0, 2);
            
        default:
            return microkit_msginfo_new(0xFFFF, 0);
    }
}
