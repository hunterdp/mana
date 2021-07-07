#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <mpi.h>
#include <pthread.h>
#include <map>

#include "dmtcp.h"
#include "util.h"
#include "jassert.h"
#include "jfilesystem.h"

#include "mpi_plugin.h"
#include "mpi_nextfunc.h"
#include "p2p_comm.h"
#include "virtual-ids.h"

using namespace dmtcp;

// Map of unserviced irecv/isend requests to MPI call params
static dmtcp::map<MPI_Request, mpi_async_call_t*> g_async_calls;
static int g_world_rank = -1; // Global rank of the current process
static int g_world_size = -1; // Total number of ranks in the current computation
// Mutex protecting g_async_calls
static pthread_mutex_t logMutex = PTHREAD_MUTEX_INITIALIZER;

  void
getLocalRankInfo()
{
  if (g_world_rank == -1) {
    JASSERT(MPI_Comm_rank(MPI_COMM_WORLD, &g_world_rank) == MPI_SUCCESS &&
        g_world_rank != -1);
  }
  if (g_world_size == -1) {
    JASSERT(MPI_Comm_size(MPI_COMM_WORLD, &g_world_size) == MPI_SUCCESS &&
        g_world_size != -1);
  }
}

  void
updateCkptDirByRank()
{
  const char *ckptDir = dmtcp_get_ckpt_dir();
  dmtcp::string baseDir;

  if (strstr(ckptDir, "ckpt_rank_") != NULL) {
    baseDir = jalib::Filesystem::DirName(ckptDir);
  } else {
    baseDir = ckptDir;
  }
  JTRACE("Updating checkpoint directory")(ckptDir)(baseDir);
  dmtcp::ostringstream o;
  o << baseDir << "/ckpt_rank_" << g_world_rank;
  dmtcp_set_ckpt_dir(o.str().c_str());

  if (!g_list || g_numMmaps == 0) return;
  o << "/lhregions.dat";
  dmtcp::string fname = o.str();
  int fd = open(fname.c_str(), O_CREAT | O_WRONLY);
#if 0
  // g_range (lh_memory_range) was written for debugging here.
  Util::writeAll(fd, g_range, sizeof(*g_range));
#endif
  Util::writeAll(fd, &g_numMmaps, sizeof(g_numMmaps));
  for (int i = 0; i < g_numMmaps; i++) {
    Util::writeAll(fd, &g_list[i], sizeof(g_list[i]));
  }
  close(fd);
}


void
addPendingRequestToLog(mpi_req_t req, const void* sbuf, void* rbuf, int cnt,
                       MPI_Datatype type, int remote, int tag,
                       MPI_Comm comm, MPI_Request rq)
{
  mpi_async_call_t *call =
        (mpi_async_call_t*)JALLOC_HELPER_MALLOC(sizeof(mpi_async_call_t));
  call->type = req;
  call->sendbuf = sbuf;
  call->recvbuf = rbuf;
  call->count = cnt;
  call->datatype = type;
  call->remote_node = remote;
  call->tag = tag;
  call->comm = comm;
  pthread_mutex_lock(&logMutex);
  g_async_calls[rq] = call;
  pthread_mutex_unlock(&logMutex);
}

void
clearPendingRequestFromLog(MPI_Request req)
{
  pthread_mutex_lock(&logMutex);
  if (g_async_calls.find(req) != g_async_calls.end()) {
    mpi_async_call_t *call = g_async_calls[req];
    if (call) {
      JALLOC_HELPER_FREE(call);
    }
    g_async_calls.erase(req);
  }
  pthread_mutex_unlock(&logMutex);
}

void
replayMpiP2pOnRestart()
{
  MPI_Request request;
  mpi_async_call_t *call = NULL;
  JTRACE("Replaying unserviced isend/irecv calls");

  for (std::pair<MPI_Request, mpi_async_call_t*> it : g_async_calls) {
    int retval = 0;
    request = it.first;
    call = it.second;
    MPI_Request realRequest = VIRTUAL_TO_REAL_REQUEST(request);
    MPI_Comm realComm = VIRTUAL_TO_REAL_COMM(call->comm);
    MPI_Datatype realType = VIRTUAL_TO_REAL_TYPE(call->datatype);
    switch (call->type) {
      case IRECV_REQUEST:
        JTRACE("Replaying Irecv call")(call->remote_node);
        JUMP_TO_LOWER_HALF(lh_info.fsaddr);
        retval = NEXT_FUNC(Irecv)(call->recvbuf, call->count,
                                  realType, call->remote_node,
                                  call->tag, realComm, &realRequest);
        RETURN_TO_UPPER_HALF();
        JASSERT(retval == MPI_SUCCESS).Text("Error while replaying recv");
        UPDATE_REQUEST_MAP(request, realRequest);
        break;
      case ISEND_REQUEST:
        JTRACE("Replaying Isend call")(call->remote_node);
        JUMP_TO_LOWER_HALF(lh_info.fsaddr);
        retval = NEXT_FUNC(Isend)(call->sendbuf, call->count,
                                  realType, call->remote_node,
                                  call->tag, realComm, &realRequest);
        RETURN_TO_UPPER_HALF();
        JASSERT(retval == MPI_SUCCESS).Text("Error while replaying recv");
        UPDATE_REQUEST_MAP(request, realRequest);
        break;
      default:
        JWARNING(false)(call->type).Text("Unhandled replay call");
        break;
    }
  }
}

// Publishes the unserviced sends' metadata to the DMTCP coordinator.
void
registerUnservicedSends()
{
}

// Publishes the count of drained sends to the DMTCP coordinator.
void
registerDrainedSends()
{
}
