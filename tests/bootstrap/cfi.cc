/* tests/bootstrap/cfi.cc - M98's cheapest reproduction
 *
 * A function that needs an unwind table and therefore makes the compiler
 * emit `.cfi_personality`, which is the directive the machine's own `as`
 * turned out not to accept - found by the first C++ translation unit
 * this machine ever compiled for itself, fifteen minutes into a compile
 * that then failed in the assembler.
 *
 * Two lines of C++ that reach the same directive in about a second, so
 * the question "what exactly is gas reading" is asked cheaply. Kept
 * after the fix for the same reason every reproduction here is kept: it
 * is the fixture that fails if it comes back.
 */
struct Guard {
    ~Guard();
};
void may_throw();

void needs_unwind() {
    Guard g;
    may_throw();
}
