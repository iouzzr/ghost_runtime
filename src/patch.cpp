#include "ghost_runtime.h"
#include <ctype.h>

// ============================================================================
// Patching Utilities
// ============================================================================

void InstallBridge(uint32_t xboxVA, const void* hostFn) {
    uint8_t* p = reinterpret_cast<uint8_t*>(xboxVA);
    uint32_t rel = reinterpret_cast<uintptr_t>(hostFn) - (xboxVA + 5);
    p[0] = 0xE9; // JMP rel32
    *reinterpret_cast<uint32_t*>(p + 1) = rel;
}

void PatchBytes(uint32_t xboxVA, const void* data, size_t len) {
    memcpy(reinterpret_cast<void*>(xboxVA), data, len);
}

void PatchU32(uint32_t xboxVA, uint32_t val) {
    *reinterpret_cast<uint32_t*>(xboxVA) = val;
}

void PatchRet(uint32_t xboxVA, uint16_t stackBytes, uint32_t retVal, bool setEax) {
    uint8_t* p = reinterpret_cast<uint8_t*>(xboxVA);
    size_t idx = 0;
    if (setEax) {
        if (retVal == 0) {
            p[idx++] = 0x33; p[idx++] = 0xC0; // xor eax, eax
        } else {
            p[idx++] = 0xB8; // mov eax, imm32
            *reinterpret_cast<uint32_t*>(p + idx) = retVal;
            idx += 4;
        }
    }
    if (stackBytes == 0) {
        p[idx++] = 0xC3; // ret
    } else {
        p[idx++] = 0xC2; // ret imm16
        *reinterpret_cast<uint16_t*>(p + idx) = stackBytes;
        idx += 2;
    }
}

// ============================================================================
// Path Translation
// ============================================================================

void EnsureDirExists(const char* dirPath) {
    if (!dirPath || !dirPath[0]) return;
    wchar_t tmp[kMaxHostPath];
    if (!Utf8ToWide(dirPath, tmp, kMaxHostPath) || !tmp[0]) return;
    for (wchar_t* p = tmp; *p; ++p) {
        if (*p == L'/') *p = L'\\';
    }
    wchar_t* start = tmp;
    if (tmp[0] && tmp[1] == L':' && tmp[2] == L'\\') {
        start = tmp + 3;
    } else if (tmp[0] == L'\\' && tmp[1] == L'\\') {
        start = tmp + 2;
        while (*start && *start != L'\\') ++start;
        if (*start == L'\\') ++start;
        while (*start && *start != L'\\') ++start;
        if (*start == L'\\') ++start;
    }
    for (wchar_t* p = start; *p; ++p) {
        if (*p == L'\\') {
            *p = L'\0';
            CreateDirectoryW(tmp, nullptr);
            *p = L'\\';
        }
    }
    CreateDirectoryW(tmp, nullptr);
}

static bool StartsWithPathPrefixCI(const char* path, const char* prefix, size_t* outPrefixLen) {
    if (!path || !prefix || !prefix[0]) return false;
    size_t i = 0;
    while (prefix[i] != '\0') {
        char a = (path[i] == '/') ? '\\' : static_cast<char>(tolower(static_cast<unsigned char>(path[i])));
        char b = (prefix[i] == '/') ? '\\' : static_cast<char>(tolower(static_cast<unsigned char>(prefix[i])));
        if (a != b) return false;
        ++i;
    }
    if (path[i] != '\0' && path[i] != '\\' && path[i] != '/') {
        return false;
    }
    if (outPrefixLen) *outPrefixLen = i;
    return true;
}

static void SanitizeRelativeSubPath(const char* rel, char* outClean, size_t outMax) {
    if (!outClean || outMax == 0) return;
    outClean[0] = '\0';
    if (!rel) return;

    // Split on '\' and '/', collapsing '.' and '..' so virtual paths can never escape sandbox roots
    const char* segStarts[64];
    size_t      segLens[64];
    size_t      segCount = 0;

    const char* p = rel;
    while (*p) {
        while (*p == '\\' || *p == '/') ++p;
        if (!*p) break;
        const char* start = p;
        while (*p && *p != '\\' && *p != '/') ++p;
        size_t len = static_cast<size_t>(p - start);
        if (len == 1 && start[0] == '.') {
            continue;
        }
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (segCount > 0) --segCount;
            continue;
        }
        if (segCount < 64) {
            segStarts[segCount] = start;
            segLens[segCount]   = len;
            ++segCount;
        }
    }

    size_t pos = 0;
    for (size_t i = 0; i < segCount; ++i) {
        if (i > 0) {
            if (pos + 1 >= outMax) break;
            outClean[pos++] = '\\';
        }
        size_t copyLen = segLens[i];
        if (pos + copyLen >= outMax) {
            copyLen = outMax - 1 - pos;
        }
        if (copyLen > 0) {
            memcpy(outClean + pos, segStarts[i], copyLen);
            pos += copyLen;
        }
        if (pos + 1 >= outMax) break;
    }
    outClean[pos] = '\0';
}

void TranslateXboxPath(const char* xboxPath, char* outWinPath, size_t outMaxLen) {
    if (!outWinPath || outMaxLen == 0) return;
    if (!xboxPath || !xboxPath[0]) {
        outWinPath[0] = '\0';
        return;
    }

    // If caller passed an already-translated host path under g_gameRootDir or g_appRootDir,
    // sanitize its relative suffix so embedded ".." segments can never escape the sandboxed root.
    size_t hostPrefixLen = 0;
    const char* matchedHostRoot = nullptr;
    if (StartsWithPathPrefixCI(xboxPath, g_gameRootDir, &hostPrefixLen)) {
        matchedHostRoot = g_gameRootDir;
    } else if (StartsWithPathPrefixCI(xboxPath, g_appRootDir, &hostPrefixLen)) {
        matchedHostRoot = g_appRootDir;
    }
    if (matchedHostRoot) {
        char cleanRel[kMaxHostPath];
        SanitizeRelativeSubPath(xboxPath + hostPrefixLen, cleanRel, sizeof(cleanRel));
        size_t rootLen = strlen(matchedHostRoot);
        bool rootHasTrailSlash = (rootLen > 0 && (matchedHostRoot[rootLen - 1] == '\\' || matchedHostRoot[rootLen - 1] == '/'));
        if (cleanRel[0]) {
            snprintf(outWinPath, outMaxLen, rootHasTrailSlash ? "%s%s" : "%s\\%s", matchedHostRoot, cleanRel);
        } else {
            snprintf(outWinPath, outMaxLen, "%s", matchedHostRoot);
        }
        for (char* p = outWinPath; *p; ++p) {
            if (*p == '/') *p = '\\';
        }
        return;
    }

    const char* rel = xboxPath;
    char subFolderBuf[32] = { 0 };
    const char* subFolder = nullptr;

    // Strip Win32/NT object namespace prefixes ("\??\" or "\\?\")
    if (strncmp(rel, "\\??\\", 4) == 0 || strncmp(rel, "\\\\?\\", 4) == 0) {
        rel += 4;
    }

    size_t matchedLen = 0;
    if (StartsWithPathPrefixCI(rel, "\\Device\\CdRom0", &matchedLen)) {
        rel += matchedLen;
        subFolder = nullptr; // Game root (D:)
    } else if (StartsWithPathPrefixCI(rel, "\\Device\\Harddisk0\\Partition0", &matchedLen)) {
        rel += matchedLen;
        subFolder = "cache";
    } else if (StartsWithPathPrefixCI(rel, "\\Device\\Harddisk0\\Partition1", &matchedLen)) {
        rel += matchedLen;
        while (*rel == '\\' || *rel == '/') ++rel;
        if (_strnicmp(rel, "UDATA\\", 6) == 0 || _strnicmp(rel, "UDATA/", 6) == 0) {
            rel += 6;
            // Skip optional 8-hex-digit TitleID folder if followed by a separator
            const char* slash = strpbrk(rel, "\\/");
            if (slash && (slash - rel) == 8) rel = slash + 1;
            subFolder = "saves";
        } else if (_strnicmp(rel, "TDATA\\", 6) == 0 || _strnicmp(rel, "TDATA/", 6) == 0) {
            rel += 6;
            const char* slash = strpbrk(rel, "\\/");
            if (slash && (slash - rel) == 8) rel = slash + 1;
            subFolder = "tdata";
        } else {
            subFolder = "hdd_e";
        }
    } else if (StartsWithPathPrefixCI(rel, "\\Device\\Harddisk0\\Partition2", &matchedLen)) {
        rel += matchedLen;
        subFolder = "hdd_c";
    } else if (StartsWithPathPrefixCI(rel, "\\Device\\Harddisk0\\Partition3", &matchedLen)) {
        rel += matchedLen;
        subFolder = "cache_x";
    } else if (StartsWithPathPrefixCI(rel, "\\Device\\Harddisk0\\Partition4", &matchedLen)) {
        rel += matchedLen;
        subFolder = "cache_y";
    } else if (StartsWithPathPrefixCI(rel, "\\Device\\Harddisk0\\Partition5", &matchedLen)) {
        rel += matchedLen;
        subFolder = "cache";
    } else if (isalpha(static_cast<unsigned char>(rel[0])) && rel[1] == ':') {
        char drive = static_cast<char>(tolower(static_cast<unsigned char>(rel[0])));
        rel += 2;
        switch (drive) {
            case 'd': subFolder = nullptr;   break;
            case 'u': subFolder = "saves";   break;
            case 't': subFolder = "tdata";   break;
            case 'z': subFolder = "cache";   break;
            case 'x': subFolder = "cache_x"; break;
            case 'y': subFolder = "cache_y"; break;
            case 'e': subFolder = "hdd_e";   break;
            case 'c': subFolder = "hdd_c";   break;
            default:
                // Sandbox any MU / extra Xbox drive letters (F:, G:, H:, ...) inside GhostXR_Data\drives_<letter>
                // so virtual Xbox paths can NEVER escape onto the host Windows C:/E:/F: filesystems.
                snprintf(subFolderBuf, sizeof(subFolderBuf), "drive_%c", drive);
                subFolder = subFolderBuf;
                break;
        }
    }

    char cleanRel[kMaxHostPath];
    SanitizeRelativeSubPath(rel, cleanRel, sizeof(cleanRel));

    // Redirect engine .reslog files into cache\ so the original game directory remains 100% untouched
    if (!subFolder && cleanRel[0]) {
        const char* dot = strrchr(cleanRel, '.');
        if (dot && _stricmp(dot, ".reslog") == 0) {
            subFolder = "cache";
        }
    }

    if (subFolder) {
        char baseDir[kMaxHostPath];
        const char* saveRoot = g_appRootDir[0] ? g_appRootDir : g_gameRootDir;
        snprintf(baseDir, sizeof(baseDir), "%s\\%s", saveRoot, subFolder);
        EnsureDirExists(baseDir);
        if (cleanRel[0]) {
            snprintf(outWinPath, outMaxLen, "%s\\%s", baseDir, cleanRel);
        } else {
            snprintf(outWinPath, outMaxLen, "%s", baseDir);
        }
    } else {
        if (cleanRel[0]) {
            snprintf(outWinPath, outMaxLen, "%s\\%s", g_gameRootDir, cleanRel);
        } else {
            snprintf(outWinPath, outMaxLen, "%s", g_gameRootDir);
        }
    }

    for (char* p = outWinPath; *p; ++p) {
        if (*p == '/') *p = '\\';
    }

    // Fallback for 'Strings\game.nls' -> 'Misc\game.nls' if 'Strings\game.nls' does not exist
    if (strstr(outWinPath, "\\Strings\\game.nls") != nullptr && GetFileAttributesUtf8(outWinPath) == INVALID_FILE_ATTRIBUTES) {
        snprintf(outWinPath, outMaxLen, "%s\\Misc\\game.nls", g_gameRootDir);
    }
}
