////////////////////////////////////////////////////////////////////////////////
// Copyright (c) 2014-2016, Lawrence Livermore National Security, LLC. 
// Produced at the Lawrence Livermore National Laboratory. 
// Written by the LBANN Research Team (B. Van Essen, et al.) listed in
// the CONTRIBUTORS file. <lbann-dev@llnl.gov>
//
// LLNL-CODE-697807.
// All rights reserved.
//
// This file is part of LBANN: Livermore Big Artificial Neural Network
// Toolkit. For details, see http://software.llnl.gov/LBANN or
// https://github.com/LLNL/LBANN. 
//
// Licensed under the Apache License, Version 2.0 (the "Licensee"); you
// may not use this file except in compliance with the License.  You may
// obtain a copy of the License at:
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied. See the License for the specific language governing
// permissions and limitations under the license.
//
// lbann_comm .hpp .cpp - LBANN communication utilities
////////////////////////////////////////////////////////////////////////////////

#include "lbann/lbann_comm.hpp"
#include "lbann/utils/lbann_timer.hpp"
#include "lbann/utils/lbann_exception.hpp"
#include "mpi.h"
#include <sstream>

using namespace std;
using namespace El;

// Error utility macro
#ifdef LBANN_DEBUG
#define checkMPI(mpi_call) {                                            \
    const int status = mpi_call;                                        \
    if(status != MPI_SUCCESS) {                                         \
      char error_string[MPI_MAX_ERROR_STRING];                          \
      int error_string_len;                                             \
      MPI_Error_string(status, error_string, &error_string_len);        \
      std::cerr << "MPI error: " << std::string(error_string, error_string_len) << "\n"; \
      std::cerr << "Error at " << __FILE__ << ":" << __LINE__ << "\n";  \
      throw lbann::lbann_exception("MPI error");                        \
    }                                                                   \
  }
#else
#define checkMPI(status) status
#endif // #ifdef LBANN_DEBUG

lbann::lbann_comm::lbann_comm(int _procs_per_model) :
  procs_per_model(_procs_per_model), num_model_barriers(0),
  num_intermodel_barriers(0), num_global_barriers(0), bytes_sent(0),
  bytes_received(0) {

  // Initialize parameters
  int world_size = mpi::Size(mpi::COMM_WORLD);
  if (procs_per_model == 0) {
    procs_per_model = world_size;
  }
  num_models = world_size / procs_per_model;
  model_rank = mpi::Rank(mpi::COMM_WORLD) / procs_per_model;
  rank_in_model = mpi::Rank(mpi::COMM_WORLD) % procs_per_model;

  // Check if parameters are valid
  if (procs_per_model > world_size) {
    stringstream err;
    err << __FILE__ << " " << __LINE__
        << " :: Not enough processes to create one model; procs_per_model: "
        << procs_per_model << " is larger than world_size: " << world_size;
    throw lbann_exception(err.str());
  }
  if (world_size % procs_per_model != 0) {
    stringstream err;
    err << __FILE__ << " " << __LINE__ 
        << " :: Procs per model does not divide total number of procs; procs_per_model: " 
        << procs_per_model << " total number of procs (world size): " << world_size;
    throw lbann_exception(err.str());
  }

  // Initialize model and intermodel communicators
  mpi::Split(mpi::COMM_WORLD, model_rank, rank_in_model, model_comm);
  mpi::Split(mpi::COMM_WORLD, rank_in_model, model_rank, intermodel_comm);

  // Initialize Elemental grid
  grid = new Grid(model_comm);

  // Initialize node communicators
  setup_node_comm();
  procs_per_node = mpi::Size(node_comm);
  rank_in_node = mpi::Rank(node_comm);
  
}

lbann::lbann_comm::~lbann_comm() {
  delete grid;
  mpi::Free(model_comm);
  mpi::Free(intermodel_comm);
  mpi::Free(node_comm);
  for (auto&& buf_vec : collective_bufs) {
    for (auto&& buf : buf_vec.second) {
      delete[] buf;
    }
  }
}

void lbann::lbann_comm::intermodel_sum_matrix(Mat& mat) {
  bytes_sent += sizeof(DataType) * mat.Height() * mat.Width();
  AllReduce(mat, intermodel_comm, mpi::SUM);
  bytes_received += sizeof(DataType) * mat.Height() * mat.Width();
}

void lbann::lbann_comm::intermodel_sum_matrix(DistMat& mat) {
  bytes_sent += sizeof(DataType) * mat.LocalHeight() * mat.LocalWidth();
  AllReduce(mat, intermodel_comm, mpi::SUM);
  bytes_received += sizeof(DataType) * mat.LocalHeight() * mat.LocalWidth();
}

/*void lbann::lbann_comm::nb_intermodel_sum_matrix(Mat& mat, mpi::Request& req) {
  MPI_Iallreduce(MPI_IN_PLACE, mat.Buffer(),
                 mat.Height() * mat.Width(), DataTypeMPI, MPI_SUM,
                 intermodel_comm.comm, &req);
}

void lbann::lbann_comm::nb_intermodel_sum_matrix(DistMat& mat,
                                                 mpi::Request& req) {
  // Note: This reaches into the Elemental internals where presently
  // mpi::Request is a typedef of MPI_Request and the MPI communicator
  // is mpi::Comm::comm.
  MPI_Iallreduce(MPI_IN_PLACE, mat.Buffer(),
                 mat.LocalHeight() * mat.LocalWidth(), DataTypeMPI, MPI_SUM,
                 intermodel_comm.comm, &req);
                 }*/

void lbann::lbann_comm::intermodel_broadcast_matrix(Mat& mat, int root) {
  Broadcast(mat, intermodel_comm, root);
}

void lbann::lbann_comm::intermodel_broadcast_matrix(DistMat& mat, int root) {
  Broadcast(mat, intermodel_comm, root);
}

/*void lbann::lbann_comm::nb_intermodel_broadcast_matrix(Mat& mat, int root,
                                                       mpi::Request& req) {
  MPI_Ibcast(mat.Buffer(), mat.Height() * mat.Width(), DataTypeMPI, root,
             intermodel_comm.comm, &req);
}

void lbann::lbann_comm::nb_intermodel_broadcast_matrix(DistMat& mat, int root,
                                                       mpi::Request& req) {
  MPI_Ibcast(mat.Buffer(), mat.LocalHeight() * mat.LocalWidth(), DataTypeMPI,
             root, intermodel_comm.comm, &req);
             }*/

void lbann::lbann_comm::intermodel_barrier() {
  ++num_intermodel_barriers;
  mpi::Barrier(intermodel_comm);
}

void lbann::lbann_comm::model_barrier() {
  ++num_model_barriers;
  mpi::Barrier(model_comm);
}

void lbann::lbann_comm::global_barrier() {
  ++num_global_barriers;
  mpi::Barrier(mpi::COMM_WORLD);
}

void lbann::lbann_comm::send(Mat& mat, int model, int rank) {
  send(mat.Buffer(), mat.Height() * mat.Width(), model, rank);
}

void lbann::lbann_comm::send(DistMat& mat, int model, int rank) {
  send(mat.Buffer(), mat.LocalHeight() * mat.LocalWidth(), model, rank);
}

void lbann::lbann_comm::nb_send(Mat& mat, int model, int rank,
                                mpi::Request<DataType>& req) {
  nb_send(mat.Buffer(), mat.Height() * mat.Width(), model, rank, req);
}

void lbann::lbann_comm::nb_send(DistMat& mat, int model, int rank,
                                mpi::Request<DataType>& req) {
  nb_send(mat.Buffer(), mat.LocalHeight() * mat.LocalWidth(), model, rank, req);
}

void lbann::lbann_comm::recv(Mat& mat, int model, int rank) {
  recv(mat.Buffer(), mat.Height() * mat.Width(), model, rank);
}

void lbann::lbann_comm::recv(DistMat& mat, int model, int rank) {
  recv(mat.Buffer(), mat.LocalHeight() * mat.LocalWidth(), model, rank);
}

void lbann::lbann_comm::recv(Mat& mat) {
  recv(mat.Buffer(), mat.Height() * mat.Width());
}

void lbann::lbann_comm::recv(DistMat& mat) {
  recv(mat.Buffer(), mat.LocalHeight() * mat.LocalWidth());
}

void lbann::lbann_comm::nb_recv(Mat& mat, int model, int rank,
                                mpi::Request<DataType>& req) {
  nb_recv(mat.Buffer(), mat.Height() * mat.Width(), model, rank, req);
}

void lbann::lbann_comm::nb_recv(DistMat& mat, int model, int rank,
                                mpi::Request<DataType>& req) {
  nb_recv(mat.Buffer(), mat.LocalHeight() * mat.LocalWidth(), model, rank, req);
}

void lbann::lbann_comm::nb_recv(Mat& mat, mpi::Request<DataType>& req) {
  nb_recv(mat.Buffer(), mat.Height() * mat.Width(), req);
}

void lbann::lbann_comm::nb_recv(DistMat& mat, mpi::Request<DataType>& req) {
  nb_recv(mat.Buffer(), mat.LocalHeight() * mat.LocalWidth(), req);
}

void lbann::lbann_comm::broadcast(Mat& mat,
                                  std::vector<int>& dests, int root) {
  broadcast(mat.Buffer(), mat.Height() * mat.Width(), dests, root);
}

void lbann::lbann_comm::broadcast(DistMat& mat,
                                  std::vector<int>& dests, int root) {
  broadcast(mat.Buffer(), mat.LocalHeight() * mat.LocalWidth(), dests, root);
}

void lbann::lbann_comm::intermodel_allreduce(
  Mat& mat, int max_recv_count,
  std::function<uint8_t*(Mat&, IR, IR, int&, bool)> send_transform,
  std::function<int(uint8_t*, Mat&)> recv_transform,
  std::function<int(uint8_t*, Mat&, bool)> recv_apply_transform,
  bool id_recv, bool no_local_trans) {
  // If not a power-of-2, we can't use the recursive doubling.
  const int nprocs = get_num_models();
  if (nprocs & (nprocs - 1)) {
    pe_ring_allreduce(intermodel_comm, mat, max_recv_count,
                      send_transform, recv_transform,
                      recv_apply_transform, id_recv,
                      no_local_trans);
  } else {
    // TODO: Don't hardcode this.
    if (mat.Height() <= 64 && mat.Width() <= 64) {
      recursive_doubling_allreduce_pow2(
        intermodel_comm, mat, max_recv_count,
        send_transform, recv_apply_transform, id_recv,
        no_local_trans);
    } else {
      pe_ring_allreduce(intermodel_comm, mat, max_recv_count,
                        send_transform, recv_transform,
                        recv_apply_transform, id_recv,
                        no_local_trans);
    }
  }
}

void lbann::lbann_comm::recursive_doubling_allreduce_pow2(
  mpi::Comm comm, Mat& mat, int max_recv_count,
  std::function<uint8_t*(Mat&, IR, IR, int&, bool)> send_transform,
  std::function<int(uint8_t*, Mat&, bool)> recv_apply_transform,
  bool id_recv, bool no_local_trans) {
  double ar_start = get_time();
  const int rank = mpi::Rank(comm);
  const int nprocs = mpi::Size(comm);
  if (nprocs == 1) return;  // Nothing to do.
  // This implementation requires a power-of-2 number of processes.
  if (nprocs & (nprocs - 1)) {
    throw lbann_exception("lbann_comm: recursive doubling allreduce requires"
                          " a power-of-2 number of participating processes");
  }
  uint8_t* max_recv_buf = get_collective_buffer(max_recv_count);
  uint8_t* recv_buf = max_recv_buf;
  unsigned int mask = 1;
  while (mask < nprocs) {
    int partner = rank ^ mask;  // The rank we exchange with this step.
    const bool is_local = no_local_trans && is_rank_node_local(partner, comm);
    // Transform the data we want to send.
    double send_trans_start = get_time();
    int send_size;
    int recv_size = max_recv_count;
    uint8_t* send_buf = nullptr;
    if (is_local) {
      send_buf = (uint8_t*) mat.Buffer();
      send_size = sizeof(DataType) * mat.Height() * mat.Width();
      recv_size = send_size;
      recv_buf = get_collective_buffer(recv_size);
    } else {
      send_buf = send_transform(mat, ALL, ALL, send_size, false);
      recv_buf = max_recv_buf;
    }
    ar_send_transform_time += get_time() - send_trans_start;
    bytes_sent += send_size;
    ar_bytes_sent += send_size;
    double sendrecv_start = get_time();
    mpi::SendRecv(send_buf, send_size, partner,
                  recv_buf, recv_size, partner, comm);
    double sendrecv_tot = get_time() - sendrecv_start;
    ar_send_time += sendrecv_tot;
    ar_recv_time += sendrecv_tot;
    // Transform and reduce the received data.
    double recv_apply_trans_start = get_time();
    recv_size = recv_apply_transform(recv_buf, mat, is_local);
    ar_recv_apply_transform_time += get_time() - recv_apply_trans_start;
    bytes_received += recv_size;
    ar_bytes_received += recv_size;
    mask <<= 1;
  }
  ar_time += get_time() - ar_start;
}

void lbann::lbann_comm::pe_ring_allreduce(
  mpi::Comm comm, Mat& mat, int max_recv_count,
  std::function<uint8_t*(Mat&, IR, IR, int&, bool)> send_transform,
  std::function<int(uint8_t*, Mat&)> recv_transform,
  std::function<int(uint8_t*, Mat&, bool)> recv_apply_transform,
  bool id_recv, bool no_local_trans) {
  double ar_start = get_time();
  const int rank = mpi::Rank(comm);
  const int nprocs = mpi::Size(comm);
  if (nprocs == 1) return;  // Nothing to do.
  // Compute the number of columns each processor sends.
  // If it doesn't divide evenly, give one extra to the earlier ranks.
  const Int cols_per_proc = mat.Width() / nprocs;
  const Int cols_remainder = mat.Width() % nprocs;
  // Compute the lengths/ends of the slices.
  std::vector<Int> slice_lengths(nprocs, cols_per_proc);
  for (int i = 0; i < cols_remainder; ++i) {
    slice_lengths[i] += 1;
  }
  std::vector<Int> slice_ends(nprocs);
  std::partial_sum(slice_lengths.begin(), slice_lengths.end(),
                   slice_ends.begin());
  uint8_t* max_recv_buf = get_collective_buffer(max_recv_count);
  uint8_t* recv_buf = max_recv_buf;
  // Local slice of our accumulated data.
  auto accum_view = mat(ALL, IR(slice_ends[rank] - slice_lengths[rank],
                                slice_ends[rank]));
  // Do a pairwise-exchange reduce-scatter.
  double rs_start = get_time();
  for (int step = 1; step < nprocs; ++step) {
    // Compute where we send to/receive from.
    const int dst = (rank + step) % nprocs;
    const int src = (rank - step + nprocs) % nprocs;
    const bool is_send_local = no_local_trans && is_rank_node_local(dst, comm);
    const bool is_recv_local = no_local_trans && is_rank_node_local(src, comm);
    // Transform the data we send. We do not look at the same chunk of data
    // twice.
    double send_trans_start = get_time();
    int send_size;
    int recv_size = max_recv_count;
    uint8_t* send_buf = nullptr;
    if (is_send_local) {
      auto send_view = mat(
        ALL, IR(slice_ends[dst] - slice_lengths[dst], slice_ends[dst]));
      send_buf = (uint8_t*) send_view.Buffer();
      send_size = sizeof(DataType) * send_view.Height() * send_view.Width();
    } else {
      send_buf = send_transform(
        mat, ALL, IR(slice_ends[dst] - slice_lengths[dst], slice_ends[dst]),
        send_size, true);
    }
    if (is_recv_local) {
      recv_size = sizeof(DataType) * accum_view.Height() * accum_view.Width();
      recv_buf = get_collective_buffer(recv_size);
    } else {
      recv_buf = max_recv_buf;
    }
    ar_send_transform_time += get_time() - send_trans_start;
    bytes_sent += send_size;
    ar_bytes_sent += send_size;
    ar_rs_bytes_sent += send_size;
    double sendrecv_start = get_time();
    mpi::SendRecv(send_buf, send_size, dst,
                  recv_buf, recv_size, src, comm);
    double sendrecv_tot = get_time() - sendrecv_start;
    ar_send_time += sendrecv_tot;
    ar_recv_time += sendrecv_tot;
    ar_rs_send_time += sendrecv_tot;
    ar_rs_recv_time += sendrecv_tot;
    double recv_apply_trans_start = get_time();
    recv_size = recv_apply_transform(recv_buf, accum_view, is_recv_local);
    ar_recv_apply_transform_time += get_time() - recv_apply_trans_start;
    bytes_received += recv_size;
    ar_bytes_received += recv_size;
    ar_rs_bytes_received += send_size;
  }
  recv_buf = max_recv_buf;  // Ensure we're back to the original.
  ar_rs_time += get_time() - rs_start;
  // Do a ring allgather.
  double ag_start = get_time();
  const int src = (rank - 1 + nprocs) % nprocs;
  const int dst = (rank + 1) % nprocs;
  // Apply the transform to our locally-accumulated slice of the data.
  // Since the same data is cycled to every process, we do not do the
  // no_local_trans here.
  int send_size;
  // Do the first step where we forward our local data.
  {
    double send_trans_start = get_time();
    uint8_t* send_buf = send_transform(
      mat, ALL, IR(slice_ends[rank] - slice_lengths[rank], slice_ends[rank]),
      send_size, false);
    ar_send_transform_time += get_time() - send_trans_start;
    const int data_src = (rank - 1 + nprocs) % nprocs;
    bytes_sent += send_size;
    ar_bytes_sent += send_size;
    ar_ag_bytes_sent += send_size;
    auto recv_view = mat(ALL,
                         IR(slice_ends[data_src] - slice_lengths[data_src],
                            slice_ends[data_src]));
    // If we can, receive directly into the destination matrix.
    if (id_recv) {
      recv_buf = (uint8_t*) recv_view.Buffer();
      max_recv_count = sizeof(DataType) * recv_view.Height() * recv_view.Width();
    }
    double sendrecv_start = get_time();
    mpi::SendRecv(send_buf, send_size, dst,
                  recv_buf, max_recv_count, src, comm);
    double sendrecv_tot = get_time() - sendrecv_start;
    ar_send_time += sendrecv_tot;
    ar_recv_time += sendrecv_tot;
    ar_ag_send_time += sendrecv_tot;
    ar_ag_recv_time += sendrecv_tot;
    double recv_trans_start = get_time();
    int recv_size = 0;
    if (id_recv) {
      recv_size = sizeof(DataType) * recv_view.Height() * recv_view.Width();
    } else {
      recv_size = recv_transform(recv_buf, recv_view);
    }
    ar_recv_transform_time += get_time() - recv_trans_start;
    bytes_received += recv_size;
    ar_bytes_received += recv_size;
    ar_ag_bytes_received += send_size;
    send_size = recv_size;
  }
  // Now do the remaining nprocs - 2 steps.
  // We always send from recv_buf and receive to recv_buf2, swapping
  // pointers to avoid copying.
  uint8_t* recv_buf2 = nullptr;
  if (!id_recv) {
    recv_buf2 = get_collective_buffer(max_recv_count, 1);
  }
  for (int step = 1; step < nprocs - 1; ++step) {
    // Compute where the data we get is coming from.
    const int data_src = (rank - step - 1 + nprocs) % nprocs;
    auto recv_view = mat(ALL,
                         IR(slice_ends[data_src] - slice_lengths[data_src],
                            slice_ends[data_src]));
    if (id_recv) {
      recv_buf2 = (uint8_t*) recv_view.Buffer();
      max_recv_count = sizeof(DataType) * recv_view.Height() * recv_view.Width();
    }
    bytes_sent += send_size;
    ar_bytes_sent += send_size;
    ar_ag_bytes_sent += send_size;
    double sendrecv_start = get_time();
    mpi::SendRecv(recv_buf, send_size, dst,
                  recv_buf2, max_recv_count, src, comm);
    double sendrecv_tot = get_time() - sendrecv_start;
    ar_send_time += sendrecv_tot;
    ar_recv_time += sendrecv_tot;
    ar_ag_send_time += sendrecv_tot;
    ar_ag_recv_time += sendrecv_tot;
    double recv_trans_start = get_time();
    int recv_size = 0;
    if (id_recv) {
      recv_size = sizeof(DataType) * recv_view.Height() * recv_view.Width();
    } else {
      recv_size = recv_transform(recv_buf2, recv_view);
    }
    ar_recv_transform_time += get_time() - recv_trans_start;
    bytes_received += recv_size;
    // Swap the send and receive buffers.
    std::swap(recv_buf, recv_buf2);
    send_size = recv_size;
    ar_bytes_received += recv_size;
    ar_ag_bytes_received += send_size;
  }
  ar_ag_time += get_time() - ag_start;
  ar_time += get_time() - ar_start;
}

void lbann::lbann_comm::ring_allreduce(
  mpi::Comm comm, Mat& mat, int max_recv_count,
  std::function<uint8_t*(Mat&, IR, IR, int&, bool)> send_transform,
  std::function<int(uint8_t*, Mat&)> recv_transform,
  std::function<int(uint8_t*, Mat&, bool)> recv_apply_transform,
  bool id_recv, bool no_local_trans) {
  double ar_start = get_time();
  const int rank = mpi::Rank(comm);
  const int nprocs = mpi::Size(comm);
  if (nprocs == 1) return;  // Nothing to do.
  // Compute the number of columns each processor sends.
  const Int cols_per_proc = mat.Width() / nprocs;
  const Int cols_remainder = mat.Width() % nprocs;
  // Compute the lengths/ends of the slices.
  std::vector<Int> slice_lengths(nprocs, cols_per_proc);
  for (int i = 0; i < cols_remainder; ++i) {
    slice_lengths[i] += 1;
  }
  std::vector<Int> slice_ends(nprocs);
  std::partial_sum(slice_lengths.begin(), slice_lengths.end(),
                   slice_ends.begin());
  uint8_t* max_recv_buf = get_collective_buffer(max_recv_count);
  uint8_t* recv_buf = max_recv_buf;
  // Compute source/destination in the ring.
  const int src = (rank - 1 + nprocs) % nprocs;
  const int dst = (rank + 1) % nprocs;
  const bool is_send_local = no_local_trans && is_rank_node_local(dst, comm);
  const bool is_recv_local = no_local_trans && is_rank_node_local(src, comm);
  // Do a ring-based reduce-scatter.
  // This is like the pairwise-exchange reduce-scatter except instead of
  // rank i accumulating only slice i, the slices are cycled around and
  // each node accumulates its portion into the slice when it passes
  // through. After the nprocs-1 steps slice k will be on rank
  // (k + nprocs - 1) % nprocs.
  double rs_start = get_time();
  for (int step = 0; step < nprocs - 1; ++step) {
    // Compute the slices to send/recv.
    const int send_slice = (rank - step + nprocs) % nprocs;
    const int recv_slice = (rank - step - 1 + nprocs) % nprocs;
    // Transform the data to send.
    double send_trans_start = get_time();
    int send_size;
    int recv_size = max_recv_count;
    uint8_t* send_buf = nullptr;
    if (is_send_local) {
      auto send_view = mat(
        ALL, IR(slice_ends[dst] - slice_lengths[dst], slice_ends[dst]));
      send_buf = (uint8_t*) send_view.Buffer();
      send_size = sizeof(DataType) * send_view.Height() * send_view.Width();
    } else {
      send_buf = send_transform(
        mat, ALL, IR(slice_ends[send_slice] - slice_lengths[send_slice],
                     slice_ends[send_slice]), send_size, false);
    }
    auto recv_view = mat(
      ALL, IR(slice_ends[recv_slice] - slice_lengths[recv_slice],
              slice_ends[recv_slice]));
    if (is_recv_local) {
      recv_size = sizeof(DataType) * recv_view.Height() * recv_view.Width();
      recv_buf = get_collective_buffer(recv_size);
    } else {
      recv_buf = max_recv_buf;
    }
    ar_send_transform_time += get_time() - send_trans_start;
    bytes_sent += send_size;
    ar_bytes_sent += send_size;
    ar_rs_bytes_sent += send_size;
    double sendrecv_start = get_time();
    mpi::SendRecv(send_buf, send_size, dst,
                  recv_buf, recv_size, src, comm);
    double sendrecv_tot = get_time() - sendrecv_start;
    ar_send_time += sendrecv_tot;
    ar_recv_time += sendrecv_tot;
    ar_rs_send_time += sendrecv_tot;
    ar_rs_recv_time += sendrecv_tot;
    double recv_apply_trans_start = get_time();
    recv_size = recv_apply_transform(recv_buf, recv_view, is_recv_local);
    ar_recv_apply_transform_time += get_time() - recv_apply_trans_start;
    bytes_received += recv_size;
    ar_bytes_received += recv_size;
    ar_rs_bytes_received += recv_size;
  }
  recv_buf = max_recv_buf;  // Ensure we're back to the original.
  ar_rs_time += get_time() - rs_start;
  // Do a ring allgather, first applying the transform to local data.
  double ag_start = get_time();
  int send_size;
  {
    const int send_slice = (rank + 1) % nprocs;
    const int recv_slice = rank;
    double send_trans_start = get_time();
    uint8_t* send_buf = send_transform(
      mat, ALL, IR(slice_ends[send_slice] - slice_lengths[send_slice],
                   slice_ends[send_slice]), send_size, false);
    ar_send_transform_time += get_time() - send_trans_start;
    bytes_sent += send_size;
    ar_bytes_sent += send_size;
    ar_ag_bytes_sent += send_size;
    auto recv_view = mat(ALL,
                         IR(slice_ends[recv_slice] - slice_lengths[recv_slice],
                            slice_ends[recv_slice]));
    // If we can, receive directly into the destination matrix.
    if (id_recv) {
      recv_buf = (uint8_t*) recv_view.Buffer();
      max_recv_count = sizeof(DataType) * recv_view.Height() * recv_view.Width();
    }
    double sendrecv_start = get_time();
    mpi::SendRecv(send_buf, send_size, dst,
                  recv_buf, max_recv_count, src, comm);
    double sendrecv_tot = get_time() - sendrecv_start;
    ar_send_time += sendrecv_tot;
    ar_recv_time += sendrecv_tot;
    ar_ag_send_time += sendrecv_tot;
    ar_ag_recv_time += sendrecv_tot;
    double recv_trans_start = get_time();
    int recv_size = 0;
    if (id_recv) {
      recv_size = sizeof(DataType) * recv_view.Height() * recv_view.Width();
    } else {
      recv_size = recv_transform(recv_buf, recv_view);
    }
    ar_recv_transform_time += get_time() - recv_trans_start;
    send_size = recv_size;
    bytes_received += recv_size;
    ar_bytes_received += recv_size;
    ar_ag_bytes_received += recv_size;
  }
  uint8_t* recv_buf2 = nullptr;
  if (!id_recv) {
    recv_buf2 = get_collective_buffer(max_recv_count, 1);
  }
  for (int step = 1; step < nprocs - 1; ++step) {
    const int send_slice = (rank - step + 1 + nprocs) % nprocs;
    const int recv_slice = (rank - step + nprocs) % nprocs;
    auto recv_view = mat(ALL,
                         IR(slice_ends[recv_slice] - slice_lengths[recv_slice],
                            slice_ends[recv_slice]));
    if (id_recv) {
      recv_buf2 = (uint8_t*) recv_view.Buffer();
      max_recv_count = sizeof(DataType) * recv_view.Height() * recv_view.Width();
    }
    bytes_sent += send_size;
    ar_bytes_sent += send_size;
    ar_ag_bytes_sent += send_size;
    double sendrecv_start = get_time();
    mpi::SendRecv(recv_buf, send_size, dst,
                  recv_buf2, max_recv_count, src, comm);
    double sendrecv_tot = get_time() - sendrecv_start;
    ar_send_time += sendrecv_tot;
    ar_recv_time += sendrecv_tot;
    ar_ag_send_time += sendrecv_tot;
    ar_ag_recv_time += sendrecv_tot;
    double recv_trans_start = get_time();
    int recv_size = 0;
    if (id_recv) {
      recv_size = sizeof(DataType) * recv_view.Height() * recv_view.Width();
    } else {
      recv_size = recv_transform(recv_buf2, recv_view);
    }
    ar_recv_transform_time += get_time() - recv_trans_start;
    // Swap the send and receive buffers.
    std::swap(recv_buf, recv_buf2);
    send_size = recv_size;
    bytes_received += recv_size;
    ar_bytes_received += recv_size;
    ar_ag_bytes_received += recv_size;
  }
  ar_ag_time += get_time() - ag_start;
  ar_time += get_time() - ar_start;
}

void lbann::lbann_comm::rabenseifner_allreduce(
  mpi::Comm comm, Mat& mat, int max_recv_count,
  std::function<uint8_t*(Mat&, IR, IR, int&, bool)> send_transform,
  std::function<int(uint8_t*, Mat&)> recv_transform,
  std::function<int(uint8_t*, Mat&, bool)> recv_apply_transform,
  bool id_recv, bool no_local_trans) {
  double ar_start = get_time();
  const int rank = mpi::Rank(comm);
  const int nprocs = mpi::Size(comm);
  if (nprocs == 1) return;  // Nothing to do.
  // This implementation requires a power-of-2 number of processes.
  if (nprocs & (nprocs - 1)) {
    throw lbann_exception("lbann_comm: Rabenseifner allreduce requires"
                          " a power-of-2 number of participating processes");
  }
  // Compute the slices on each processor.
  const Int cols_per_proc = mat.Width() / nprocs;
  const Int cols_remainder = mat.Width() % nprocs;
  // Compute the lengths/ends of the slices.
  std::vector<Int> slice_lengths(nprocs, cols_per_proc);
  for (int i = 0; i < cols_remainder; ++i) {
    slice_lengths[i] += 1;
  }
  std::vector<Int> slice_ends(nprocs);
  std::partial_sum(slice_lengths.begin(), slice_lengths.end(),
                   slice_ends.begin());
  // Do a recursive-halving reduce-scatter.
  // In each step here a process sends all the data needed for the other
  // "half" of the processes. i.e. each process sends half their data in the
  // first step, a quarter in the second step, etc.
  double rs_start = get_time();
  unsigned int partner_mask = nprocs >> 1;
  unsigned int slice_mask = 1;
  unsigned int send_idx = 0;
  unsigned int recv_idx = 0;
  unsigned int last_idx = nprocs;
  uint8_t* recv_buf = get_collective_buffer(max_recv_count);
  while (partner_mask > 0) {
    int partner = rank ^ partner_mask;  // The rank we exchange with this step.
    const bool is_local = no_local_trans && is_rank_node_local(partner, comm);
    // Determine the range of data to send/recv.
    IR send_range, recv_range;
    if (rank < partner) {
      send_idx = recv_idx + nprocs / (slice_mask*2);
      send_range = IR(slice_ends[send_idx] - slice_lengths[send_idx],
                      slice_ends[last_idx-1]);
      recv_range = IR(slice_ends[recv_idx] - slice_lengths[recv_idx],
                      slice_ends[send_idx-1]);
    } else {
      recv_idx = send_idx + nprocs / (slice_mask*2);
      send_range = IR(slice_ends[send_idx] - slice_lengths[send_idx],
                      slice_ends[recv_idx-1]);
      recv_range = IR(slice_ends[recv_idx] - slice_lengths[recv_idx],
                      slice_ends[last_idx-1]);
    }
    auto recv_view = mat(ALL, recv_range);
    // Transform the data to send.
    double send_trans_start = get_time();
    int send_size;
    int recv_size = max_recv_count;
    uint8_t* send_buf = nullptr;
    if (is_local) {
      auto send_view = mat(ALL, send_range);
      send_buf = (uint8_t*) send_view.Buffer();
      send_size = sizeof(DataType) * send_view.Height() * send_view.Width();
      recv_size = sizeof(DataType) * recv_view.Height() * recv_view.Width();
    } else {
      send_buf = send_transform(mat, ALL, send_range, send_size, false);
    }
    ar_send_transform_time += get_time() - send_trans_start;
    bytes_sent += send_size;
    ar_bytes_sent += send_size;
    ar_rs_bytes_sent += send_size;
    double sendrecv_start = get_time();
    mpi::SendRecv(send_buf, send_size, partner,
                  recv_buf, recv_size, partner, comm);
    double sendrecv_tot = get_time() - sendrecv_start;
    ar_send_time += sendrecv_tot;
    ar_recv_time += sendrecv_tot;
    ar_rs_send_time += sendrecv_tot;
    ar_rs_recv_time += sendrecv_tot;
    // Transform the received data.
    double recv_apply_trans_start = get_time();
    recv_size = recv_apply_transform(recv_buf, recv_view, is_local);
    ar_recv_apply_transform_time += get_time() - recv_apply_trans_start;
    bytes_received += recv_size;
    ar_bytes_received += recv_size;
    ar_rs_bytes_received += send_size;
    // Update info for next iteration.
    // Except last_idx when needed for the allgather.
    send_idx = recv_idx;
    partner_mask >>= 1;
    slice_mask <<= 1;
    if (partner_mask > 0) {
      last_idx = recv_idx + nprocs / (slice_mask);
    }
  }
  ar_rs_time += get_time() - rs_start;
  // Do a recursive-doubling algather.
  double ag_start = get_time();
  slice_mask >>= 1;
  partner_mask = 1;
  // Now do the remaining steps.
  while (partner_mask < nprocs) {
    int partner = rank ^ partner_mask;
    const bool is_local = no_local_trans && is_rank_node_local(partner, comm);
    // Determine range to send/recv.
    IR send_range, recv_range;
    if (rank < partner) {
      if (slice_mask != nprocs / 2) {
        last_idx = last_idx + nprocs / (slice_mask*2);
      }
      recv_idx = send_idx + nprocs / (slice_mask*2);
      send_range = IR(slice_ends[send_idx] - slice_lengths[send_idx],
                      slice_ends[recv_idx-1]);
      recv_range = IR(slice_ends[recv_idx] - slice_lengths[recv_idx],
                      slice_ends[last_idx-1]);
    } else {
      recv_idx = send_idx - nprocs / (slice_mask*2);
      send_range = IR(slice_ends[send_idx] - slice_lengths[send_idx],
                      slice_ends[last_idx-1]);
      recv_range = IR(slice_ends[recv_idx] - slice_lengths[recv_idx],
                      slice_ends[send_idx-1]);
    }
    auto recv_view = mat(ALL, recv_range);
    // Transform the data to send.
    double send_trans_start = get_time();
    int send_size;
    int recv_size = max_recv_count;
    uint8_t* send_buf = nullptr;
    if (is_local) {
      auto send_view = mat(ALL, send_range);
      send_buf = (uint8_t*) send_view.Buffer();
      send_size = sizeof(DataType) * send_view.Height() * send_view.Width();
      recv_size = sizeof(DataType) * recv_view.Height() * recv_view.Width();
    } else {
      send_buf = send_transform(mat, ALL, send_range, send_size, false);
    }
    ar_send_transform_time += get_time() - send_trans_start;
    if (id_recv || is_local) {
      recv_buf = (uint8_t*) recv_view.Buffer();
      recv_size = sizeof(DataType) * recv_view.Height() * recv_view.Width();
    }
    bytes_sent += send_size;
    ar_bytes_sent += send_size;
    ar_ag_bytes_sent += send_size;
    double sendrecv_start = get_time();
    mpi::SendRecv(send_buf, send_size, partner,
                  recv_buf, recv_size, partner, comm);
    double sendrecv_tot = get_time() - sendrecv_start;
    ar_send_time += sendrecv_tot;
    ar_recv_time += sendrecv_tot;
    ar_ag_send_time += sendrecv_tot;
    ar_ag_recv_time += sendrecv_tot;
    double recv_trans_start = get_time();
    if (id_recv) {
      recv_size = sizeof(DataType) * recv_view.Height() * recv_view.Width();
    } else {
      recv_size = recv_transform(recv_buf, recv_view);
    }
    ar_recv_transform_time += get_time() - recv_trans_start;
    bytes_received += recv_size;
    ar_bytes_received += recv_size;
    ar_ag_bytes_received += send_size;
    // Update for the next iteration.
    if (rank > partner) {
      send_idx = recv_idx;
    }
    partner_mask <<= 1;
    slice_mask >>= 1;
  }
  ar_ag_time += get_time() - ag_start;
  ar_time += get_time() - ar_start;
}

void lbann::lbann_comm::setup_node_comm() {
 
  // Get string specifying compute node
  char node_name[MPI_MAX_PROCESSOR_NAME];
  int node_name_len;
  checkMPI(MPI_Get_processor_name(node_name, &node_name_len));
  const std::string node_string(node_name);

  // Hash node names and split MPI processes
  int hash = std::hash<std::string>()(node_string);
  hash = hash >= 0 ? hash : -hash;  // Make sure hash is non-negative
  mpi::Comm hash_comm;
  mpi::Split(mpi::COMM_WORLD, hash, mpi::Rank(mpi::COMM_WORLD), hash_comm);
  const int hash_comm_size = mpi::Size(hash_comm);

  // Compare node names and split MPI processes
  char* node_name_list = new char[hash_comm_size*MPI_MAX_PROCESSOR_NAME];
  checkMPI(MPI_Allgather(node_name, MPI_MAX_PROCESSOR_NAME, MPI_CHAR,
                         node_name_list, MPI_MAX_PROCESSOR_NAME, MPI_CHAR,
                         hash_comm.comm));
  int node_num = mpi::Rank(hash_comm);
  for(int i=0; i<hash_comm_size; ++i) {
    const std::string other_node_string(node_name_list + i*MPI_MAX_PROCESSOR_NAME);
    if(node_string == other_node_string) {
      node_num = i;
      break;
    }
  }
  delete[] node_name_list;
  mpi::Split(hash_comm, node_num, mpi::Rank(mpi::COMM_WORLD), node_comm);
  mpi::Free(hash_comm);

  // Set up list of ranks that are local.
  int node_comm_size = mpi::Size(node_comm);
  for (int i = 0; i < node_comm_size; ++i) {
    world_ranks_on_node.push_back(
      mpi::Translate(node_comm, i, mpi::COMM_WORLD));
  }
}

uint8_t* lbann::lbann_comm::get_collective_buffer(size_t size, size_t idx) {
  auto buf_iter = collective_bufs.find(size);
  if (buf_iter == collective_bufs.end()) {
    if (idx != 0) {
      // TODO: Raise exception.
      return nullptr;
    }
    collective_bufs.emplace(std::make_pair(size, std::vector<uint8_t*>()));
    collective_bufs[size].push_back(new uint8_t[size]);
    return collective_bufs[size][0];
  } else {
    if (collective_bufs[size].size() > idx) {
      return collective_bufs[size][idx];
    } else {
      if (collective_bufs[size].size() != idx) {
        // TODO: Raise exception.
        return nullptr;
      }
      collective_bufs[size].push_back(new uint8_t[size]);
      return collective_bufs[size][idx];
    }
  }
}
