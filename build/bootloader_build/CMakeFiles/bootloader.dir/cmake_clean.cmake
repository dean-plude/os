file(REMOVE_RECURSE
  "CMakeFiles/bootloader"
  "bootx64.efi"
  "console.obj"
  "elf_loader.obj"
  "main.obj"
  "paging.obj"
)

# Per-language clean rules from dependency scanning.
foreach(lang )
  include(CMakeFiles/bootloader.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
