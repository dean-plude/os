# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/mnt/c/Users/dean.plude/Documents/GitHub/os/kernel"
  "/mnt/c/Users/dean.plude/Documents/GitHub/os/build/kernel_build"
  "/mnt/c/Users/dean.plude/Documents/GitHub/os/build/kernel_build-prefix"
  "/mnt/c/Users/dean.plude/Documents/GitHub/os/build/kernel_build-prefix/tmp"
  "/mnt/c/Users/dean.plude/Documents/GitHub/os/build/kernel_build-prefix/src/kernel_build-stamp"
  "/mnt/c/Users/dean.plude/Documents/GitHub/os/build/kernel_build-prefix/src"
  "/mnt/c/Users/dean.plude/Documents/GitHub/os/build/kernel_build-prefix/src/kernel_build-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/mnt/c/Users/dean.plude/Documents/GitHub/os/build/kernel_build-prefix/src/kernel_build-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/mnt/c/Users/dean.plude/Documents/GitHub/os/build/kernel_build-prefix/src/kernel_build-stamp${cfgdir}") # cfgdir has leading slash
endif()
