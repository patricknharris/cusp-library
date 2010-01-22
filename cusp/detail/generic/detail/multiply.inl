/*
 *  Copyright 2008-2009 NVIDIA Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <cusp/coo_matrix.h>
#include <cusp/array2d.h>

#include <cusp/detail/format_utils.h>

#include <thrust/gather.h>
#include <thrust/scan.h>
#include <thrust/segmented_scan.h>
#include <thrust/scatter.h>
#include <thrust/sort.h>
#include <thrust/transform.h>
#include <thrust/unique.h>

namespace cusp
{
namespace detail
{
namespace generic
{

template <typename IndexType,
          typename ValueType,
          typename SpaceOrAlloc>
void multiply(const cusp::coo_matrix<IndexType,ValueType,SpaceOrAlloc>& A,
              const cusp::coo_matrix<IndexType,ValueType,SpaceOrAlloc>& B,
                    cusp::coo_matrix<IndexType,ValueType,SpaceOrAlloc>& C)
{
    typedef typename cusp::coo_matrix<IndexType,ValueType,SpaceOrAlloc>::memory_space MemorySpace;

    // check whether matrices are empty
    if (A.num_entries == 0 || B.num_entries == 0)
    {
        cusp::coo_matrix<IndexType,ValueType,SpaceOrAlloc> temp(A.num_rows, B.num_cols, 0);
        C.swap(temp);
        return;
    }

    // compute row offsets for B
    cusp::array1d<IndexType,MemorySpace> B_row_offsets(B.num_rows + 1);
    cusp::detail::indices_to_offsets(B.row_indices, B_row_offsets);

    // compute row lengths for B
    cusp::array1d<IndexType,MemorySpace> B_row_lengths(B.num_rows);
    thrust::transform(B_row_offsets.begin() + 1, B_row_offsets.end(), B_row_offsets.begin(), B_row_lengths.begin(), thrust::minus<IndexType>());

    // for each element A(i,j) compute the number of nonzero elements in B(j,:)
    cusp::array1d<IndexType,MemorySpace> segment_lengths(A.num_entries);
    thrust::gather(segment_lengths.begin(), segment_lengths.end(),
                   A.column_indices.begin(),
                   B_row_lengths.begin());
    
    // output pointer
    cusp::array1d<IndexType,MemorySpace> output_ptr(A.num_entries + 1);
    thrust::exclusive_scan(segment_lengths.begin(), segment_lengths.end(),
                           output_ptr.begin(),
                           IndexType(0));
    output_ptr[A.num_entries] = output_ptr[A.num_entries - 1] + segment_lengths[A.num_entries - 1]; // XXX is this necessary?

    IndexType coo_num_nonzeros = output_ptr[A.num_entries];
    
    // enumerate the segments in the intermediate format corresponding to each entry A(i,j)
    // XXX could be done with offset_to_index instead
    cusp::array1d<IndexType,MemorySpace> segments(coo_num_nonzeros, 0);
    thrust::scatter_if(thrust::counting_iterator<IndexType>(0), thrust::counting_iterator<IndexType>(A.num_entries),
                       output_ptr.begin(), 
                       segment_lengths.begin(),
                       segments.begin());
    thrust::inclusive_scan(segments.begin(), segments.end(), segments.begin(), thrust::maximum<IndexType>());
   
    // compute gather locations of intermediate format
    cusp::array1d<IndexType,MemorySpace> gather_locations(coo_num_nonzeros, 1);
    {
        // TODO replace temp arrays with permutation_iterator
        // TODO fuse two calls to scatter_if with zip_iterator
        cusp::array1d<IndexType,MemorySpace> temp(A.num_entries);  // B_row_offsets[Aj[n]]

        thrust::gather(temp.begin(), temp.end(),
                       A.column_indices.begin(),    
                       B_row_offsets.begin());

        thrust::scatter_if(temp.begin(), temp.end(),
                           output_ptr.begin(),
                           segment_lengths.begin(),
                           gather_locations.begin());
    }
    thrust::experimental::inclusive_segmented_scan(gather_locations.begin(), gather_locations.end(),
                                                   segments.begin(),
                                                   gather_locations.begin());
    
    // compute column entries and values of intermediate format
    cusp::array1d<IndexType,MemorySpace> I(coo_num_nonzeros);
    cusp::array1d<IndexType,MemorySpace> J(coo_num_nonzeros);
    cusp::array1d<ValueType,MemorySpace> V(coo_num_nonzeros);
    
    thrust::gather(I.begin(), I.end(),
                   segments.begin(),
                   A.row_indices.begin());

    thrust::gather(J.begin(), J.end(),
                   gather_locations.begin(),
                   B.column_indices.begin());
    {
        // TODO replace temp arrays with permutation_iterator
        cusp::array1d<ValueType,MemorySpace> temp1(coo_num_nonzeros);  // A_values[segments[n]]
        cusp::array1d<ValueType,MemorySpace> temp2(coo_num_nonzeros);  // B_values[gather_locations[n]]

        thrust::gather(temp1.begin(), temp1.end(), segments.begin(),         A.values.begin());
        thrust::gather(temp2.begin(), temp2.end(), gather_locations.begin(), B.values.begin());

        thrust::transform(temp1.begin(), temp1.end(),
                          temp2.begin(),
                          V.begin(),
                          thrust::multiplies<ValueType>());
    }

    // sort by (I,J)
    {
        // TODO use explicit permuation array
        thrust::sort_by_key(J.begin(), J.end(), thrust::make_zip_iterator(thrust::make_tuple(I.begin(), V.begin())));
        thrust::sort_by_key(I.begin(), I.end(), thrust::make_zip_iterator(thrust::make_tuple(J.begin(), V.begin())));
    }

    // compress duplicate (I,J) entries
    size_t NNZ = thrust::unique_by_key(thrust::make_zip_iterator(thrust::make_tuple(I.begin(), J.begin())),
                                       thrust::make_zip_iterator(thrust::make_tuple(I.end(), J.end())),
                                       V.begin(),
                                       thrust::equal_to< thrust::tuple<IndexType,IndexType> >(),
                                       thrust::plus<ValueType>()).second - V.begin();
    I.resize(NNZ);
    J.resize(NNZ);
    V.resize(NNZ);

    C.resize(A.num_rows, B.num_cols, NNZ);
    C.row_indices    = I;
    C.column_indices = J;
    C.values         = V;
}


template <typename ValueType,
          typename SpaceOrAlloc>
void multiply(const cusp::array2d<ValueType,SpaceOrAlloc>& A,
              const cusp::array2d<ValueType,SpaceOrAlloc>& B,
                    cusp::array2d<ValueType,SpaceOrAlloc>& C)
{
    C.resize(A.num_rows, B.num_cols);

    for(size_t i = 0; i < C.num_rows; i++)
    {
        for(size_t j = 0; j < C.num_cols; j++)
        {
            ValueType v = 0;

            for(size_t k = 0; k < A.num_cols; k++)
                v += A(i,k) * B(k,j);
            
            C(i,j) = v;
        }
    }
}

} // end namespace generic
} // end namespace detail
} // end namespace cusp

