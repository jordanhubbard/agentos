#include <platform/net_virt_pump.h>
#include <stddef.h>

/* This fixture uses the production C layout, initialization and pump. Rust
 * never constructs the peer's queue descriptors in the interoperability test. */
int native_net_test_init(void *page, size_t bytes)
{
    aos_net_virt_client_t client = {0};
    if (!page || bytes != AOS_NET_CLIENT_STRIDE) return -1;
    aos_net_client_bind(page, 0, &client);
    aos_net_client_init_buffers(&client);
    return 0;
}

int native_net_test_pump(void *page, size_t bytes)
{
    aos_net_virt_client_t client = {0};
    aos_net_virt_t virt = {0};
    if (!page || bytes != AOS_NET_CLIENT_STRIDE) return -1;
    aos_net_client_bind(page, 0, &client);
    if (aos_net_virt_add_client(&virt, &client) != 0) return -1;
    return (int)aos_net_virt_pump(&virt);
}
