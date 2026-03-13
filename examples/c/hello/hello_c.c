#include "stdio.h"

int
main(int argc, char **argv)
{
    static int printed = 0;
    (void)argc;
    (void)argv;

    if (!printed) {
        printed = 1;
        static const char line1[] = "Hello from C on WASMOS!\n";
        static const char line2[] = "This is a tiny WASMOS-APP written in C.\n";
        static const char line3[] = "Entry: main\n";
        putsn(line1, sizeof(line1) - 1);
        putsn(line2, sizeof(line2) - 1);
        putsn(line3, sizeof(line3) - 1);
    }

    return 0;
}
