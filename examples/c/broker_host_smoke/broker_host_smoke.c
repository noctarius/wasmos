#include "stdio.h"

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    static const char msg[] = "[test] broker host smoke\n";
    (void)putsn(msg, sizeof(msg) - 1u);
    return 0;
}
