.file "crt0.s"
.syntax unified
.arm
.section .text.crt0,"ax"
.global plugin_start
plugin_start:
.word 0
.word plugin_mod0-plugin_start
.space 0x78
.section .rodata.mod0,"a"
.global plugin_mod0
plugin_mod0:
.ascii "MOD0"
.word __dynamic_start__-plugin_mod0
.word __bss_start__-plugin_mod0
.word __bss_end__-plugin_mod0
.word 0
.word 0
.word plugin_module_runtime-plugin_mod0
.bss
.balign 4
.hidden plugin_module_runtime
plugin_module_runtime:
.space 0xd0
