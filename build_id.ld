/* The GNU build ID in flash, after .rodata, where the firmware can read it:
 * crash reports name the build that crashed, and the server finds the same
 * note in the uploaded ELF and UF2. Added with -T; INSERT keeps the SDK's script,
 * and with no region named it follows .rodata in flash, whichever order the
 * linker reads the scripts in. */
SECTIONS
{
    .ps_build_id : ALIGN(4) {
        __ps_build_id_start = .;
        KEEP(*(.note.gnu.build-id))
        __ps_build_id_end = .;
    }
}
INSERT AFTER .rodata;
