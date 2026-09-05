#include <stdio.h>

#include <platform/guest_memory_layout.h>

static int check(int condition, const char *name)
{
    printf("%s - %s\n", condition ? "ok" : "not ok", name);
    return condition ? 0 : 1;
}

int main(void)
{
    int failed = 0;

    failed += check(AOS_LINUX_GUEST_RAM_MB == 1024u,
                    "dual Ubuntu capacity is one GiB");
    failed += check(AOS_FREEBSD_GUEST_RAM_MB == 256u,
                    "dual FreeBSD capacity is 256 MiB");
    failed += check(AOS_FREEBSD_GUEST_RAM_BASE +
                        AOS_FREEBSD_GUEST_RAM_SIZE <=
                    AOS_LINUX_GUEST_RAM_BASE,
                    "dual guest virtual RAM windows do not overlap");
    failed += check(AOS_LINUX_GUEST_DTB_BASE >= AOS_LINUX_GUEST_RAM_BASE &&
                    AOS_LINUX_GUEST_DTB_BASE <
                        AOS_LINUX_GUEST_RAM_BASE + AOS_LINUX_GUEST_RAM_SIZE &&
                    AOS_FREEBSD_GUEST_DTB_BASE >=
                        AOS_FREEBSD_GUEST_RAM_BASE &&
                    AOS_FREEBSD_GUEST_DTB_BASE <
                        AOS_FREEBSD_GUEST_RAM_BASE +
                        AOS_FREEBSD_GUEST_RAM_SIZE,
                    "both device trees fit their allocated RAM windows");

    printf("1..4\n");
    return failed == 0 ? 0 : 1;
}
