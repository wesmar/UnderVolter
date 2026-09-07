// IniHelpers.c — Implementation of shared INI streaming-scan helpers.
//                See IniHelpers.h for contract documentation.
#include "IniHelpers.h"

BOOLEAN IniMatchCI(CONST CHAR8* s, CONST CHAR8* lit) {
    while (*lit) {
        CHAR8 a = *s, b = *lit;
        if (a >= 'a' && a <= 'z') a = (CHAR8)(a - 32);
        if (b >= 'a' && b <= 'z') b = (CHAR8)(b - 32);
        if (a != b) return FALSE;
        s++; lit++;
    }
    return TRUE;
}

UINTN IniStrLen8(CONST CHAR8* s) {
    UINTN n = 0; while (*s++) n++; return n;
}

BOOLEAN IniReadBool(CONST CHAR8* p, BOOLEAN Default) {
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '=') return Default;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '0' && *p != '1') return Default;
    BOOLEAN value = (*p++ == '1');
    while (*p == ' ' || *p == '\t') p++;
    return (!*p || *p == '\r' || *p == '\n' || *p == ';' || *p == '#') ? value : Default;
}

UINT32 IniReadUint(CONST CHAR8* p, UINT32 Default) {
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '=') return Default;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p < '0' || *p > '9') return Default;
    UINT32 v = 0;
    while (*p >= '0' && *p <= '9') {
        UINT32 digit = (UINT32)(*p++ - '0');
        if (v > (MAX_UINT32 - digit) / 10) return Default;
        v = v * 10 + digit;
    }
    while (*p == ' ' || *p == '\t') p++;
    if (*p && *p != '\r' && *p != '\n' && *p != ';' && *p != '#') return Default;
    return v;
}

VOID IniReadPath(CONST CHAR8* p, CHAR16* Out, UINTN OutLen) {
    if (OutLen == 0) return;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '=') return;     // caller pre-fills default; leave as-is
    p++;
    while (*p == ' ' || *p == '\t') p++;
    CHAR8 quote = (*p == '"' || *p == '\'') ? *p++ : 0;
    UINTN i = 0;
    while (*p && *p != ';' && *p != '#' && (!quote || *p != quote) && *p != '\r' && *p != '\n' && i < OutLen - 1) {
        CHAR8 c = *p++;
        Out[i++] = (c == '/') ? L'\\' : (CHAR16)c;
    }
    while (i > 0 && (Out[i - 1] == L' ' || Out[i - 1] == L'\t')) i--;
    Out[i] = L'\0';
}

BOOLEAN IniSectionMatch(CONST CHAR8* p, CONST CHAR8* SectionName) {
    while (*p == ' ' || *p == '\t') p++;
    if (!IniMatchCI(p, SectionName)) return FALSE;
    p += IniStrLen8(SectionName);
    while (*p == ' ' || *p == '\t') p++;
    return (*p == ']');
}
