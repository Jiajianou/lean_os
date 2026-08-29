#pragma once

void panic(const char *msg) __attribute__((noreturn));

/* M70: paint the panic band and message onto the framebuffer, WITHOUT
 * halting. panic() calls this on its way down; it is exposed so the boot
 * self-test can check that the painting works while the machine is still
 * able to report the result.
 *
 * That split is deliberate and is the only honest way to test this. A
 * test that triggered a real panic could not then tell anybody what it
 * found - the machine is stopped, which is the point. So the *rendering*
 * is verified here, by painting and reading the pixels back, and the
 * halt-and-broadcast half stays exercised only by real panics, where it
 * has always been. Splitting them is what makes one of the two testable
 * instead of neither. */
void panic_render(const char *msg);
