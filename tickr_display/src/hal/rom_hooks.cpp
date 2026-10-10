#include "rom_hooks.h"
#include <stdarg.h>
#include <stdio.h>
#include <sys/reent.h>
#include <esp32/rom/libc_stubs.h>
#include <esp_err.h>
#include <esp_private/panic_internal.h>

// ---------------------------------------------------------------------------
// %f in the ROM printf
// ---------------------------------------------------------------------------
// The ROM's vfprintf (linked through esp32.rom.newlib-nano.ld) handles a
// floating-point conversion by calling `syscall_table_ptr->_printf_float`.
// The IDF fills that slot only when it is itself built with
// CONFIG_NEWLIB_NANO_FORMAT; the Arduino core is not, so the slot is NULL and
// the first "%f" anywhere in the image would be a call to address 0 -> panic
// -> reboot. This stub keeps the format string in step (the double is taken
// from the va_list) and emits "?" through the printer callback.
//
// Signature as in esp32/rom/libc_stubs.h (newlib nano's _printf_float):
// `pdata` is newlib's private `struct _prt_data_t` (flags, width, precision)
// and is not consulted - padding and precision are ignored for the "?".
static int printf_float_stub(struct _reent* data, void* pdata, FILE* fp,
                             int (*pfunc)(struct _reent*, FILE*, const char*, size_t len),
                             va_list* ap) {
    (void)pdata;
    (void)va_arg(*ap, double);          // floats are promoted to double in varargs
    return pfunc(data, fp, "?", 1) < 0 ? -1 : 1;
}

// `%f` in a scanf: consume nothing, report "no conversion" (0 assignments).
static int scanf_float_stub(struct _reent* rptr, void* pdata, FILE* fp, va_list* ap) {
    (void)rptr; (void)pdata; (void)fp; (void)ap;
    return 0;
}

void rom_hooks_init() {
    // Both pointers address the same table in the IDF; set both in case
    // a future core separates them again.
    if (syscall_table_ptr_pro) {
        syscall_table_ptr_pro->_printf_float = printf_float_stub;
        syscall_table_ptr_pro->_scanf_float  = scanf_float_stub;
    }
    if (syscall_table_ptr_app && syscall_table_ptr_app != syscall_table_ptr_pro) {
        syscall_table_ptr_app->_printf_float = printf_float_stub;
        syscall_table_ptr_app->_scanf_float  = scanf_float_stub;
    }
}

// ---------------------------------------------------------------------------
// Core dump to flash - compiled out
// ---------------------------------------------------------------------------
// `-Wl,--wrap=esp_core_dump_init` / `--wrap=esp_core_dump_to_flash` redirect
// the two references the IDF makes (startup and the panic handler) here, so
// libespcoredump is not linked at all. The panic handler continues to the
// configured action after the (now empty) dump step: backtrace on the serial
// port, then reboot (CONFIG_ESP_SYSTEM_PANIC_PRINT_REBOOT). The `coredump`
// partition stays in the table and is simply not written.
extern "C" void __wrap_esp_core_dump_init(void) {}
extern "C" void __wrap_esp_core_dump_to_flash(panic_info_t* info) { (void)info; }
