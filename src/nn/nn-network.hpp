#ifndef NN_NETWORK_H
#define NN_NETWORK_H

#include "nn-executor.hpp"
#include "nn-worker-cache.hpp"
#include <cstdint>

#define ROOT_SOCKET_INDEX 0

// Worker discovery protocol magic values
#define WORKER_HELLO_MAGIC  0x574F524BU
#define WORKER_ASSIGN_MAGIC 0x41535347U
// Sent by server as nameSize=0xFFFFFFFF in weight stream → worker has valid cache
#define WEIGHT_CACHE_SKIP   0xFFFFFFFFU

struct WorkerRegistrationResult {
    NnUint nodeIndex;
    NnUint nNodes;
    uint64_t modelHash;
    bool usedCache;
};

struct WorkerSysInfo {
    char displayHostname[256]; // sent by worker in HELLO
    char connectHost[256];     // IP from getpeername on server side
    NnUint listenPort;
    NnUint cpuCores;
    NnUint cpuMhz;
    NnUint totalMemoryMb;
};

// Cross-platform system info helpers
NnUint getSysCpuCores();
NnUint getSysCpuMhz();
NnUint getSysTotalMemoryMb();
void getSysHostname(char *buf, NnUint size);

// Server: accept nWorkers registrations on host:port, fill outHosts/outPorts/outCacheValid/outSysInfo
void acceptWorkerRegistrations(const char *host, NnUint port, NnUint nWorkers,
    uint64_t modelHash, char **outHosts, NnUint *outPorts, bool *outCacheValid,
    WorkerSysInfo *outSysInfo);

// Worker: register with server at serverHost:serverPort, listen on myListenPort
WorkerRegistrationResult registerWithServer(const char *serverHost, NnUint serverPort,
    NnUint myListenPort, const char *cacheDir);

void initSockets();
void cleanupSockets();
int acceptSocket(int serverSocket);
void setReuseAddr(int socket);
void writeSocket(int socket, const void* data, NnSize size);
void readSocket(int socket, void* data, NnSize size);
int createServerSocket(const char *host, const int port);
void destroySocket(int serverSocket);

class NnConnectionSocketException : public std::runtime_error {
public:
    NnConnectionSocketException(const std::string message);
};

class NnTransferSocketException : public std::runtime_error {
public:
    int code;
    NnTransferSocketException(int code, const std::string message);
};

class NnSocket {
public:
    int fd;
    NnSocket();
    NnSocket(int fd);
    ~NnSocket();
    void assign(int fd);
    int release();
};

struct NnSocketIo {
    NnUint socketIndex;
    const void *data;
    NnSize size;
};

class NnNetwork {
private:
    int *sockets;
    NnSize *sentBytes;
    NnSize *recvBytes;

public:
    static std::unique_ptr<NnNetwork> serve(const char *host, const int port);
    static std::unique_ptr<NnNetwork> connect(NnUint nSockets, char **hosts, NnUint *ports);

    NnUint nSockets;

    NnNetwork(std::vector<NnSocket> *sockets);
    ~NnNetwork();

    void setTurbo(bool enabled);
    void write(const NnUint socketIndex, const void *data, const NnSize size);
    void read(const NnUint socketIndex, void *data, const NnSize size);
    void writeAck(const NnUint socketIndex);
    void readAck(const NnUint socketIndex);
    bool tryReadWithMaxAttempts(NnUint socketIndex, void *data, NnSize size, unsigned long maxAttempts);
    void writeMany(NnUint n, NnSocketIo *ios);
    void writeAll(void *data, NnSize size);
    void readMany(NnUint n, NnSocketIo *ios);
    void getStats(NnSize *sentBytes, NnSize *recvBytes);
    void resetStats();
};

class NnNetworkNodeSynchronizer : public NnNodeSynchronizer {
private:
    NnNetwork *network;
    NnNetExecution *execution;
    NnNetConfig *netConfig;
    NnNodeConfig *nodeConfig;
public:
    NnNetworkNodeSynchronizer(NnNetwork *network, NnNetExecution *execution, NnNetConfig *netConfig, NnNodeConfig *nodeConfig);
    ~NnNetworkNodeSynchronizer() override {};
    void sync(NnUint segmentIndex, NnUint nThreads, NnUint threadIndex) override;
};

class NnRootConfigWriter {
private:
    NnNetwork *network;
public:
    NnRootConfigWriter(NnNetwork *network);
    void writeNet(NnUint socketIndex, NnNetConfig *config);
    void writeNode(NnUint socketIndex, NnNodeConfig *config);
    void writeToWorkers(NnNetConfig *netConfig, NnNodeConfig *nodeConfigs);
};

class NnWorkerConfigReader {
private:
    NnNetwork *network;
public:
    NnWorkerConfigReader(NnNetwork *network);
    NnNetConfig readNet();
    NnNodeConfig readNode();
};

class NnWeightLoader {
public:
    virtual ~NnWeightLoader() = default;
    virtual NnSize loadRoot(const char *opName, NnUint opIndex, NnSize nBytes, NnByte *weight) = 0;
    virtual NnSize loadAll(const char *opName, NnUint opIndex, NnSize nBytes, NnByte *weight) = 0;
    virtual NnSize loadRowMatmulSlices(const char *opName, NnUint opIndex, NnUint expertIndex, NnRowMatmulSlice *slice, NnByte *weight) = 0;
    virtual NnSize loadColMatmulSlices(const char *opName, NnUint opIndex, NnUint expertIndex, NnColMatmulSlice *slice, NnByte *weight) = 0;
    virtual void finish() = 0;
};

class NnRootWeightLoader : public NnWeightLoader {
private:
    NnExecutor *executor;
    NnNetwork *network;
    NnUint nNodes;
    NnByte *temp;
    NnSize tempSize;
    bool *workerCacheValid; // size = nNodes - 1, indexed by (nodeIndex - 1)
public:
    NnRootWeightLoader(NnExecutor *executor, NnNetwork *network, NnUint nNodes);
    ~NnRootWeightLoader();
    void setWorkerCacheValid(NnUint nodeIndex, bool valid);
    void writeWeight(NnUint nodeIndex, const char *opName, NnUint opIndex, NnSize offset, NnSize nBytes, NnByte *weight);
    NnSize loadRoot(const char *opName, NnUint opIndex, NnSize nBytes, NnByte *weight) override;
    NnSize loadAll(const char *opName, NnUint opIndex, NnSize nBytes, NnByte *weight) override;
    NnSize loadRowMatmulSlices(const char *opName, NnUint opIndex, NnUint expertIndex, NnRowMatmulSlice *slice, NnByte *weight) override;
    NnSize loadColMatmulSlices(const char *opName, NnUint opIndex, NnUint expertIndex, NnColMatmulSlice *slice, NnByte *weight) override;
    void finish() override;
private:
    void allocate(NnSize size);
};

// Generates a worker weight cache file from a local model file without network transfer.
// Call loadLlmNetWeight with this loader, then finish() to write and close the cache.
class NnPrepareWorkerWeightLoader : public NnWeightLoader {
public:
    NnPrepareWorkerWeightLoader(NnUint targetNodeIndex, NnUint nNodes, NnWorkerWeightCache *cache);
    ~NnPrepareWorkerWeightLoader();
    NnSize loadRoot(const char *opName, NnUint opIndex, NnSize nBytes, NnByte *weight) override;
    NnSize loadAll(const char *opName, NnUint opIndex, NnSize nBytes, NnByte *weight) override;
    NnSize loadRowMatmulSlices(const char *opName, NnUint opIndex, NnUint expertIndex, NnRowMatmulSlice *slice, NnByte *weight) override;
    NnSize loadColMatmulSlices(const char *opName, NnUint opIndex, NnUint expertIndex, NnColMatmulSlice *slice, NnByte *weight) override;
    void finish() override;
private:
    NnUint targetNodeIndex;
    NnUint nNodes;
    FILE *cacheFile;
    NnByte *temp;
    NnSize tempSize;
    void allocate(NnSize size);
};

class NnWorkerWeightReader {
private:
    NnExecutor *executor;
    NnNetwork *network;
    NnByte *temp;
    NnUint tempSize;
    NnWorkerWeightCache *cache; // may be null
    bool useCache;              // if true, skip network and load from cache
public:
    NnWorkerWeightReader(NnExecutor *executor, NnNetwork *network);
    NnWorkerWeightReader(NnExecutor *executor, NnNetwork *network, NnWorkerWeightCache *cache, bool useCache);
    ~NnWorkerWeightReader();
    void read();
private:
    void allocate(NnUint size);
};

#endif
