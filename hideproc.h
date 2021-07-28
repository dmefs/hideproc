#ifndef HIDEPROC_H
#define HIDEPROC_H

#define HOOK(_name, _hook, _orig)                          \
    {                                                      \
        .name = (_name), .func = (_hook), .orig = (_orig), \
    }

#endif