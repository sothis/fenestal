/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * The kernel's input ABI: the event the KMS backend reads from evdev and the
 * button codes the Wayland backend reads from its pointer. Linux keeps its
 * own UAPI headers; FreeBSD's evdev has the same structure, codes and ioctls
 * under its own names, and its ioctl numbers are its own -- which is why the
 * header is included and not the numbers written out here.
 */

#ifndef FNL_EVDEV_INPUT_H
#define FNL_EVDEV_INPUT_H

#if defined(__linux__)
#include <linux/input.h>
#include <linux/input-event-codes.h>
#elif defined(__FreeBSD__)
#include <dev/evdev/input.h>
#include <dev/evdev/input-event-codes.h>
#else
#error "evdev is Linux's or FreeBSD's"
#endif

#endif /* FNL_EVDEV_INPUT_H */
