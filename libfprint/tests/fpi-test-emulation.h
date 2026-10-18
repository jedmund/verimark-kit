/*
 * Static copy of the meson-generated tests/fpi-test-emulation.h.
 *
 * The vendored kit does not build the test suite, but libfprint/fpi-device.c
 * includes this header unconditionally. The values below only matter for
 * the emulation test harness (FP_DEVICE_EMULATION), which the kit does
 * not use; they are placeholders.
 */

#pragma once

#define FPI_EMULATION_ENV_VAR "FP_DEVICE_EMULATION"

#define FPI_EMULATION_HELPER_BUILDDIR "/nonexistent"

#define FPI_EMULATION_HELPER_INSTALLDIR "/usr/local/libexec/installed-tests/libfprint-2"

#define FPI_EMULATION_HELPER_MODULE "libfprint-test-emulation.so"
