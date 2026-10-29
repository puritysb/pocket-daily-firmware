SECTIONS {
 .text 0x50001700 : { KEEP(*(.text.entry)) *(.text .text.*) }
 /DISCARD/ : { *(.comment) *(.eh_frame*) }
}
ASSERT(entry == 0x50001700, "RTC entry must be the published first byte")
ASSERT(SIZEOF(.text) <= 0x380, "RTC code overlaps fixed GPIO trace")
