#include "nn-worker-cache.hpp"
#include <cstring>
#include <stdexcept>
#include <cstdio>

#ifdef _WIN32
#include <direct.h>
#define MKDIR_CMD(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR_CMD(p) mkdir(p, 0755)
#endif

NnWorkerWeightCache::NnWorkerWeightCache(const char *cacheDir, uint64_t modelHash, NnUint nNodes, NnUint nodeIndex) {
    this->cacheDir = cacheDir;
    this->modelHash = modelHash;
    this->nNodes = nNodes;
    this->nodeIndex = nodeIndex;
    MKDIR_CMD(cacheDir);
}

std::string NnWorkerWeightCache::getCachePath() const {
    char name[128];
    snprintf(name, sizeof(name), "/dllama_%016llx_%u_%u.bin",
             (unsigned long long)modelHash, nNodes, nodeIndex);
    return cacheDir + name;
}

bool NnWorkerWeightCache::isValid() const {
    std::string path = getCachePath();
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;

    NnUint magic;
    uint64_t hash;
    NnUint nn, ni;
    bool ok = (fread(&magic, 4, 1, f) == 1) &&
              (fread(&hash, 8, 1, f) == 1) &&
              (fread(&nn, 4, 1, f) == 1) &&
              (fread(&ni, 4, 1, f) == 1) &&
              (magic == WORKER_CACHE_MAGIC) &&
              (hash == modelHash) &&
              (nn == nNodes) &&
              (ni == nodeIndex);
    fclose(f);
    if (ok) printf("💾 Cache hit: %s\n", path.c_str());
    return ok;
}

void NnWorkerWeightCache::loadFromDisk(NnExecutor *executor) const {
    std::string path = getCachePath();
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("Cannot open cache file: " + path);

    // Skip already-validated header
    fseek(f, 4 + 8 + 4 + 4, SEEK_SET);

    NnUint nameLen;
    NnUint opIndex;
    NnSize offset, nBytes;

    while (true) {
        if (fread(&nameLen, 4, 1, f) != 1 || nameLen == 0) break;

        char *opName = new char[nameLen];
        if (fread(opName, 1, nameLen, f) != nameLen) {
            delete[] opName;
            break;
        }
        if (fread(&opIndex, 4, 1, f) != 1 ||
            fread(&offset, sizeof(NnSize), 1, f) != 1 ||
            fread(&nBytes, sizeof(NnSize), 1, f) != 1) {
            delete[] opName;
            break;
        }

        NnByte *data = new NnByte[nBytes];
        if (fread(data, 1, nBytes, f) != nBytes) {
            delete[] data;
            delete[] opName;
            break;
        }

        executor->loadWeight(opName, opIndex, offset, nBytes, data);
        printf("💾 Loaded %22s %3d, %12zu kB\n", opName, opIndex, nBytes / 1024);

        delete[] data;
        delete[] opName;
    }
    fclose(f);
    printf("💾 Weights loaded from cache\n");
}

FILE *NnWorkerWeightCache::openForWriting() const {
    std::string path = getCachePath();
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("Cannot create cache file: " + path);

    NnUint magic = WORKER_CACHE_MAGIC;
    fwrite(&magic, 4, 1, f);
    fwrite(&modelHash, 8, 1, f);
    fwrite(&nNodes, 4, 1, f);
    fwrite(&nodeIndex, 4, 1, f);
    return f;
}

void NnWorkerWeightCache::writeEntry(FILE *f, const char *opName, NnUint opIndex, NnSize offset, NnSize nBytes, const NnByte *data) {
    NnUint nameLen = (NnUint)strlen(opName) + 1;
    fwrite(&nameLen, 4, 1, f);
    fwrite(opName, 1, nameLen, f);
    fwrite(&opIndex, 4, 1, f);
    fwrite(&offset, sizeof(NnSize), 1, f);
    fwrite(&nBytes, sizeof(NnSize), 1, f);
    fwrite(data, 1, nBytes, f);
}

void NnWorkerWeightCache::writeTerminator(FILE *f) {
    NnUint zero = 0;
    fwrite(&zero, 4, 1, f);
}

uint64_t computeModelHash(const char *modelPath) {
    FILE *f = fopen(modelPath, "rb");
    if (!f) throw std::runtime_error("Cannot open model file for hashing");

    uint64_t hash = 0xcbf29ce484222325ULL;
    unsigned char buf[256];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    for (size_t i = 0; i < n; i++) {
        hash ^= buf[i];
        hash *= 0x00000100000001b3ULL;
    }
    return hash;
}
