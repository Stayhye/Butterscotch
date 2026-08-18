#include "ps2_file_system.h"
#include "ps2_utils.h"
#include "../json_reader.h"
#include "../utils.h"

#include "stdio_compat.h"
#include <stdlib.h>
#include "string_compat.h" 
#include <sys/stat.h>

#include "stb_ds.h"

// ===[ Internal Types ]===

typedef struct {
    char* key; // game-relative file name
    char** value; // stb_ds dynamic array of resolved device paths
} Ps2FileMapping;

// Parsed save icon configuration from CONFIG.JSN "saveIcon" section
typedef struct {
    uint32_t bgAlpha;
    int32_t bgColors[4][4]; // 4 corners x RGBA
    float lightDirs[3][4]; // 3 lights x XYZW
    float lightColors[3][4]; // 3 lights x RGBA
    float ambient[4]; // RGBA
} SaveIconConfig;

typedef struct {
    FileSystem base;
    Ps2FileMapping* mappings; // stb_ds string hashmap
    char* gameTitle; // game display name for icon.sys generation
    SaveIconConfig saveIconConfig;
} Ps2FileSystem;

// ===[ Helpers ]===

// Expands $BOOT: prefix to the boot device path, or returns a strdup of the input
static char* expandBootPrefix(const char* path) {
    const char* bootPrefix = "$BOOT:";
    size_t bootPrefixLen = strlen(bootPrefix);

    if (strncmp(path, bootPrefix, bootPrefixLen) == 0) {
        const char* relativePart = path + bootPrefixLen;
        return PS2Utils_createDevicePath(relativePart);
    }

    return safeStrdup(path);
}

// ===[ icon.sys Generation ]===
#define ICON_SYS_SIZE 964

static void asciiToShiftJIS(char c, uint8_t* out) {
    if (c == ' ') {
        out[0] = 0x81; out[1] = 0x40;
    } else if (c >= '0' && '9' >= c) {
        out[0] = 0x82; out[1] = 0x4F + (c - '0');
    } else if (c >= 'A' && 'Z' >= c) {
        out[0] = 0x82; out[1] = 0x60 + (c - 'A');
    } else if (c >= 'a' && 'z' >= c) {
        out[0] = 0x82; out[1] = 0x81 + (c - 'a');
    } else if (c == '!') {
        out[0] = 0x81; out[1] = 0x49;
    } else if (c == '?') {
        out[0] = 0x81; out[1] = 0x48;
    } else if (c == '.') {
        out[0] = 0x81; out[1] = 0x44;
    } else if (c == ',') {
        out[0] = 0x81; out[1] = 0x43;
    } else if (c == ':') {
        out[0] = 0x81; out[1] = 0x46;
    } else if (c == '-') {
        out[0] = 0x81; out[1] = 0x7C;
    } else if (c == '(') {
        out[0] = 0x81; out[1] = 0x69;
    } else if (c == ')') {
        out[0] = 0x81; out[1] = 0x6A;
    } else {
        out[0] = 0x81; out[1] = 0x40;
    }
}

static void writeShiftJISTitle(uint8_t* titleField, const char* gameTitle) {
    size_t srcLen = strlen(gameTitle);
    size_t dstPos = 0;
    size_t maxBytes = 66;

    repeat(srcLen, i) {
        if (dstPos + 2 > maxBytes)
            break;
        asciiToShiftJIS(gameTitle[i], titleField + dstPos);
        dstPos += 2;
    }
}

static void generateIconSys(uint8_t* buffer, const char* gameTitle, const SaveIconConfig* config) {
    memset(buffer, 0, ICON_SYS_SIZE);
    memcpy(buffer + 0x000, "PS2D", 4);
    memcpy(buffer + 0x00C, &config->bgAlpha, 4);
    memcpy(buffer + 0x010, config->bgColors, sizeof(config->bgColors));
    memcpy(buffer + 0x050, config->lightDirs, sizeof(config->lightDirs));
    memcpy(buffer + 0x080, config->lightColors, sizeof(config->lightColors));
    memcpy(buffer + 0x0B0, config->ambient, sizeof(config->ambient));
    writeShiftJISTitle(buffer + 0x0C0, gameTitle);

    const char* iconFileName = "ASSETS/ICON.ICO";
    size_t iconNameLen = strlen(iconFileName);
    memcpy(buffer + 0x104, iconFileName, iconNameLen);
    memcpy(buffer + 0x144, iconFileName, iconNameLen);
    memcpy(buffer + 0x184, iconFileName, iconNameLen);
}

static bool copyFile(const char* srcPath, const char* dstPath) {
    FILE* src = fopen(srcPath, "rb");
    if (src == nullptr)
        return false;

    fseek(src, 0, SEEK_END);
    long size = ftell(src);
    fseek(src, 0, SEEK_SET);

    uint8_t* data = (uint8_t *)safeMalloc((size_t) size);
    size_t bytesRead = fread(data, 1, (size_t) size, src);
    fclose(src);

    FILE* dst = fopen(dstPath, "wb");
    if (dst == nullptr) {
        free(data);
        return false;
    }

    size_t written = fwrite(data, 1, bytesRead, dst);
    fclose(dst);
    free(data);
    return written == bytesRead;
}

static void copyIconIcoIfMissing(const char* dirPath) {
    size_t dirLen = strlen(dirPath);
    size_t pathLen = dirLen + 1 + 8 + 1;
    char* dstPath = (char *)safeMalloc(pathLen);
    snprintf(dstPath, pathLen, "%s/ICON.ICO", dirPath);

    FILE* check = fopen(dstPath, "rb");
    if (check != nullptr) {
        fclose(check);
        free(dstPath);
        return;
    }

    char* srcPath = PS2Utils_createDevicePath("ASSETS/ICON.ICO");
    if (copyFile(srcPath, dstPath)) {
        logInfo("Ps2FileSystem: Copied ICON.ICO to %s\n", dirPath);
    } else {
        logWarn("Ps2FileSystem: Failed to copy ICON.ICO from %s to %s\n", srcPath, dstPath);
    }

    free(srcPath);
    free(dstPath);
}

static void writeIconSysIfMissing(const char* dirPath, const char* gameTitle, const SaveIconConfig* config) {
    size_t dirLen = strlen(dirPath);
    size_t pathLen = dirLen + 1 + 8 + 1;
    char* iconSysPath = (char *)safeMalloc(pathLen);
    snprintf(iconSysPath, pathLen, "%s/icon.sys", dirPath);

    FILE* check = fopen(iconSysPath, "rb");
    if (check != nullptr) {
        fclose(check);
        free(iconSysPath);
        return;
    }

    uint8_t buffer[ICON_SYS_SIZE];
    generateIconSys(buffer, gameTitle, config);

    FILE* f = fopen(iconSysPath, "wb");
    if (f != nullptr) {
        fwrite(buffer, 1, ICON_SYS_SIZE, f);
        fclose(f);
        logInfo("Ps2FileSystem: Created icon.sys in %s\n", dirPath);
    } else {
        logWarn("Ps2FileSystem: Failed to create icon.sys in %s\n", dirPath);
    }

    free(iconSysPath);
}

static void ensureParentDirectory(Ps2FileSystem* pfs, const char* path) {
    if (strncmp(path, "cdrom0:", 7) == 0 || strncmp(path, "mass:", 5) == 0)
        return;

    char* pathCopy = safeStrdup(path);
    char* lastSlash = strrchr(pathCopy, '/');
    if (lastSlash != nullptr && lastSlash != pathCopy) {
        *lastSlash = '\0';
        mkdir(pathCopy, 0777);
        writeIconSysIfMissing(pathCopy, pfs->gameTitle, &pfs->saveIconConfig);
        copyIconIcoIfMissing(pathCopy);
    }
    free(pathCopy);
}

// ===[ Directory Searching & Selection Helper ]===

// Scans potential paths/folders to find valid ones containing CONFIG.JSN
typedef struct {
    char** folders; // dynamic array of discovered folder paths containing CONFIG.JSN
} DiscoveredFolders;

static DiscoveredFolders scanForValidConfigFolders(const char** candidateRoots, int count) {
    DiscoveredFolders result = {0};
    result.folders = NULL;

    repeat(count, i) {
        const char* root = candidateRoots[i];
        size_t pathLen = strlen(root) + 12 + 1; // e.g., "mc0:/path/CONFIG.JSN"
        char* fullPath = (char*)safeMalloc(pathLen);
        snprintf(fullPath, pathLen, "%s/CONFIG.JSN", root);

        FILE* f = fopen(fullPath, "rb");
        if (f != nullptr) {
            fclose(f);
            arrput(result.folders, safeStrdup(root));
            logInfo("Found valid CONFIG.JSN in folder: %s\n", root);
        }
        free(fullPath);
    }

    return result;
}

// ===[ Vtable Implementations ]===

static char* resolvePath(FileSystem* fs, const char* relativePath) {
    Ps2FileSystem* pfs = (Ps2FileSystem*) fs;
    ptrdiff_t idx = shgeti(pfs->mappings, relativePath);
    if (0 > idx)
        return nullptr;

    if (arrlen(pfs->mappings[idx].value) > 0)
        return safeStrdup(pfs->mappings[idx].value[0]);

    return nullptr;
}

static bool fileExists(FileSystem* fs, const char* relativePath) {
    Ps2FileSystem* pfs = (Ps2FileSystem*) fs;
    ptrdiff_t idx = shgeti(pfs->mappings, relativePath);
    if (0 > idx)
        return false;

    char** paths = pfs->mappings[idx].value;
    int pathCount = arrlen(paths);
    repeat(pathCount, i) {
        FILE* f = fopen(paths[i], "rb");
        if (f != nullptr) {
            fclose(f);
            return true;
        }
    }

    return false;
}

static char* readFileText(FileSystem* fs, const char* relativePath) {
    Ps2FileSystem* pfs = (Ps2FileSystem*) fs;
    ptrdiff_t idx = shgeti(pfs->mappings, relativePath);
    if (0 > idx)
        return nullptr;

    char** paths = pfs->mappings[idx].value;
    int pathCount = arrlen(paths);
    repeat(pathCount, i) {
        FILE* f = fopen(paths[i], "rb");
        if (f == nullptr)
            continue;

        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);

        char* content = (char *)safeMalloc((size_t) size + 1);
        size_t bytesRead = fread(content, 1, (size_t) size, f);
        content[bytesRead] = '\0';
        fclose(f);
        return content;
    }

    return nullptr;
}

static bool writeFileText(FileSystem* fs, const char* relativePath, const char* contents) {
    Ps2FileSystem* pfs = (Ps2FileSystem*) fs;
    ptrdiff_t idx = shgeti(pfs->mappings, relativePath);
    if (0 > idx)
        return false;

    char** paths = pfs->mappings[idx].value;
    if (arrlen(paths) == 0)
        return false;

    const char* writePath = paths[0];
    ensureParentDirectory(pfs, writePath);

    FILE* f = fopen(writePath, "wb");
    if (f == nullptr)
        return false;

    size_t len = strlen(contents);
    size_t written = fwrite(contents, 1, len, f);
    fclose(f);
    return written == len;
}

static bool deleteFile(FileSystem* fs, const char* relativePath) {
    Ps2FileSystem* pfs = (Ps2FileSystem*) fs;
    ptrdiff_t idx = shgeti(pfs->mappings, relativePath);
    if (0 > idx)
        return false;

    char** paths = pfs->mappings[idx].value;
    if (arrlen(paths) == 0)
        return false;

    return remove(paths[0]) == 0;
}

static bool ps2ReadFileBinary(FileSystem* fs, const char* relativePath, uint8_t** outData, int32_t* outSize) {
    Ps2FileSystem* pfs = (Ps2FileSystem*) fs;
    ptrdiff_t idx = shgeti(pfs->mappings, relativePath);
    if (0 > idx)
        return false;

    char** paths = pfs->mappings[idx].value;
    int pathCount = arrlen(paths);
    repeat(pathCount, i) {
        FILE* f = fopen(paths[i], "rb");
        if (f == nullptr)
            continue;

        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);

        uint8_t* data = (uint8_t *)safeMalloc((size_t) size);
        size_t bytesRead = fread(data, 1, (size_t) size, f);
        fclose(f);

        *outData = data;
        *outSize = (int32_t) bytesRead;
        return true;
    }

    return false;
}

static bool ps2WriteFileBinary(FileSystem* fs, const char* relativePath, const uint8_t* data, int32_t size) {
    Ps2FileSystem* pfs = (Ps2FileSystem*) fs;
    ptrdiff_t idx = shgeti(pfs->mappings, relativePath);
    if (0 > idx)
        return false;

    char** paths = pfs->mappings[idx].value;
    if (arrlen(paths) == 0)
        return false;

    const char* writePath = paths[0];
    ensureParentDirectory(pfs, writePath);

    FILE* f = fopen(writePath, "wb");
    if (f == nullptr)
        return false;

    size_t written = fwrite(data, 1, (size_t) size, f);
    fclose(f);
    return written == (size_t) size;
}

// ===[ Streaming Binary I/O Stubs ]===
typedef struct {
    FILE* fp;
    char* resolvedPath;
} Ps2BinaryHandle;

static Ps2BinaryHandle* ps2BinaryHandleNew(FILE* fp, const char* resolvedPath) {
    Ps2BinaryHandle* h = (Ps2BinaryHandle *)safeMalloc(sizeof(Ps2BinaryHandle));
    h->fp = fp;
    h->resolvedPath = safeStrdup(resolvedPath);
    return h;
}

static void* ps2BinaryOpen(FileSystem* fs, const char* relativePath, int32_t mode) {
    Ps2FileSystem* pfs = (Ps2FileSystem*) fs;
    ptrdiff_t idx = shgeti(pfs->mappings, relativePath);
    if (0 > idx) return nullptr;
    char** paths = pfs->mappings[idx].value;
    int pathCount = arrlen(paths);
    if (pathCount == 0) return nullptr;

    if (mode == GML_FILE_BIN_READ) {
        repeat(pathCount, i) {
            FILE* f = fopen(paths[i], "rb");
            if (f != nullptr) return ps2BinaryHandleNew(f, paths[i]);
        }
        return nullptr;
    }

    const char* writePath = paths[0];
    if (mode == GML_FILE_BIN_WRITE) {
        ensureParentDirectory(pfs, writePath);
        FILE* f = fopen(writePath, "wb");
        return f != nullptr ? ps2BinaryHandleNew(f, writePath) : nullptr;
    }

    FILE* f = fopen(writePath, "r+b");
    if (f != nullptr) return ps2BinaryHandleNew(f, writePath);
    ensureParentDirectory(pfs, writePath);
    f = fopen(writePath, "w+b");
    return f != nullptr ? ps2BinaryHandleNew(f, writePath) : nullptr;
}

static void ps2BinaryClose(MAYBE_UNUSED FileSystem* fs, void* handle) {
    if (handle == nullptr) return;
    Ps2BinaryHandle* h = (Ps2BinaryHandle*) handle;
    if (h->fp != nullptr) fclose(h->fp);
    free(h->resolvedPath);
    free(h);
}

static int32_t ps2BinaryRead(MAYBE_UNUSED FileSystem* fs, void* handle, void* dst, int32_t n) {
    if (handle == nullptr || 0 >= n) return 0;
    return (int32_t) fread(dst, 1, (size_t) n, ((Ps2BinaryHandle*) handle)->fp);
}

static int32_t ps2BinaryWrite(MAYBE_UNUSED FileSystem* fs, void* handle, const void* src, int32_t n) {
    if (handle == nullptr || 0 >= n) return 0;
    return (int32_t) fwrite(src, 1, (size_t) n, ((Ps2BinaryHandle*) handle)->fp);
}

static int32_t ps2BinaryTell(MAYBE_UNUSED FileSystem* fs, void* handle) {
    if (handle == nullptr) return 0;
    return (int32_t) ftell(((Ps2BinaryHandle*) handle)->fp);
}

static bool ps2BinarySeek(MAYBE_UNUSED FileSystem* fs, void* handle, int32_t pos) {
    if (handle == nullptr) return false;
    return fseek(((Ps2BinaryHandle*) handle)->fp, pos, SEEK_SET) == 0;
}

static int32_t ps2BinarySize(MAYBE_UNUSED FileSystem* fs, void* handle) {
    if (handle == nullptr) return 0;
    FILE* f = ((Ps2BinaryHandle*) handle)->fp;
    long saved = ftell(f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, saved, SEEK_SET);
    return (int32_t) size;
}

static void ps2BinaryRewrite(MAYBE_UNUSED FileSystem* fs, void* handle) {
    if (handle == nullptr) return;
    Ps2BinaryHandle* h = (Ps2BinaryHandle*) handle;
    if (h->fp != nullptr) fclose(h->fp);
    h->fp = fopen(h->resolvedPath, "wb+");
}

static bool ps2DirectoryExists(FileSystem* fs, const char* relativePath) { (void)fs; (void)relativePath; return true; }
static bool ps2CreateDirectory(FileSystem* fs, const char* relativePath) { (void)fs; (void)relativePath; return true; }
static bool ps2DeleteDirectory(FileSystem* fs, const char* relativePath) { (void)fs; (void)relativePath; return true; }
static FileSystemDirEntry* ps2ListDirectory(FileSystem* fs, const char* relativeDirPath) { (void)fs; (void)relativeDirPath; return nullptr; }

// ===[ Vtable ]===
static FileSystemVtable ps2FileSystemVtable;

// ===[ Lifecycle ]===

static SaveIconConfig parseSaveIconConfig(JsonValue* configRoot) {
    JsonValue* saveIconObj = JsonReader_getJsonValueByKey(configRoot, "saveIcon");
    requireNotNullMessage(saveIconObj, "CONFIG.JSN is missing the 'saveIcon' section");
    require(JsonReader_isObject(saveIconObj));

    SaveIconConfig config = {0};

    JsonValue* bgAlphaVal = JsonReader_getJsonValueByKey(saveIconObj, "bgAlpha");
    requireNotNullMessage(bgAlphaVal, "saveIcon.bgAlpha is missing");
    config.bgAlpha = (uint32_t) JsonReader_getDouble(bgAlphaVal);

    JsonValue* bgColorsArr = JsonReader_getJsonValueByKey(saveIconObj, "bgColors");
    requireNotNullMessage(bgColorsArr, "saveIcon.bgColors is missing");
    require(JsonReader_isArray(bgColorsArr) && JsonReader_arrayLength(bgColorsArr) == 4);
    repeat(4, i) {
        JsonValue* corner = JsonReader_getArrayElement(bgColorsArr, i);
        JsonReader_readInt32Array(corner, config.bgColors[i], 3);
        config.bgColors[i][3] = 0;
    }

    JsonValue* lightDirsArr = JsonReader_getJsonValueByKey(saveIconObj, "lightDirs");
    requireNotNullMessage(lightDirsArr, "saveIcon.lightDirs is missing");
    require(JsonReader_isArray(lightDirsArr) && JsonReader_arrayLength(lightDirsArr) == 3);
    repeat(3, i) {
        JsonValue* dir = JsonReader_getArrayElement(lightDirsArr, i);
        JsonReader_readFloatArray(dir, config.lightDirs[i], 3);
        config.lightDirs[i][3] = 0.0f;
    }

    JsonValue* lightColorsArr = JsonReader_getJsonValueByKey(saveIconObj, "lightColors");
    requireNotNullMessage(lightColorsArr, "saveIcon.lightColors is missing");
    require(JsonReader_isArray(lightColorsArr) && JsonReader_arrayLength(lightColorsArr) == 3);
    repeat(3, i) {
        JsonValue* color = JsonReader_getArrayElement(lightColorsArr, i);
        JsonReader_readFloatArray(color, config.lightColors[i], 3);
        config.lightColors[i][3] = 0.0f;
    }

    JsonValue* ambientArr = JsonReader_getJsonValueByKey(saveIconObj, "ambient");
    requireNotNullMessage(ambientArr, "saveIcon.ambient is missing");
    JsonReader_readFloatArray(ambientArr, config.ambient, 3);
    config.ambient[3] = 0.0f;

    return config;
}

FileSystem* Ps2FileSystem_create(JsonValue* configRoot, const char* gameTitle) {
    JsonValue* fileSystemObj = JsonReader_getJsonValueByKey(configRoot, "fileSystem");
    require(fileSystemObj != nullptr && JsonReader_isObject(fileSystemObj));

    Ps2FileSystem* pfs = (Ps2FileSystem *)safeCalloc(1, sizeof(Ps2FileSystem));
    pfs->base.vtable = &ps2FileSystemVtable;
    ps2FileSystemVtable.resolvePath = resolvePath;
    ps2FileSystemVtable.fileExists = fileExists;
    ps2FileSystemVtable.readFileText = readFileText;
    ps2FileSystemVtable.writeFileText = writeFileText;
    ps2FileSystemVtable.deleteFile = deleteFile;
    ps2FileSystemVtable.readFileBinary = ps2ReadFileBinary;
    ps2FileSystemVtable.writeFileBinary = ps2WriteFileBinary;
    ps2FileSystemVtable.binaryOpen = ps2BinaryOpen;
    ps2FileSystemVtable.binaryClose = ps2BinaryClose;
    ps2FileSystemVtable.binaryRead = ps2BinaryRead;
    ps2FileSystemVtable.binaryWrite = ps2BinaryWrite;
    ps2FileSystemVtable.binaryTell = ps2BinaryTell;
    ps2FileSystemVtable.binarySeek = ps2BinarySeek;
    ps2FileSystemVtable.binarySize = ps2BinarySize;
    ps2FileSystemVtable.binaryRewrite = ps2BinaryRewrite;
    ps2FileSystemVtable.directoryExists = ps2DirectoryExists;
    ps2FileSystemVtable.createDirectory = ps2CreateDirectory;
    ps2FileSystemVtable.deleteDirectory = ps2DeleteDirectory;
    ps2FileSystemVtable.listDirectory = ps2ListDirectory;
    
    pfs->gameTitle = safeStrdup(gameTitle);
    pfs->saveIconConfig = parseSaveIconConfig(configRoot);
    pfs->mappings = nullptr;
    sh_new_strdup(pfs->mappings);

    int entryCount = JsonReader_objectLength(fileSystemObj);
    repeat(entryCount, i) {
        const char* gameFileName = JsonReader_getJsonKeyByIndex(fileSystemObj, i);
        JsonValue* pathArray = JsonReader_getJsonValueByIndex(fileSystemObj, i);

        require(JsonReader_isArray(pathArray));

        char** resolvedPaths = nullptr;
        int pathCount = JsonReader_arrayLength(pathArray);
        repeat(pathCount, j) {
            JsonValue* pathElement = JsonReader_getArrayElement(pathArray, j);
            require(JsonReader_isString(pathElement));

            const char* rawPath = JsonReader_getString(pathElement);
            char* resolved = expandBootPrefix(rawPath);
            arrput(resolvedPaths, resolved);
            logInfo("Ps2FileSystem: '%s' -> '%s'\n", gameFileName, resolved);
        }

        shput(pfs->mappings, gameFileName, resolvedPaths);
    }

    logInfo("Ps2FileSystem: Loaded %d file mappings\n", (int) shlen(pfs->mappings));
    return (FileSystem*) pfs;
}

void Ps2FileSystem_destroy(FileSystem* fs) {
    Ps2FileSystem* pfs = (Ps2FileSystem*) fs;
    free(pfs->gameTitle);
    int mappingCount = shlen(pfs->mappings);
    repeat(mappingCount, i) {
        char** paths = pfs->mappings[i].value;
        int pathCount = arrlen(paths);
        repeat(pathCount, j) {
            free(paths[j]);
        }
        arrfree(paths);
    }
    shfree(pfs->mappings);
    free(pfs);
}