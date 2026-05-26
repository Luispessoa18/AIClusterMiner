#ifndef NN_WORKER_CACHE_HPP
#define NN_WORKER_CACHE_HPP

#include "nn-executor.hpp"
#include <cstdint>
#include <string>
#include <cstdio>

#define WORKER_CACHE_MAGIC 0xDCA4E001U

class NnWorkerWeightCache {
private:
    std::string cacheDir;
    uint64_t modelHash;
    NnUint nNodes;
    NnUint nodeIndex;

    std::string getCachePath() const;

public:
    NnWorkerWeightCache(const char *cacheDir, uint64_t modelHash, NnUint nNodes, NnUint nodeIndex);
    bool isValid() const;
    void loadFromDisk(NnExecutor *executor) const;
    FILE *openForWriting() const;

    static void writeEntry(FILE *f, const char *opName, NnUint opIndex, NnSize offset, NnSize nBytes, const NnByte *data);
    static void writeTerminator(FILE *f);
};

uint64_t computeModelHash(const char *modelPath);

#endif
