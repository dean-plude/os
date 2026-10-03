/* psapi.dll — process status: kernel32 has these as K32* (as on
 * Windows 7 and later), psapi forwards to them under the old names */
__asm__(".section .drectve,\"yn\"\n\t"
        ".ascii \" /EXPORT:EnumProcesses=kernel32.K32EnumProcesses\"\n\t"
        ".ascii \" /EXPORT:EnumProcessModules=kernel32.K32EnumProcessModules\"\n\t"
        ".ascii \" /EXPORT:EnumProcessModulesEx=kernel32.K32EnumProcessModulesEx\"\n\t"
        ".ascii \" /EXPORT:GetModuleBaseNameA=kernel32.K32GetModuleBaseNameA\"\n\t"
        ".ascii \" /EXPORT:GetModuleBaseNameW=kernel32.K32GetModuleBaseNameW\"\n\t"
        ".ascii \" /EXPORT:GetModuleFileNameExA=kernel32.K32GetModuleFileNameExA\"\n\t"
        ".ascii \" /EXPORT:GetModuleFileNameExW=kernel32.K32GetModuleFileNameExW\"\n\t"
        ".ascii \" /EXPORT:GetModuleInformation=kernel32.K32GetModuleInformation\"\n\t"
        ".ascii \" /EXPORT:GetProcessMemoryInfo=kernel32.K32GetProcessMemoryInfo\"\n\t"
        ".ascii \" /EXPORT:GetProcessImageFileNameW=kernel32.K32GetProcessImageFileNameW\"\n\t"
        ".ascii \" /EXPORT:GetProcessImageFileNameA=kernel32.K32GetProcessImageFileNameA\"\n\t"
        ".ascii \" /EXPORT:GetMappedFileNameW=kernel32.K32GetMappedFileNameW\"\n\t"
        ".ascii \" /EXPORT:EmptyWorkingSet=kernel32.K32EmptyWorkingSet\"\n\t"
        ".ascii \" /EXPORT:InitializeProcessForWsWatch=kernel32.K32InitializeProcessForWsWatch\"\n\t"
        ".ascii \" /EXPORT:GetPerformanceInfo=kernel32.K32GetPerformanceInfo\"\n\t"
        ".text\n");
