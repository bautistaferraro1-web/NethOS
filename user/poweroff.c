#define NLIB_MAIN
#include "nlib.h"

#define LINUX_REBOOT_MAGIC1 0xFEE1DEADUL
#define LINUX_REBOOT_MAGIC2 0x28121969UL
#define LINUX_REBOOT_CMD_POWER_OFF 0x4321FEDCUL

int nmain(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    long rc = sys3(169, (long)LINUX_REBOOT_MAGIC1,
                   (long)LINUX_REBOOT_MAGIC2,
                   (long)LINUX_REBOOT_CMD_POWER_OFF);

    print("poweroff: la llamada al kernel devolvió ");
    if (rc < 0) {
        print_num((u64)(-rc));
        print(" (error)\n");
    } else {
        print_num((u64)rc);
        print(" (inesperado)\n");
    }
    return 1;
}
