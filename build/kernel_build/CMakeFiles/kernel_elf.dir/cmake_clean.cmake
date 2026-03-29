file(REMOVE_RECURSE
  "CMakeFiles/kernel_elf"
  "asm_entry.o"
  "asm_isr_stubs.o"
  "asm_syscall_entry.o"
  "kernel.elf"
)

# Per-language clean rules from dependency scanning.
foreach(lang )
  include(CMakeFiles/kernel_elf.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
