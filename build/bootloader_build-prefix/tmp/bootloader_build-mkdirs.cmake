# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/user/os/bootloader"
  "/home/user/os/build/bootloader_build"
  "/home/user/os/build/bootloader_build-prefix"
  "/home/user/os/build/bootloader_build-prefix/tmp"
  "/home/user/os/build/bootloader_build-prefix/src/bootloader_build-stamp"
  "/home/user/os/build/bootloader_build-prefix/src"
  "/home/user/os/build/bootloader_build-prefix/src/bootloader_build-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/user/os/build/bootloader_build-prefix/src/bootloader_build-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/user/os/build/bootloader_build-prefix/src/bootloader_build-stamp${cfgdir}") # cfgdir has leading slash
endif()
