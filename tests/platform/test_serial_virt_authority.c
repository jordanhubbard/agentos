#include "contracts/serial_virt_contract.h"
#include <platform/serial_virt_layout.h>
#include <stdio.h>

int main(void)
{
    const uint64_t badges[] = {0, VIRT_CLIENT_BADGE_PRIMARY,
        VIRT_CLIENT_BADGE_SECONDARY, SERIAL_VIRT_FRONTEND_BADGE,
        VIRT_CLIENT_BADGE_PRIMARY | (UINT64_C(1) << 32), UINT64_MAX,
        SERIAL_VIRT_OPERATOR_BADGE};
    const uint32_t clients[] = {0, 1, 2, 3, UINT32_MAX};
    const uint32_t roles[] = {0, 1, 2, UINT32_MAX};
    unsigned checks = 0, failures = 0;
    for (unsigned b = 0; b < sizeof(badges) / sizeof(badges[0]); b++)
        for (unsigned c = 0; c < sizeof(clients) / sizeof(clients[0]); c++)
            for (unsigned r = 0; r < sizeof(roles) / sizeof(roles[0]); r++) {
                int expected = (b == 1 && c == 0 && r == 0) ||
                               (b == 2 && c == 1 && r == 0) ||
                               (b == 3 && c < 3 && r == 1) ||
                               (b == 6 && c == 2 && r == 2);
                if (!!serial_virt_authorized(badges[b], clients[c], roles[r]) != expected)
                    failures++;
                checks++;
            }
    printf("%s - %u serial role/client/badge combinations\n1..1\n",
           failures ? "not ok" : "ok", checks);
    return failures ? 1 : 0;
}
