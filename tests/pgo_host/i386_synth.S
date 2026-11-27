/* The profile sections of the Vita objects as files (cnts.bin, data.bin, names.bin from the objcopy step of run_synth.sh), back in
 * sections of the names the compiler uses; counters first, the records right behind them, as in the Vita ELF. */
	.section __llvm_prf_cnts,"aw",@progbits
	.balign 8
	.incbin "cnts.bin"
	.section __llvm_prf_data,"aw",@progbits
	.balign 8
	.incbin "data.bin"
	.section __llvm_prf_names,"a",@progbits
	.incbin "names.bin"
	.section .note.GNU-stack,"",@progbits
/* what every instrumented unit defines (hidden, in its own comdat): raw format 9 with the IR-instrumentation flag; the Vita build must see
 * this one and not a runtime default, so the runtime does not define the variable (vita/vgx-game/CMakeLists.txt) */
	.section .rodata.__llvm_profile_raw_version,"a",@progbits
	.globl __llvm_profile_raw_version
	.hidden __llvm_profile_raw_version
	.balign 8
__llvm_profile_raw_version:
	.quad 0x0100000000000009
