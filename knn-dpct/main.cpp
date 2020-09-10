/** Modifed version of knn-CUDA from https://github.com/vincentfpgarcia/kNN-CUDA
 * The modifications are
 *      removed texture memory usage
 *      removed split query KNN computation
 *      added feature extraction with bilinear interpolation
 *
 * Last modified by Christopher B. Choy <chrischoy@ai.stanford.edu> 12/23/2016
 */

// Includes
#include <CL/sycl.hpp>
#include <dpct/dpct.hpp>
#include <cmath>
#include <algorithm>
#include <sys/time.h>

// Constants used by the program
#define BLOCK_DIM 16


/**
  * Computes the distance between two matrix A (reference points) and
  * B (query points) containing respectively wA and wB points.
  *
  * @param A     pointer on the matrix A
  * @param wA    width of the matrix A = number of points in A
  * @param B     pointer on the matrix B
  * @param wB    width of the matrix B = number of points in B
  * @param dim   dimension of points = height of matrices A and B
  * @param AB    pointer on the matrix containing the wA*wB distances computed
  */
void cuComputeDistanceGlobal(float *A, int wA, float *B, int wB,
                                        int dim, float *AB,
                                        sycl::nd_item<3> item_ct1,
                                        dpct::accessor<float, dpct::local, 2> shared_A,
                                        dpct::accessor<float, dpct::local, 2> shared_B,
                                        int *begin_A, int *begin_B, int *step_A,
                                        int *step_B, int *end_A) {

  // Declaration of the shared memory arrays As and Bs used to store the
  // sub-matrix of A and B

  // Sub-matrix of A (begin, step, end) and Sub-matrix of B (begin, step)

  // Thread index
  int tx = item_ct1.get_local_id(2);
  int ty = item_ct1.get_local_id(1);

  // Other variables
  float tmp;
  float ssd = 0;

  // Loop parameters
  *begin_A = BLOCK_DIM * item_ct1.get_group(1);
  *begin_B = BLOCK_DIM * item_ct1.get_group(2);
  *step_A = BLOCK_DIM * wA;
  *step_B = BLOCK_DIM * wB;
  *end_A = *begin_A + (dim - 1) * wA;

  // Conditions
  int cond0 = (*begin_A + tx < wA); // used to write in shared memory
  int cond1 =
      (*begin_B + tx < wB); // used to write in shared memory & to
                            // computations and to write in output matrix
  int cond2 = (*begin_A + ty <
               wA); // used to computations and to write in output matrix

  // Loop over all the sub-matrices of A and B required to compute the block
  // sub-matrix
  for (int a = (*begin_A), b = (*begin_B); a <= *end_A;
       a += *step_A, b += *step_B) {
    // Load the matrices from device memory to shared memory; each thread loads
    // one element of each matrix
    if (a / wA + ty < dim) {
      shared_A[ty][tx] = (cond0) ? A[a + wA * ty + tx] : 0;
      shared_B[ty][tx] = (cond1) ? B[b + wB * ty + tx] : 0;
    } else {
      shared_A[ty][tx] = 0;
      shared_B[ty][tx] = 0;
    }

    // Synchronize to make sure the matrices are loaded
    item_ct1.barrier();

    // Compute the difference between the two matrixes; each thread computes one
    // element of the block sub-matrix
    if (cond2 && cond1) {
      for (int k = 0; k < BLOCK_DIM; ++k) {
        tmp = shared_A[k][ty] - shared_B[k][tx];
        ssd += tmp * tmp;
      }
    }

    // Synchronize to make sure that the preceding computation is done before
    // loading two new sub-matrices of A and B in the next iteration
    item_ct1.barrier();
  }

  // Write the block sub-matrix to device memory; each thread writes one element
  if (cond2 && cond1)
    AB[(*begin_A + ty) * wB + *begin_B + tx] = ssd;
}

/**
  * Gathers k-th smallest distances for each column of the distance matrix in
 * the top.
  *
  * @param dist        distance matrix
  * @param ind         index matrix
  * @param width       width of the distance matrix and of the index matrix
  * @param height      height of the distance matrix and of the index matrix
  * @param k           number of neighbors to consider
  */
void cuInsertionSort(float *dist, int *ind, int width, int height,
                                int k, sycl::nd_item<3> item_ct1) {

  // Variables
  int l, i, j;
  float *p_dist;
  int *p_ind;
  float curr_dist, max_dist;
  int curr_row, max_row;
  unsigned int xIndex =
      item_ct1.get_group(2) * item_ct1.get_local_range().get(2) +
      item_ct1.get_local_id(2);

  if (xIndex < width) {
    // Pointer shift, initialization, and max value
    p_dist = dist + xIndex;
    p_ind = ind + xIndex;
    max_dist = p_dist[0];
    p_ind[0] = 0;

    // Part 1 : sort kth firt elementZ
    for (l = 1; l < k; l++) {
      curr_row = l * width;
      curr_dist = p_dist[curr_row];
      if (curr_dist < max_dist) {
        i = l - 1;
        for (int a = 0; a < l - 1; a++) {
          if (p_dist[a * width] > curr_dist) {
            i = a;
            break;
          }
        }
        for (j = l; j > i; j--) {
          p_dist[j * width] = p_dist[(j - 1) * width];
          p_ind[j * width] = p_ind[(j - 1) * width];
        }
        p_dist[i * width] = curr_dist;
        p_ind[i * width] = l;
      } else {
        p_ind[l * width] = l;
      }
      max_dist = p_dist[curr_row];
    }

    // Part 2 : insert element in the k-th first lines
    max_row = (k - 1) * width;
    for (l = k; l < height; l++) {
      curr_dist = p_dist[l * width];
      if (curr_dist < max_dist) {
        i = k - 1;
        for (int a = 0; a < k - 1; a++) {
          if (p_dist[a * width] > curr_dist) {
            i = a;
            break;
          }
        }
        for (j = k - 1; j > i; j--) {
          p_dist[j * width] = p_dist[(j - 1) * width];
          p_ind[j * width] = p_ind[(j - 1) * width];
        }
        p_dist[i * width] = curr_dist;
        p_ind[i * width] = l;
        max_dist = p_dist[max_row];
      }
    }
  }
}

/**
  * Computes the square root of the first line (width-th first element)
  * of the distance matrix.
  *
  * @param dist    distance matrix
  * @param width   width of the distance matrix
  * @param k       number of neighbors to consider
  */
void cuParallelSqrt(float *dist, int width, int k, sycl::nd_item<3> item_ct1) {
  unsigned int xIndex =
      item_ct1.get_group(2) * item_ct1.get_local_range().get(2) +
      item_ct1.get_local_id(2);
  unsigned int yIndex =
      item_ct1.get_group(1) * item_ct1.get_local_range().get(1) +
      item_ct1.get_local_id(1);
  if (xIndex < width && yIndex < k)
    dist[yIndex * width + xIndex] = sycl::sqrt(dist[yIndex * width + xIndex]);
}

//-----------------------------------------------------------------------------------------------//
//                                   K-th NEAREST NEIGHBORS //
//-----------------------------------------------------------------------------------------------//
/**
  * K nearest neighbor algorithm
  * - Initialize CUDA
  * - Allocate device memory
  * - Copy point sets (reference and query points) from host to device memory
  * - Compute the distances + indexes to the k nearest neighbors for each query
 * point
  * - Copy distances from device to host memory
  *
  * @param ref_host      reference points ; pointer to linear matrix
  * @param ref_width     number of reference points ; width of the matrix
  * @param query_host    query points ; pointer to linear matrix
  * @param query_width   number of query points ; width of the matrix
  * @param height        dimension of points ; height of the matrices
  * @param k             number of neighbor to consider
  * @param dist_host     distances to k nearest neighbors ; pointer to linear
 * matrix
  * @param dist_host     indexes of the k nearest neighbors ; pointer to linear
 * matrix
  *
  */
void knn_parallel(float *ref_host, int ref_width, float *query_host,
                  int query_width, int height, int k, float *dist_host,
                  int *ind_host) {
  dpct::device_ext &dev_ct1 = dpct::get_current_device();
  sycl::queue &q_ct1 = dev_ct1.default_queue();

  unsigned int size_of_float = sizeof(float);
  unsigned int size_of_int = sizeof(int);

  // Variables
  float *query_dev;
  float *ref_dev;
  float *dist_dev;
  int *ind_dev;


  // Allocation of global memory for query points and for distances, CUDA_CHECK
  query_dev =
      (float *)sycl::malloc_device(query_width * height * size_of_float, q_ct1);
  dist_dev = (float *)sycl::malloc_device(
      query_width * ref_width * size_of_float, q_ct1);

  // Allocation of global memory for indexes CUDA_CHECK
  ind_dev = (int *)sycl::malloc_device(query_width * k * size_of_int, q_ct1);

  // Allocation of global memory CUDA_CHECK
  ref_dev =
      (float *)sycl::malloc_device(ref_width * height * size_of_float, q_ct1);

  q_ct1.memcpy(ref_dev, ref_host, ref_width * height * size_of_float).wait();

  // Copy of part of query actually being treated
  q_ct1.memcpy(query_dev, query_host, query_width * height * size_of_float)
      .wait();

  // Grids ans threads
  sycl::range<3> g_16x16((query_width + 15) / 16, (ref_width + 15) / 16, 1);
  sycl::range<3> t_16x16(16, 16, 1);
  //
  sycl::range<3> g_256x1((query_width + 255) / 256, 1, 1);
  sycl::range<3> t_256x1(256, 1, 1);

  sycl::range<3> g_k_16x16((query_width + 15) / 16, (k + 15) / 16, 1);
  sycl::range<3> t_k_16x16(16, 16, 1);

  // Kernel 1: Compute all the distances
  q_ct1.submit([&](sycl::handler &cgh) {
    sycl::range<2> shared_A_range_ct1(16 /*BLOCK_DIM*/, 16 /*BLOCK_DIM*/);
    sycl::range<2> shared_B_range_ct1(16 /*BLOCK_DIM*/, 16 /*BLOCK_DIM*/);

    sycl::accessor<float, 2, sycl::access::mode::read_write,
                   sycl::access::target::local>
        shared_A_acc_ct1(shared_A_range_ct1, cgh);
    sycl::accessor<float, 2, sycl::access::mode::read_write,
                   sycl::access::target::local>
        shared_B_acc_ct1(shared_B_range_ct1, cgh);
    sycl::accessor<int, 0, sycl::access::mode::read_write,
                   sycl::access::target::local>
        begin_A_acc_ct1(cgh);
    sycl::accessor<int, 0, sycl::access::mode::read_write,
                   sycl::access::target::local>
        begin_B_acc_ct1(cgh);
    sycl::accessor<int, 0, sycl::access::mode::read_write,
                   sycl::access::target::local>
        step_A_acc_ct1(cgh);
    sycl::accessor<int, 0, sycl::access::mode::read_write,
                   sycl::access::target::local>
        step_B_acc_ct1(cgh);
    sycl::accessor<int, 0, sycl::access::mode::read_write,
                   sycl::access::target::local>
        end_A_acc_ct1(cgh);

    auto dpct_global_range = g_16x16 * t_16x16;

    cgh.parallel_for(
        sycl::nd_range<3>(
            sycl::range<3>(dpct_global_range.get(2), dpct_global_range.get(1),
                           dpct_global_range.get(0)),
            sycl::range<3>(t_16x16.get(2), t_16x16.get(1), t_16x16.get(0))),
        [=](sycl::nd_item<3> item_ct1) {
          cuComputeDistanceGlobal(
              ref_dev, ref_width, query_dev, query_width, height, dist_dev,
              item_ct1,
              dpct::accessor<float, dpct::local, 2>(shared_A_acc_ct1,
                                                    shared_A_range_ct1),
              dpct::accessor<float, dpct::local, 2>(shared_B_acc_ct1,
                                                    shared_B_range_ct1),
              begin_A_acc_ct1.get_pointer(), begin_B_acc_ct1.get_pointer(),
              step_A_acc_ct1.get_pointer(), step_B_acc_ct1.get_pointer(),
              end_A_acc_ct1.get_pointer());
        });
  });

#ifdef DEBUG
  cudaMemcpy(dist_host, dist_dev, query_width * ref_width * size_of_float,
             cudaMemcpyDeviceToHost);

  for (int i = 0; i < query_width * ref_width; i++)
    printf("k1 dist: %d %f\n", i, dist_host[i]);
#endif

  // Kernel 2: Sort each column
  q_ct1.submit([&](sycl::handler &cgh) {
    auto dpct_global_range = g_256x1 * t_256x1;

    cgh.parallel_for(
        sycl::nd_range<3>(
            sycl::range<3>(dpct_global_range.get(2), dpct_global_range.get(1),
                           dpct_global_range.get(0)),
            sycl::range<3>(t_256x1.get(2), t_256x1.get(1), t_256x1.get(0))),
        [=](sycl::nd_item<3> item_ct1) {
          cuInsertionSort(dist_dev, ind_dev, query_width, ref_width, k,
                          item_ct1);
        });
  });

#ifdef DEBUG
  cudaMemcpy(dist_host, dist_dev, query_width * ref_width * size_of_float,
             cudaMemcpyDeviceToHost);

  for (int i = 0; i < query_width * ref_width; i++)
    printf("k2 dist: %d %f\n", i, dist_host[i]);

  cudaMemcpy(ind_host, ind_dev, query_width * k * size_of_int,
             cudaMemcpyDeviceToHost);
  for (int i = 0; i < query_width * k; i++)
    printf("k2 index: %d %d\n", i, ind_host[i]);
#endif

  // Kernel 3: Compute square root of k first elements
  q_ct1.submit([&](sycl::handler &cgh) {
    auto dpct_global_range = g_k_16x16 * t_k_16x16;

    cgh.parallel_for(
        sycl::nd_range<3>(sycl::range<3>(dpct_global_range.get(2),
                                         dpct_global_range.get(1),
                                         dpct_global_range.get(0)),
                          sycl::range<3>(t_k_16x16.get(2), t_k_16x16.get(1),
                                         t_k_16x16.get(0))),
        [=](sycl::nd_item<3> item_ct1) {
          cuParallelSqrt(dist_dev, query_width, k, item_ct1);
        });
  });
  dev_ct1.queues_wait_and_throw();
  // Memory copy of output from device to host
  q_ct1.memcpy(dist_host, dist_dev, query_width * k * size_of_float).wait();

  q_ct1.memcpy(ind_host, ind_dev, query_width * k * size_of_int).wait();

  // Free memory
  sycl::free(ref_dev, q_ct1);
  sycl::free(ind_dev, q_ct1);
  sycl::free(query_dev, q_ct1);
  sycl::free(dist_dev, q_ct1);
}

float compute_distance(const float *ref, int ref_nb, const float *query,
                       int query_nb, int dim, int ref_index, int query_index) {
  float sum = 0.f;
  for (int d = 0; d < dim; ++d) {
    const float diff =
        ref[d * ref_nb + ref_index] - query[d * query_nb + query_index];
    sum += diff * diff;
  }
  return sqrtf(sum);
}

void modified_insertion_sort(float *dist, int *index, int length, int k) {

  // Initialise the first index
  index[0] = 0;

  // Go through all points
  for (int i = 1; i < length; ++i) {

    // Store current distance and associated index
    float curr_dist = dist[i];
    int curr_index = i;

    // Skip the current value if its index is >= k and if it's higher the k-th
    // slready sorted mallest value
    if (i >= k && curr_dist >= dist[k - 1]) {
      continue;
    }

    // Shift values (and indexes) higher that the current distance to the right
    int j = std::min(i, k - 1);
    while (j > 0 && dist[j - 1] > curr_dist) {
      dist[j] = dist[j - 1];
      index[j] = index[j - 1];
      --j;
    }

    // Write the current distance and index at their position
    dist[j] = curr_dist;
    index[j] = curr_index;
  }
}

bool knn_c(const float *ref, int ref_nb, const float *query, int query_nb,
           int dim, int k, float *knn_dist, int *knn_index) {
  // Allocate local array to store all the distances / indexes for a given query
  // point
  float *dist = (float *)malloc(ref_nb * sizeof(float));
  int *index = (int *)malloc(ref_nb * sizeof(int));

  // Allocation checks
  if (!dist || !index) {
    printf("Memory allocation error\n");
    free(dist);
    free(index);
    return false;
  }

  // Process one query point at the time
  for (int i = 0; i < query_nb; ++i) {

    // Compute all distances / indexes
    for (int j = 0; j < ref_nb; ++j) {
      dist[j] = compute_distance(ref, ref_nb, query, query_nb, dim, j, i);
      index[j] = j;
    }

    // Sort distances / indexes
    modified_insertion_sort(dist, index, ref_nb, k);

    // Copy k smallest distances and their associated index
    for (int j = 0; j < k; ++j) {
      knn_dist[j * query_nb + i] = dist[j];
      knn_index[j * query_nb + i] = index[j];
    }
  }

  // Memory clean-up
  free(dist);
  free(index);
  return true;
}

/**
  * Example of use of kNN search CUDA.
  */
int main(void) {
  // Variables and parameters
  float *ref;          // Pointer to reference point array
  float *query;        // Pointer to query point array
  float *dist;         // Pointer to distance array
  int *ind;            // Pointer to index array
  int ref_nb = 4096;   // Reference point number, max=65535
  int query_nb = 4096; // Query point number,     max=65535
  int dim = 68;        // Dimension of points
  int k = 20;          // Nearest neighbors to consider
  int iterations = 100;
  int c_iterations = 1;
  int i;
  const float precision = 0.001f; // distance error max
  int nb_correct_precisions = 0;
  int nb_correct_indexes = 0;
  // Memory allocation
  ref = (float *)malloc(ref_nb * dim * sizeof(float));
  query = (float *)malloc(query_nb * dim * sizeof(float));
  dist = (float *)malloc(query_nb * k * sizeof(float));
  ind = (int *)malloc(query_nb * k * sizeof(float));

  // Init
  srand(2);
  for (i = 0; i < ref_nb * dim; i++)
    ref[i] = (float)rand() / (float)RAND_MAX;
  for (i = 0; i < query_nb * dim; i++)
    query[i] = (float)rand() / (float)RAND_MAX;


  // Display informations
  printf("Number of reference points      : %6d\n", ref_nb);
  printf("Number of query points          : %6d\n", query_nb);
  printf("Dimension of points             : %4d\n", dim);
  printf("Number of neighbors to consider : %4d\n", k);
  printf("Processing kNN search           :\n");

  float *knn_dist = (float *)malloc(query_nb * k * sizeof(float));
  int *knn_index = (int *)malloc(query_nb * k * sizeof(int));
  printf("Ground truth computation in progress...\n\n");
  if (!knn_c(ref, ref_nb, query, query_nb, dim, k, knn_dist, knn_index)) {
    free(ref);
    free(query);
    free(knn_dist);
    free(knn_index);
    return EXIT_FAILURE;
  }

  struct timeval tic;
  struct timeval toc;
  float elapsed_time;

  printf("On CPU: \n");
  gettimeofday(&tic, NULL);
  for (i = 0; i < c_iterations; i++) {
    knn_c(ref, ref_nb, query, query_nb, dim, k, dist, ind);
  }
  gettimeofday(&toc, NULL);
  elapsed_time = toc.tv_sec - tic.tv_sec;
  elapsed_time += (toc.tv_usec - tic.tv_usec) / 1000000.;
  printf(" done in %f s for %d iterations (%f s by iteration)\n", elapsed_time,
         c_iterations, elapsed_time / (c_iterations));

  printf("on GPU: \n");
  gettimeofday(&tic, NULL);
  for (i = 0; i < iterations; i++) {
    knn_parallel(ref, ref_nb, query, query_nb, dim, k, dist, ind);
  }
  gettimeofday(&toc, NULL);
  elapsed_time = toc.tv_sec - tic.tv_sec;
  elapsed_time += (toc.tv_usec - tic.tv_usec) / 1000000.;
  printf(" done in %f s for %d iterations (%f s by iteration)\n", elapsed_time,
         iterations, elapsed_time / (iterations));

  for (int i = 0; i < query_nb * k; ++i) {
    if (fabs(dist[i] - knn_dist[i]) <= precision) {
      nb_correct_precisions++;
    }
    if (ind[i] == knn_index[i]) {
      nb_correct_indexes++;
    } else {
      printf("Mismatch @%d: %d %d\n", i, ind[i], knn_index[i]);
    }
  }

  float precision_accuracy = nb_correct_precisions / ((float)query_nb * k);
  float index_accuracy = nb_correct_indexes / ((float)query_nb * k);
  printf("Precision accuracy %f\nIndex accuracy %f\n", precision_accuracy, index_accuracy);

  free(ind);
  free(dist);
  free(query);
  free(ref);
}
