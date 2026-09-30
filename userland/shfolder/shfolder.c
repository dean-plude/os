/* shfolder.dll — the old home of SHGetFolderPath (installers such as NSIS
 * look for it there); forwarded to shell32 */
__asm__(".section .drectve,\"yn\"\n\t"
        ".ascii \" /EXPORT:SHGetFolderPathA=shell32.SHGetFolderPathA\"\n\t"
        ".ascii \" /EXPORT:SHGetFolderPathW=shell32.SHGetFolderPathW\"\n\t"
        ".text\n");
