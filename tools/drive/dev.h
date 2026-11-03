/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The modes of fenestal-dev (dev.c), each a program's main() in a file of
 * its own: argv[0] is the mode's name and the rest are its arguments.
 */
#ifndef FNL_DEV_H
#define FNL_DEV_H

int pace_main(int argc, char **argv);       /* pace.c */
int lifecycle_main(int argc, char **argv);  /* lifecycle.c */

#endif
