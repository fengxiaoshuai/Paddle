// Copyright (c) 2022 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once
#include <thrust/functional.h>

#include <algorithm>
#include <functional>
#include <string>

#include "paddle/fluid/operators/math.h"
#include "paddle/phi/backends/gpu/gpu_primitives.h"
#include "paddle/phi/core/dense_tensor.h"
#include "paddle/phi/core/hostdevice.h"

namespace phi {
static constexpr int kNumCUDAThreads = 512;
static constexpr int kNumMaxinumNumBlocks = 4096;
static const int NTHREADS = 32;
static inline int NumBlocks(const int N) {
  return std::min((N + kNumCUDAThreads - 1) / kNumCUDAThreads,
                  kNumMaxinumNumBlocks);
}

template <typename T>
__global__ void GPUNLLLossForward1D_no_reduce(T* out_data,
                                              const T* x_data,
                                              const int64_t* label_data,
                                              const T* weight_data,
                                              const int64_t batch_size,
                                              const int64_t n_classes,
                                              const int64_t ignore_index) {
  CUDA_KERNEL_LOOP(i, batch_size) {
    const int64_t cur_label = label_data[i];
    if (cur_label == ignore_index) {
      out_data[i] = 0;
      continue;
    }
    PADDLE_ENFORCE(cur_label >= 0 && cur_label < n_classes,
                   "label should not be out of bounds.");
    const T cur_weight = weight_data ? weight_data[cur_label] : (T)1;
    out_data[i] = -x_data[i * n_classes + cur_label] * cur_weight;
  }
}

template <typename T>
__global__ void GPUNLLLossForward1D_with_reduce(T* out_data,
                                                T* total_weight_data,
                                                const T* x_data,
                                                const int64_t* label_data,
                                                const T* weight_data,
                                                const int64_t batch_size,
                                                const int64_t n_classes,
                                                const int64_t size_average,
                                                const int64_t ignore_index) {
  __shared__ T sharedInputs[NTHREADS], sharedWeights[NTHREADS];
  sharedInputs[threadIdx.x] = 0;
  sharedWeights[threadIdx.x] = 0;
  int i;
  for (i = threadIdx.x; i < batch_size; i += NTHREADS) {
    const auto cur_label = label_data[i];
    if (cur_label != ignore_index) {
      PADDLE_ENFORCE(cur_label >= 0 && cur_label < n_classes,
                     "label should not be out of bounds.");
      const auto cur_weight = weight_data ? weight_data[cur_label] : (T)1;
      sharedInputs[threadIdx.x] -=
          x_data[i * n_classes + cur_label] * cur_weight;
      sharedWeights[threadIdx.x] += cur_weight;
    }
  }
  __syncthreads();

  if (threadIdx.x == 0) {
    *out_data = *total_weight_data = 0;
    T output_val = 0;
    T total_weight_val = 0;
    for (i = 0; i < NTHREADS; ++i) {
      output_val += sharedInputs[i];
      total_weight_val += sharedWeights[i];
    }
    *total_weight_data = total_weight_val;
    *out_data = output_val;

    if (size_average && *total_weight_data != 0) {
      *out_data = output_val / total_weight_val;
    }
  }
}

// Reduce N values concurrently, i.e. suppose N = 2, and there are 4 threads:
// (1, 2), (3, 4), (5, 6), (7, 8), then the return in threadVals for thread 0
// is (1 + 3 + 5 + 7, 2 + 4 + 6 + 8) = (16, 20)
//
// If smem is not used again, there is no need to __syncthreads before this
// call. However, if smem will be used, e.g., this function is called in a loop,
// then __syncthreads is needed either before or afterwards to prevent non-0
// threads overriding smem in the next loop before num-0 thread reads from it.
template <typename T, typename ReduceOp, int N>
__device__ void reduceNValuesInBlock(T* smem,
                                     T threadVals[N],
                                     const unsigned int numVals,
                                     ReduceOp reduceOp,
                                     T init) {
  if (numVals == 0) {
#pragma unroll
    for (int i = 0; i < N; ++i) {
      threadVals[i] = init;
    }
    return;
  }

  // We store each of the N values contiguously, so if N = 2, all values for
  // the first threadVal for each thread in the block are stored followed by
  // all of the values for the second threadVal for each thread in the block
  if (threadIdx.x < numVals) {
#pragma unroll
    for (int i = 0; i < N; ++i) {
      smem[i * numVals + threadIdx.x] = threadVals[i];
    }
  }
  __syncthreads();

  // Number of lanes in the final reduction --> this is used to determine
  // where to put the outputs of each of the n things we are reducing. If
  // nLP = 32, then we have the 32 outputs for the first threadVal,
  // followed by the 32 outputs for the second threadVal, etc.
  const unsigned int numLanesParticipating = min(numVals, warpSize);

  if (numVals > warpSize && ((threadIdx.x / warpSize) == 0)) {
#pragma unroll
    for (int i = 0; i < N; ++i) {
      threadVals[i] = threadIdx.x < numVals ? threadVals[i] : init;
    }

    for (int i = warpSize + threadIdx.x; i < numVals; i += warpSize) {
#pragma unroll
      for (int j = 0; j < N; ++j) {
        threadVals[j] = reduceOp(threadVals[j], smem[j * numVals + i]);
      }
    }

#pragma unroll
    for (int i = 0; i < N; ++i) {
      smem[i * numLanesParticipating + threadIdx.x] = threadVals[i];
    }
  }
  __syncthreads();

  if (threadIdx.x == 0) {
    if (numLanesParticipating == 32) {
#pragma unroll
      for (int i = 0; i < N; ++i) {
#pragma unroll
        for (int j = 1; j < 32; ++j) {
          threadVals[i] = reduceOp(threadVals[i], smem[i * 32 + j]);
        }
      }
    } else {
#pragma unroll
      for (int i = 0; i < N; ++i) {
        for (int j = 1; j < numLanesParticipating; ++j) {
          threadVals[i] = reduceOp(threadVals[i], smem[i * numVals + j]);
        }
      }
    }
  }
}

// Block-wide reduction in shared memory helper; only threadIdx.x == 0 will
// return the reduced value
//
// If smem is not used again, there is no need to __syncthreads before this
// call. However, if smem will be used, e.g., this function is called in a loop,
// then __syncthreads is needed either before or afterwards to prevent non-0
// threads overriding smem in the next loop before num-0 thread reads from it.
template <typename T, typename ReduceOp>
__device__ T reduceBlock(T* smem,
                         const unsigned int numVals,
                         T threadVal,
                         ReduceOp reduceOp,
                         T init) {
  reduceNValuesInBlock<T, ReduceOp, 1>(
      smem, &threadVal, numVals, reduceOp, init);
  return threadVal;
}

template <typename T>
__global__ void GPUNLLLossForward2D_no_reduce(T* out_data,
                                              const T* x_data,
                                              const int64_t* label_data,
                                              const T* weight_data,
                                              const int64_t batch_size,
                                              const int64_t n_classes,
                                              const int64_t in_dim2,
                                              const int64_t in_dim3,
                                              const int64_t ignore_index) {
  const int64_t map_size = in_dim2 * in_dim3;
  const int64_t sample_size = n_classes * map_size;
  const int64_t out_numel = batch_size * map_size;
  CUDA_KERNEL_LOOP(i, out_numel) {
    const int64_t b = i % batch_size;
    const int64_t h = (i / batch_size) % in_dim2;
    const int64_t w = (i / (batch_size * in_dim2)) % in_dim3;

    const int64_t index = b * map_size + h * in_dim3 + w;
    const int64_t cur_label = label_data[index];
    if (cur_label == ignore_index) {
      out_data[index] = 0;
      continue;
    }
    PADDLE_ENFORCE(cur_label >= 0 && cur_label < n_classes,
                   "label should not be out of bounds.");
    const T cur_weight = weight_data ? weight_data[cur_label] : (T)1;
    out_data[index] =
        -x_data[b * sample_size + cur_label * map_size + h * in_dim3 + w] *
        cur_weight;
  }
}

template <typename T>
__global__ void GPUNLLLossForward2D_with_reduce(T* out_data,
                                                T* total_weight_data,
                                                const T* x_data,
                                                const int64_t* label_data,
                                                const T* weight_data,
                                                const int64_t batch_size,
                                                const int64_t n_classes,
                                                const int64_t map_nelem,
                                                const int64_t blocks_per_sample,
                                                const int64_t ignore_index) {
  __shared__ T partial_sums[kNumCUDAThreads];
  int64_t i;
  T input_sum = 0;
  T acc_weight = 0;
  *out_data = 0;
  *total_weight_data = 0;

  int64_t sample = blockIdx.x / blocks_per_sample;
  int64_t toffset = sample * map_nelem;
  int64_t ioffset = sample * map_nelem * n_classes;
  int64_t step = blockDim.x * blocks_per_sample;
  for (i = (blockIdx.x % blocks_per_sample) * blockDim.x + threadIdx.x;
       i < map_nelem;
       i += step) {
    const int64_t cur_label = label_data[toffset + i];
    if (cur_label != ignore_index) {
      PADDLE_ENFORCE(cur_label >= 0 && cur_label < n_classes,
                     "label should not be out of bounds.");
      const T cur_weight = weight_data ? weight_data[cur_label] : (T)1;
      input_sum -= x_data[ioffset + i + map_nelem * cur_label] * cur_weight;
      acc_weight += cur_weight;
    }
  }

  input_sum =
      reduceBlock(partial_sums, blockDim.x, input_sum, thrust::plus<T>(), (T)0);
  __syncthreads();
  acc_weight = reduceBlock(
      partial_sums, blockDim.x, acc_weight, thrust::plus<T>(), (T)0);

  if (threadIdx.x == 0) {
    phi::CudaAtomicAdd(total_weight_data, acc_weight);
    phi::CudaAtomicAdd(out_data, input_sum);
  }
}

template <typename T>
__global__ void GPUNLLLossForward2D_size_average(T* out_data,
                                                 T* total_weight_data) {
  if (*total_weight_data != 0) {
    *out_data /= *total_weight_data;
  }
}
template <typename T>
__global__ void GPUNLLLossBackward1D_no_reduce(T* dx_data,
                                               const int64_t* label_data,
                                               const T* weight_data,
                                               const T* dout_data,
                                               const int64_t batch_size,
                                               const int64_t n_classes,
                                               const int64_t ignore_index) {
  CUDA_KERNEL_LOOP(i, batch_size) {
    const int64_t cur_label = label_data[i];
    if (cur_label == ignore_index) {
      continue;
    }
    const T cur_weight = weight_data ? weight_data[cur_label] : (T)1;
    dx_data[i * n_classes + cur_label] = -dout_data[i] * cur_weight;
  }
}

template <typename T>
__global__ void GPUNLLLossBackward1D_with_reduce(T* dx_data,
                                                 const T* total_weight_data,
                                                 const int64_t* label_data,
                                                 const T* weight_data,
                                                 const T* dout_data,
                                                 const int64_t batch_size,
                                                 const int64_t n_classes,
                                                 const int64_t size_average,
                                                 const int64_t ignore_index) {
  if (*total_weight_data <= 0) {
    return;
  }
  int i;
  const T norm = size_average ? (T)(1 / *total_weight_data) : (T)1;
  for (i = threadIdx.x; i < batch_size; i += NTHREADS) {
    const int64_t cur_label = label_data[i];
    if (cur_label != ignore_index) {
      const T cur_weight = weight_data ? weight_data[cur_label] : (T)1;
      dx_data[i * n_classes + cur_label] = -cur_weight * dout_data[0] * norm;
    }
  }
}

template <typename T>
__global__ void GPUNLLLossBackward2D_no_reduce(T* dx_data,
                                               const int64_t* label_data,
                                               const T* weight_data,
                                               const T* dout_data,
                                               const int64_t batch_size,
                                               const int64_t n_classes,
                                               const int64_t in_dim2,
                                               const int64_t in_dim3,
                                               const int64_t ignore_index) {
  const int64_t map_size = in_dim2 * in_dim3;
  const int64_t sample_size = n_classes * map_size;
  const int64_t out_numel = batch_size * map_size;
  CUDA_KERNEL_LOOP(i, out_numel) {
    const int64_t b = i % batch_size;
    const int64_t h = (i / batch_size) % in_dim2;
    const int64_t w = (i / (batch_size * in_dim2)) % in_dim3;
    const int64_t index = b * map_size + h * in_dim3 + w;
    const int64_t cur_label = label_data[index];
    if (cur_label == ignore_index) {
      continue;
    }
    const T cur_weight = weight_data ? weight_data[cur_label] : (T)1;
    dx_data[b * sample_size + cur_label * map_size + h * in_dim3 + w] =
        -dout_data[index] * cur_weight;
  }
}

template <typename T>
__global__ void GPUNLLLossBackward2D_with_reduce(
    T* dx_data,
    const T* total_weight_data,
    const int64_t* label_data,
    const T* weight_data,
    const T* dout_data,
    const int64_t batch_size,
    const int64_t n_classes,
    const int64_t map_nelem,
    const int64_t blocks_per_sample,
    const int64_t size_average,
    const int64_t ignore_index) {
  if (*total_weight_data <= 0) {
    return;
  }
  int64_t i;
  const T norm = size_average ? (T)(1 / *total_weight_data) : (T)1;
  int sample = blockIdx.x / blocks_per_sample;
  int step = blockDim.x * blocks_per_sample;
  int toffset = sample * map_nelem;
  int ioffset = sample * map_nelem * n_classes;
  for (i = (blockIdx.x % blocks_per_sample) * blockDim.x + threadIdx.x;
       i < map_nelem;
       i += step) {
    const int64_t cur_label = label_data[toffset + i];
    if (cur_label != ignore_index) {
      dx_data[ioffset + i + map_nelem * cur_label] =
          -(weight_data ? weight_data[cur_label] : (T)1) * norm * dout_data[0];
    }
  }
}

}  // namespace phi
