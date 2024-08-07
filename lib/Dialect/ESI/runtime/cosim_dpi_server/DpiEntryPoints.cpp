//===- DpiEntryPoints.cpp - ESI cosim DPI calls -----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Cosim DPI function implementations. Mostly C-C++ gaskets to the C++
// RpcServer.
//
// These function signatures were generated by an HW simulator (see dpi.h) so
// we don't change them to be more rational here. The resulting code gets
// dynamically linked in and I'm concerned about maintaining binary
// compatibility with all the simulators.
//
//===----------------------------------------------------------------------===//

#include "dpi.h"
#include "esi/Ports.h"
#include "esi/backends/RpcServer.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>

using namespace esi;
using namespace esi::cosim;

/// If non-null, log to this file. Protected by 'serverMutex`.
static FILE *logFile;
static std::unique_ptr<RpcServer> server = nullptr;
static std::mutex serverMutex;

// ---- Helper functions ----

/// Emit the contents of 'msg' to the log file in hex.
static void log(char *epId, bool toClient, const MessageData &msg) {
  std::lock_guard<std::mutex> g(serverMutex);
  if (!logFile)
    return;

  fprintf(logFile, "[ep: %50s to: %4s]", epId, toClient ? "host" : "sim");
  size_t msgSize = msg.getSize();
  auto bytes = msg.getBytes();
  for (size_t i = 0; i < msgSize; ++i) {
    auto b = bytes[i];
    // Separate 32-bit words.
    if (i % 4 == 0 && i > 0)
      fprintf(logFile, " ");
    // Separate 64-bit words
    if (i % 8 == 0 && i > 0)
      fprintf(logFile, "  ");
    fprintf(logFile, " %02x", b);
  }
  fprintf(logFile, "\n");
  fflush(logFile);
}

/// Get the TCP port on which to listen. If the port isn't specified via an
/// environment variable, return 0 to allow automatic selection.
static int findPort() {
  const char *portEnv = getenv("COSIM_PORT");
  if (portEnv == nullptr) {
    printf(
        "[COSIM] RPC server port not found. Letting RPC server select one\n");
    return 0;
  }
  printf("[COSIM] Opening RPC server on port %s\n", portEnv);
  return std::strtoull(portEnv, nullptr, 10);
}

/// Check that an array is an array of bytes and has some size.
// NOLINTNEXTLINE(misc-misplaced-const)
static int validateSvOpenArray(const svOpenArrayHandle data,
                               int expectedElemSize) {
  if (svDimensions(data) != 1) {
    printf("DPI-C: ERROR passed array argument that doesn't have expected 1D "
           "dimensions\n");
    return -1;
  }
  if (svGetArrayPtr(data) == NULL) {
    printf("DPI-C: ERROR passed array argument that doesn't have C layout "
           "(ptr==NULL)\n");
    return -2;
  }
  int totalBytes = svSizeOfArray(data);
  if (totalBytes == 0) {
    printf("DPI-C: ERROR passed array argument that doesn't have C layout "
           "(total_bytes==0)\n");
    return -3;
  }
  int numElems = svSize(data, 1);
  int elemSize = numElems == 0 ? 0 : (totalBytes / numElems);
  if (numElems * expectedElemSize != totalBytes) {
    printf("DPI-C: ERROR: passed array argument that doesn't have expected "
           "element-size: expected=%d actual=%d numElems=%d totalBytes=%d\n",
           expectedElemSize, elemSize, numElems, totalBytes);
    return -4;
  }
  return 0;
}

// ---- Traditional cosim DPI entry points ----

// Lookups for registered ports. As a future optimization, change the DPI API to
// return a handle when registering wherein said handle is a pointer to a port.
std::map<std::string, ReadChannelPort &> readPorts;
std::map<ReadChannelPort *, std::future<MessageData>> readFutures;
std::map<std::string, WriteChannelPort &> writePorts;

// Register simulated device endpoints.
// - return 0 on success, non-zero on failure (duplicate EP registered).
// TODO: Change this by breaking it in two functions, one for read and one for
// write. Also return the pointer as a handle.
DPI int sv2cCosimserverEpRegister(char *endpointId, char *fromHostTypeIdC,
                                  int fromHostTypeSize, char *toHostTypeIdC,
                                  int toHostTypeSize) {
  // Ensure the server has been constructed.
  sv2cCosimserverInit();
  std::string fromHostTypeId(fromHostTypeIdC), toHostTypeId(toHostTypeIdC);

  // Both only one type allowed.
  if (!(fromHostTypeId.empty() ^ toHostTypeId.empty())) {
    printf("ERROR: Only one of fromHostTypeId and toHostTypeId can be set!\n");
    return -2;
  }
  if (readPorts.contains(endpointId)) {
    printf("ERROR: Endpoint already registered!\n");
    return -3;
  }

  if (!fromHostTypeId.empty()) {
    ReadChannelPort &port =
        server->registerReadPort(endpointId, fromHostTypeId);
    readPorts.emplace(endpointId, port);
    readFutures.emplace(&port, port.readAsync());
  } else {
    writePorts.emplace(endpointId,
                       server->registerWritePort(endpointId, toHostTypeId));
  }
  return 0;
}

// Attempt to recieve data from a client.
//   - Returns negative when call failed (e.g. EP not registered).
//   - If no message, return 0 with dataSize == 0.
//   - Assumes buffer is large enough to contain entire message. Fails if not
//     large enough. (In the future, will add support for getting the message
//     into a fixed-size buffer over multiple calls.)
DPI int sv2cCosimserverEpTryGet(char *endpointId,
                                // NOLINTNEXTLINE(misc-misplaced-const)
                                const svOpenArrayHandle data,
                                unsigned int *dataSize) {
  if (server == nullptr)
    return -1;

  auto portIt = readPorts.find(endpointId);
  if (portIt == readPorts.end()) {
    fprintf(stderr, "Endpoint not found in registry!\n");
    return -4;
  }

  ReadChannelPort &port = portIt->second;
  std::future<MessageData> &f = readFutures.at(&port);
  // Poll for a message.
  if (f.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
    // No message.
    *dataSize = 0;
    return 0;
  }
  MessageData msg = f.get();
  f = port.readAsync();
  log(endpointId, false, msg);

  // Do the validation only if there's a message available. Since the
  // simulator is going to poll up to every tick and there's not going to be
  // a message most of the time, this is important for performance.
  if (validateSvOpenArray(data, sizeof(int8_t)) != 0) {
    printf("ERROR: DPI-func=%s line=%d event=invalid-sv-array\n", __func__,
           __LINE__);
    return -2;
  }

  // Detect or verify size of buffer.
  if (*dataSize == ~0u) {
    *dataSize = svSizeOfArray(data);
  } else if (*dataSize > (unsigned)svSizeOfArray(data)) {
    printf("ERROR: DPI-func=%s line %d event=invalid-size (max %d)\n", __func__,
           __LINE__, (unsigned)svSizeOfArray(data));
    return -3;
  }
  // Verify it'll fit.
  size_t msgSize = msg.getSize();
  if (msgSize > *dataSize) {
    printf("ERROR: Message size too big to fit in HW buffer\n");
    return -5;
  }

  // Copy the message data.
  size_t i;
  auto bytes = msg.getBytes();
  for (i = 0; i < msgSize; ++i) {
    auto b = bytes[i];
    *(char *)svGetArrElemPtr1(data, i) = b;
  }
  // Zero out the rest of the buffer.
  for (; i < *dataSize; ++i) {
    *(char *)svGetArrElemPtr1(data, i) = 0;
  }
  // Set the output data size.
  *dataSize = msg.getSize();
  return 0;
}

// Attempt to send data to a client.
// - return 0 on success, negative on failure (unregistered EP).
// - if dataSize is negative, attempt to dynamically determine the size of
//   'data'.
DPI int sv2cCosimserverEpTryPut(char *endpointId,
                                // NOLINTNEXTLINE(misc-misplaced-const)
                                const svOpenArrayHandle data, int dataSize) {
  if (server == nullptr)
    return -1;

  if (validateSvOpenArray(data, sizeof(int8_t)) != 0) {
    printf("ERROR: DPI-func=%s line=%d event=invalid-sv-array\n", __func__,
           __LINE__);
    return -2;
  }

  // Detect or verify size.
  if (dataSize < 0) {
    dataSize = svSizeOfArray(data);
  } else if (dataSize > svSizeOfArray(data)) { // not enough data
    printf("ERROR: DPI-func=%s line %d event=invalid-size limit %d array %d\n",
           __func__, __LINE__, dataSize, svSizeOfArray(data));
    return -3;
  }

  // Copy the message data into 'blob'.
  std::vector<uint8_t> dataVec(dataSize);
  for (int i = 0; i < dataSize; ++i) {
    dataVec[i] = *(char *)svGetArrElemPtr1(data, i);
  }
  auto blob = std::make_unique<esi::MessageData>(dataVec);

  // Queue the blob.
  auto portIt = writePorts.find(endpointId);
  if (portIt == writePorts.end()) {
    fprintf(stderr, "Endpoint not found in registry!\n");
    return -4;
  }
  log(endpointId, true, *blob);
  WriteChannelPort &port = portIt->second;
  port.write(*blob);
  return 0;
}

// Teardown cosimserver (disconnects from primary server port, stops connections
// from active clients).
DPI void sv2cCosimserverFinish() {
  std::lock_guard<std::mutex> g(serverMutex);
  printf("[cosim] Tearing down RPC server.\n");
  if (server != nullptr) {
    server->stop();
    server = nullptr;

    fclose(logFile);
    logFile = nullptr;
  }
}

// Start cosimserver (spawns server for HW-initiated work, listens for
// connections from new SW-clients).
DPI int sv2cCosimserverInit() {
  std::lock_guard<std::mutex> g(serverMutex);
  if (server == nullptr) {
    // Open log file if requested.
    const char *logFN = getenv("COSIM_DEBUG_FILE");
    if (logFN != nullptr) {
      printf("[cosim] Opening debug log: %s\n", logFN);
      logFile = fopen(logFN, "w");
    }

    // Find the port and run.
    printf("[cosim] Starting RPC server.\n");
    server = std::make_unique<RpcServer>();
    server->run(findPort());
  }
  return 0;
}

// ---- Manifest DPI entry points ----

DPI void
sv2cCosimserverSetManifest(int esiVersion,
                           const svOpenArrayHandle compressedManifest) {
  if (server == nullptr)
    sv2cCosimserverInit();

  if (validateSvOpenArray(compressedManifest, sizeof(int8_t)) != 0) {
    printf("ERROR: DPI-func=%s line=%d event=invalid-sv-array\n", __func__,
           __LINE__);
    return;
  }

  // Copy the message data into 'blob'.
  int size = svSizeOfArray(compressedManifest);
  std::vector<uint8_t> blob(size);
  for (int i = 0; i < size; ++i) {
    blob[size - i - 1] = *(char *)svGetArrElemPtr1(compressedManifest, i);
  }
  printf("[cosim] Setting manifest (esiVersion=%d, size=%d)\n", esiVersion,
         size);
  server->setManifest(esiVersion, blob);
}

// ---- Low-level cosim DPI entry points ----

// TODO: These had the shit broken outta them in the gRPC conversion. We're not
// actively using them at the moment, but they'll have to be revived again in
// the future.

static bool mmioRegistered = false;
DPI int sv2cCosimserverMMIORegister() {
  if (mmioRegistered) {
    printf("ERROR: DPI MMIO master already registered!");
    return -1;
  }
  sv2cCosimserverInit();
  mmioRegistered = true;
  return 0;
}

DPI int sv2cCosimserverMMIOReadTryGet(uint32_t *address) {
  // assert(server);
  // LowLevel *ll = server->getLowLevel();
  // std::optional<int> reqAddress = ll->readReqs.pop();
  // if (!reqAddress.has_value())
  return -1;
  // *address = reqAddress.value();
  // ll->readsOutstanding++;
  // return 0;
}

DPI void sv2cCosimserverMMIOReadRespond(uint32_t data, char error) {
  assert(false && "unimplemented");
  // assert(server);
  // LowLevel *ll = server->getLowLevel();
  // if (ll->readsOutstanding == 0) {
  //   printf("ERROR: More read responses than requests! Not queuing
  //   response.\n"); return;
  // }
  // ll->readsOutstanding--;
  // ll->readResps.push(data, error);
}

DPI void sv2cCosimserverMMIOWriteRespond(char error) {
  assert(false && "unimplemented");
  // assert(server);
  // LowLevel *ll = server->getLowLevel();
  // if (ll->writesOutstanding == 0) {
  //   printf(
  //       "ERROR: More write responses than requests! Not queuing
  //       response.\n");
  //   return;
  // }
  // ll->writesOutstanding--;
  // ll->writeResps.push(error);
}

DPI int sv2cCosimserverMMIOWriteTryGet(uint32_t *address, uint32_t *data) {
  // assert(server);
  // LowLevel *ll = server->getLowLevel();
  // auto req = ll->writeReqs.pop();
  // if (!req.has_value())
  return -1;
  // *address = req.value().first;
  // *data = req.value().second;
  // ll->writesOutstanding++;
  // return 0;
}
