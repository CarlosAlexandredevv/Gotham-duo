extern fp_GetFileVersionInfoA:qword
extern fp_GetFileVersionInfoByHandle:qword
extern fp_GetFileVersionInfoExA:qword
extern fp_GetFileVersionInfoExW:qword
extern fp_GetFileVersionInfoSizeA:qword
extern fp_GetFileVersionInfoSizeExA:qword
extern fp_GetFileVersionInfoSizeExW:qword
extern fp_GetFileVersionInfoSizeW:qword
extern fp_GetFileVersionInfoW:qword
extern fp_VerFindFileA:qword
extern fp_VerFindFileW:qword
extern fp_VerInstallFileA:qword
extern fp_VerInstallFileW:qword
extern fp_VerLanguageNameA:qword
extern fp_VerLanguageNameW:qword
extern fp_VerQueryValueA:qword
extern fp_VerQueryValueW:qword

.code
Proxy_GetFileVersionInfoA proc
    jmp fp_GetFileVersionInfoA
Proxy_GetFileVersionInfoA endp

Proxy_GetFileVersionInfoByHandle proc
    jmp fp_GetFileVersionInfoByHandle
Proxy_GetFileVersionInfoByHandle endp

Proxy_GetFileVersionInfoExA proc
    jmp fp_GetFileVersionInfoExA
Proxy_GetFileVersionInfoExA endp

Proxy_GetFileVersionInfoExW proc
    jmp fp_GetFileVersionInfoExW
Proxy_GetFileVersionInfoExW endp

Proxy_GetFileVersionInfoSizeA proc
    jmp fp_GetFileVersionInfoSizeA
Proxy_GetFileVersionInfoSizeA endp

Proxy_GetFileVersionInfoSizeExA proc
    jmp fp_GetFileVersionInfoSizeExA
Proxy_GetFileVersionInfoSizeExA endp

Proxy_GetFileVersionInfoSizeExW proc
    jmp fp_GetFileVersionInfoSizeExW
Proxy_GetFileVersionInfoSizeExW endp

Proxy_GetFileVersionInfoSizeW proc
    jmp fp_GetFileVersionInfoSizeW
Proxy_GetFileVersionInfoSizeW endp

Proxy_GetFileVersionInfoW proc
    jmp fp_GetFileVersionInfoW
Proxy_GetFileVersionInfoW endp

Proxy_VerFindFileA proc
    jmp fp_VerFindFileA
Proxy_VerFindFileA endp

Proxy_VerFindFileW proc
    jmp fp_VerFindFileW
Proxy_VerFindFileW endp

Proxy_VerInstallFileA proc
    jmp fp_VerInstallFileA
Proxy_VerInstallFileA endp

Proxy_VerInstallFileW proc
    jmp fp_VerInstallFileW
Proxy_VerInstallFileW endp

Proxy_VerLanguageNameA proc
    jmp fp_VerLanguageNameA
Proxy_VerLanguageNameA endp

Proxy_VerLanguageNameW proc
    jmp fp_VerLanguageNameW
Proxy_VerLanguageNameW endp

Proxy_VerQueryValueA proc
    jmp fp_VerQueryValueA
Proxy_VerQueryValueA endp

Proxy_VerQueryValueW proc
    jmp fp_VerQueryValueW
Proxy_VerQueryValueW endp

end
