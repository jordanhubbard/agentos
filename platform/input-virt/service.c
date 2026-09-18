/* SPDX-License-Identifier: BSD-2-Clause */
#include <platform/input.h>

static uint32_t load(const uint32_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void publish(uint32_t *p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }

int aos_input_service_init(aos_input_service_t *s, aos_input_frontend_t *f,
    aos_input_client_region_t *const clients[AOS_INPUT_CLIENTS], uint32_t mask)
{
    if (!s || !f || !clients || mask & ~((1u<<AOS_INPUT_CLIENTS)-1u)) return -1;
    for (unsigned i=0; i<AOS_INPUT_CLIENTS; ++i)
        if ((mask & (1u<<i)) && !clients[i]) return -1;
    aos_input_service_t initialized={.frontend=f, .allowed_mask=mask};
    for (unsigned i=0; i<AOS_INPUT_CLIENTS; ++i) initialized.clients[i]=clients[i];
    *s=initialized;
    return 0;
}
int aos_input_submit(aos_input_frontend_t *f, const aos_input_request_t *q)
{
    if (!f || !q) return -1;
    uint32_t head=load(&f->req_head), tail=load(&f->req_tail);
    if (tail-head>=AOS_INPUT_REQUEST_CAPACITY) return -1;
    f->requests[tail%AOS_INPUT_REQUEST_CAPACITY]=*q;
    publish(&f->req_tail, tail+1u);
    return 0;
}
int aos_input_receive(aos_input_frontend_t *f, aos_input_response_t *p)
{
    if (!f || !p) return -1;
    uint32_t head=load(&f->resp_head), tail=load(&f->resp_tail);
    if (tail==head || tail-head>AOS_INPUT_REQUEST_CAPACITY) return -1;
    *p=f->responses[head%AOS_INPUT_REQUEST_CAPACITY];
    publish(&f->resp_head, head+1u);
    return 0;
}
int aos_input_event_receive(aos_input_event_queue_t *q, aos_input_event_t *event)
{
    if (!q || !event) return -1;
    uint32_t head=load(&q->head), tail=load(&q->tail);
    if (head==tail || tail-head>AOS_INPUT_EVENT_CAPACITY) return -1;
    *event=q->events[head%AOS_INPUT_EVENT_CAPACITY];
    publish(&q->head, head+1u);
    return 0;
}
static int valid_batch(const aos_input_request_t *q)
{
    if (q->version!=AOS_INPUT_VERSION || q->device>=AOS_INPUT_DEVICES ||
        !q->count || q->count>AOS_INPUT_BATCH_EVENTS ||
        q->reserved[0] || q->reserved[1] || q->reserved[2]) return 0;
    for (unsigned i=0; i<q->count; ++i) {
        const aos_input_event_t *e=&q->events[i];
        if (i==q->count-1) {
            if (e->type || e->code || e->value) return 0;
        } else if (q->device==AOS_INPUT_KEYBOARD) {
            if (e->type!=1 || !e->code || e->code>255 || e->value<0 || e->value>2) return 0;
        } else if (e->type==1) {
            if (e->code<0x110 || e->code>0x117 || e->value<0 || e->value>1) return 0;
        } else if (e->type!=2 || (e->code!=0 && e->code!=1 && e->code!=6 && e->code!=8)) {
            return 0;
        }
    }
    return 1;
}
static uint32_t deliver(aos_input_service_t *s, const aos_input_request_t *q)
{
    int release=q->version==AOS_INPUT_RELEASE_VERSION && q->count==0 &&
        q->device<AOS_INPUT_DEVICES && !q->reserved[0] && !q->reserved[1] && !q->reserved[2];
    if (!release && !valid_batch(q)) return AOS_INPUT_BAD_REQUEST;
    if (q->client>=AOS_INPUT_CLIENTS || !(s->allowed_mask & (1u<<q->client)))
        return AOS_INPUT_DENIED;
    if (release) {
        s->releasing[q->client] |= 1u<<q->device;
        return AOS_INPUT_OK;
    }
    if (s->releasing[q->client] & (1u<<q->device)) return AOS_INPUT_WOULD_BLOCK;
    aos_input_event_queue_t *out=&s->clients[q->client]->devices[q->device];
    uint32_t head=load(&out->head), tail=load(&out->tail), occupied=tail-head;
    if (occupied>AOS_INPUT_EVENT_CAPACITY || q->count>AOS_INPUT_EVENT_CAPACITY-occupied)
        return AOS_INPUT_WOULD_BLOCK;
    for (unsigned i=0; i<q->count; ++i)
        out->events[(tail+i)%AOS_INPUT_EVENT_CAPACITY]=q->events[i];
    for (unsigned i=0;i<q->count;i++) {
        const aos_input_event_t *e=&q->events[i];
        if (e->type!=1 || e->value==2) continue;
        unsigned index=q->device==AOS_INPUT_KEYBOARD ? e->code : e->code-0x110u;
        uint32_t *word=&s->held[q->client][q->device][index/32u], bit=1u<<(index%32u);
        if (e->value) *word |= bit;
        else *word &= ~bit;
    }
    publish(&out->tail, tail+q->count);
    return AOS_INPUT_OK;
}

static unsigned release_pending(aos_input_service_t *s, uint32_t *ready)
{
    unsigned progress=0;
    for (unsigned client=0;client<AOS_INPUT_CLIENTS;client++) {
        for (unsigned device=0;device<AOS_INPUT_DEVICES;device++) {
            if (!(s->releasing[client] & (1u<<device))) continue;
            aos_input_event_queue_t *out=&s->clients[client]->devices[device];
            uint32_t tail=load(&out->tail), occupied=tail-load(&out->head), count=0;
            if (occupied>AOS_INPUT_EVENT_CAPACITY) continue;
            uint32_t room=AOS_INPUT_EVENT_CAPACITY-occupied;
            unsigned left=0;
            for (unsigned index=0;index<256;index++) {
                uint32_t *word=&s->held[client][device][index/32u], bit=1u<<(index%32u);
                if (!(*word & bit)) continue;
                if (count+1u>=room || count>=AOS_INPUT_BATCH_EVENTS-1u) { left++; continue; }
                uint16_t code=(uint16_t)(device==AOS_INPUT_KEYBOARD ? index : index+0x110u);
                out->events[(tail+count++)%AOS_INPUT_EVENT_CAPACITY]=(aos_input_event_t){1,code,0};
                *word &= ~bit;
            }
            if (count) {
                out->events[(tail+count++)%AOS_INPUT_EVENT_CAPACITY]=(aos_input_event_t){0,0,0};
                publish(&out->tail,tail+count);
                if (ready) *ready |= 1u<<client;
                progress++;
            }
            if (!left) { s->releasing[client] &= ~(1u<<device); progress++; }
        }
    }
    return progress;
}
unsigned aos_input_pump(aos_input_service_t *s, uint32_t *ready)
{
    if (ready) *ready=0;
    if (!s || !s->frontend) return 0;
    aos_input_frontend_t *f=s->frontend;
    uint32_t head=load(&f->req_head), tail=load(&f->req_tail);
    uint32_t pending=tail-head;
    if (pending>AOS_INPUT_REQUEST_CAPACITY) return 0;
    unsigned processed=0;
    while (processed<pending) {
        uint32_t out=load(&f->resp_tail), consumed=load(&f->resp_head);
        if (out-consumed>=AOS_INPUT_REQUEST_CAPACITY) break;
        aos_input_request_t q=f->requests[head%AOS_INPUT_REQUEST_CAPACITY];
        uint32_t status=deliver(s,&q);
        f->responses[out%AOS_INPUT_REQUEST_CAPACITY]=(aos_input_response_t){
            q.version==AOS_INPUT_RELEASE_VERSION && !q.count ? AOS_INPUT_RELEASE_VERSION : AOS_INPUT_VERSION,
            q.id,status,status==AOS_INPUT_OK ? q.count : 0};
        if (status==AOS_INPUT_OK && q.count && ready) *ready |= 1u<<q.client;
        publish(&f->resp_tail,out+1u);
        publish(&f->req_head,++head);
        ++processed;
    }
    return processed+release_pending(s,ready);
}
