# The TIMER5 entry trap: a static C function silently swallows the asm wrapper

## Symptom

Screen draws (or the splash text sits there), and then:

- the main loop never runs (any "tick / heartbeat" on screen stays put),
- USB never enumerates (no MIDI device on the host),
- no MIDI is emitted, ever,
- no crash screen, no watchdog reset loop, no fault -- it just sits.

One timer tick after `fm1_timer5_start()` and the unit is a picture frame.

## Cause

`firmware/hal/fm1_isr.S` holds the real hardware entry:

        FM1_ISR isr_timer5, fm1_timer5_irq
    =>
        isr_timer5:
            [--sp] = {psr, rets, reti}     <- save the interrupted context
            [--sp] = {r3-r0}
            call fm1_timer5_irq
            {r3-r0} = [sp++]
            {psr, rets, reti} = [sp++]
            csync                          <- clang does not emit this
            rti                            <- return from interrupt

An application that writes its own

    static void isr_timer5(void) { ... }

and passes *that* to `fm1_timer5_start(isr_timer5, 4)` shadows the symbol. Two
things then happen, silently:

1. Nothing references the wrapper in fm1_isr.S, so `--gc-sections` drops it
   (its section is `.text.isr_timer5`, one symbol per wrapper).
2. The vector table points at the C function. It has no `rti` and never
   restored `psr/rets/reti`, so the first tick leaves the CPU with a return
   address that means nothing.

The linker is happy: the symbol `isr_timer5` is defined, exactly as asked for.
There is no warning, because from the compiler's view a function pointer is a
function pointer. This is not a typo you can see in a diff -- it costs a flash
cycle to find, and it looks like a dead USB stack.

## The rule

- The C body **must** be named `fm1_timer5_irq` (that is what the wrapper calls).
- `fm1_timer5_start()` **must** be given `isr_timer5`, the assembler wrapper.
- Declare it, do not define it: `extern void isr_timer5(void);`
- Same for the audio ISR: body `fm1_alnk0_irq`, entry `isr_alnk0`.

## Verify without hardware

In the linked image the wrapper must exist and end in `csync` / `rti`:

    objdump -d build/<name>.elf | awk '/^[0-9a-f]+ <isr_timer5>:/{f=1} f{print; if(/rti/) exit}'

A wrong build shows a C prologue (`[--sp] = {rets, r15-r4}`, `sp += -N`) and
zero `rti`. Add the check to the build so it cannot regress:

    grep -q '^isr_timer5:' build/ctrl.dis || echo "ISR WRAPPER DROPPED"

## Confirmed instances

- `fm1-ctrl` smoke build, first flash (2026-10-23): screen on, no USB device,
  no notes. `isr_timer5` was a C function; `ctrl_main.c` had the same bug in
  the full build.
- `driftseq` `firmware/src/app_main.c:41,123` (same lineage, same HAL): its
  `isr_timer5` is also a C body with no `rti` in the linked image, so its
  TIMER5 ISR cannot return either -- and it also links `isr_alnk0` for the
  audio render, which is unreachable for the same reason.
