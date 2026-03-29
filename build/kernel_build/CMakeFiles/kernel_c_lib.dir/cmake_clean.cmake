file(REMOVE_RECURSE
  "libkernel_c.a"
  "libkernel_c.pdb"
)

# Per-language clean rules from dependency scanning.
foreach(lang C)
  include(CMakeFiles/kernel_c_lib.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
