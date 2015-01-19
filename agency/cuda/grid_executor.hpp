#pragma once

#include <agency/execution_categories.hpp>
#include <future>
#include <memory>
#include <iostream>
#include <exception>
#include <cstring>
#include <type_traits>
#include <cassert>
#include <thrust/system_error.h>
#include <thrust/system/cuda/error.h>
#include <agency/flattened_executor.hpp>
#include <agency/detail/ignore.hpp>
#include <thrust/detail/minmax.h>
#include <agency/cuda/detail/tuple.hpp>
#include <agency/cuda/detail/feature_test.hpp>
#include <agency/cuda/gpu.hpp>
#include <agency/cuda/detail/bind.hpp>
#include <agency/cuda/detail/unique_ptr.hpp>
#include <agency/cuda/detail/terminate.hpp>
#include <agency/cuda/detail/uninitialized.hpp>
#include <agency/cuda/detail/launch_kernel.hpp>
#include <agency/cuda/detail/workaround_unused_variable_warning.hpp>
#include <agency/coordinate.hpp>
#include <agency/detail/shape_cast.hpp>
#include <agency/detail/index_tuple.hpp>


namespace agency
{
namespace cuda
{
namespace detail
{


template<class Function, class OuterSharedType, class InnerSharedType>
struct function_with_shared_arguments
{
  __host__ __device__
  function_with_shared_arguments(Function f, OuterSharedType* outer_ptr, InnerSharedType inner_shared_init)
    : f_(f),
      outer_ptr_(outer_ptr),
      inner_shared_init_(inner_shared_init)
  {}

  __device__
  void operator()(const agency::uint2 idx)
  {
    // XXX can't rely on a default constructor
    __shared__ uninitialized<InnerSharedType> inner_param;

    // initialize the inner shared parameter
    if(idx[1] == 0)
    {
      inner_param.construct(inner_shared_init_);
    }
    __syncthreads();

    tuple<OuterSharedType&,InnerSharedType&> shared_params(*outer_ptr_, inner_param);

    f_(idx, shared_params);

    __syncthreads();

    // destroy the inner shared parameter
    if(idx[1] == 0)
    {
      inner_param.destroy();
    }
  }

  Function         f_;
  OuterSharedType* outer_ptr_;
  InnerSharedType  inner_shared_init_;
};


template<class Function, class InnerSharedType>
struct function_with_shared_arguments<Function,agency::detail::ignore_t,InnerSharedType>
{
  __host__ __device__
  function_with_shared_arguments(Function f, agency::detail::ignore_t, InnerSharedType inner_shared_init)
    : f_(f),
      inner_shared_init_(inner_shared_init)
  {}

  __device__
  void operator()(const agency::uint2 idx)
  {
    // XXX can't rely on a default constructor
    __shared__ uninitialized<InnerSharedType> inner_param;

    // initialize the inner shared parameter
    if(idx[1] == 0)
    {
      inner_param.construct(inner_shared_init_);
    }
    __syncthreads();

    agency::detail::ignore_t ignore;
    tuple<agency::detail::ignore_t&,InnerSharedType&> shared_params(ignore, inner_param);

    f_(idx, shared_params);

    __syncthreads();

    // destroy the inner shared parameter
    if(idx[1] == 0)
    {
      inner_param.destroy();
    }
  }

  Function         f_;
  InnerSharedType  inner_shared_init_;
};


template<class Function, class OuterSharedType>
struct function_with_shared_arguments<Function,OuterSharedType,agency::detail::ignore_t>
{
  __host__ __device__
  function_with_shared_arguments(Function f, OuterSharedType* outer_ptr, agency::detail::ignore_t)
    : f_(f),
      outer_ptr_(outer_ptr)
  {}

  __device__
  void operator()(const agency::uint2 idx)
  {
    agency::detail::ignore_t ignore;
    tuple<OuterSharedType&,agency::detail::ignore_t&> shared_params(*outer_ptr_, ignore);

    f_(idx, shared_params);
  }

  Function         f_;
  OuterSharedType* outer_ptr_;
};


template<class ThisIndexFunction, class Function>
__global__ void grid_executor_kernel(Function f)
{
  f(ThisIndexFunction{}());
}


void grid_executor_notify(cudaStream_t stream, cudaError_t status, void* data)
{
  std::unique_ptr<std::promise<void>> promise(reinterpret_cast<std::promise<void>*>(data));

  promise->set_value();
}


template<class Shape, class Index, class ThisIndexFunction>
class basic_grid_executor
{
  public:
    using execution_category =
      nested_execution_tag<
        parallel_execution_tag,
        concurrent_execution_tag
      >;


    using shape_type = Shape;


    using index_type = Index;


    template<class Tuple>
    using shared_param_type = detail::tuple_of_references_t<Tuple>;


    __host__ __device__
    explicit basic_grid_executor(int shared_memory_size = 0, cudaStream_t stream = 0, gpu_id gpu = detail::current_gpu())
      : shared_memory_size_(shared_memory_size),
        stream_(stream),
        gpu_(gpu)
    {}


    __host__ __device__
    int shared_memory_size() const
    {
      return shared_memory_size_;
    }


    __host__ __device__
    cudaStream_t stream() const
    {
      return stream_; 
    }


    __host__ __device__
    gpu_id gpu() const
    {
      return gpu_;
    }


    template<class Function>
    std::future<void> bulk_async(Function f, shape_type shape)
    {
      launch(f, shape);

      void* kernel = reinterpret_cast<void*>(global_function_pointer<Function>());

      // XXX unique_ptr & promise won't be valid in __device__ code
      std::unique_ptr<std::promise<void>> promise(new std::promise<void>());
    
      auto result = promise->get_future();
    
      // call __notify when kernel is finished
      // XXX cudaStreamAddCallback probably isn't valid in __device__ code
      detail::throw_on_error(cudaStreamAddCallback(stream(), detail::grid_executor_notify, promise.release(), 0),
                             "cuda::grid_executor::bulk_async(): cudaStreamAddCallback");
    
      return result;
    }

  private:
    // case where we have actual inner & outer shared parameters 
    template<class Function, class T1, class T2>
    std::future<void> bulk_async_with_shared_args(Function f, shape_type shape, const T1& outer_shared_arg, const T2& inner_shared_arg)
    {
      // make outer shared argument
      auto outer_shared_arg_ptr = detail::make_unique<T1>(stream(), outer_shared_arg);

      // wrap up f in a thing that will marshal the shared arguments to it
      // note the .release()
      auto g = detail::function_with_shared_arguments<Function, T1, T2>(f, outer_shared_arg_ptr.release(), inner_shared_arg);

      // XXX to deallocate & destroy the outer_shared_arg, we need to do a bulk_async(...).then(...)
      //     for now it just leaks :(

      return bulk_async(g, shape);
    }

    // case where we have only inner shared parameter
    template<class Function, class T>
    std::future<void> bulk_async_with_shared_args(Function f, shape_type shape, agency::detail::ignore_t ignore, const T& inner_shared_arg)
    {
      // wrap up f in a thing that will marshal the shared arguments to it
      auto g = detail::function_with_shared_arguments<Function, agency::detail::ignore_t, T>(f, ignore, inner_shared_arg);

      return bulk_async(g, shape);
    }

    // case where we have only outer shared parameter
    template<class Function, class T>
    std::future<void> bulk_async_with_shared_args(Function f, shape_type shape, const T& outer_shared_arg, agency::detail::ignore_t ignore)
    {
      // make outer shared argument
      auto outer_shared_arg_ptr = detail::make_unique<T>(stream(), outer_shared_arg);

      // wrap up f in a thing that will marshal the shared arguments to it
      // note the .release()
      auto g = detail::function_with_shared_arguments<Function, T, agency::detail::ignore_t>(f, outer_shared_arg_ptr.release(), ignore);

      // XXX to deallocate & destroy the outer_shared_arg, we need to do a bulk_async(...).then(...)
      //     for now it just leaks :(

      return bulk_async(g, shape);
    }

    // case where we have no actual shared parameters
    template<class Function>
    std::future<void> bulk_async_with_shared_args(Function f, shape_type shape, agency::detail::ignore_t, agency::detail::ignore_t)
    {
      return bulk_async(f, shape);
    }

  public:
    template<class Function, class Tuple>
    std::future<void> bulk_async(Function f, shape_type shape, Tuple shared_arg_tuple)
    {
      auto outer_shared_arg = agency::detail::get<0>(shared_arg_tuple);
      auto inner_shared_arg = agency::detail::get<1>(shared_arg_tuple);

      return bulk_async_with_shared_args(f, shape, outer_shared_arg, inner_shared_arg);
    }


    template<class Function>
    __host__ __device__
    void bulk_invoke(Function f, shape_type shape)
    {
      // XXX bulk_invoke() needs to be implemented with a lowering to bulk_async
      //     do this when we switch to executor-specific futures instead of std::future
#ifndef __CUDA_ARCH__
      bulk_async(f, shape).wait();
#else
      launch(f, shape);

#  if __cuda_lib_has_cudart
      detail::throw_on_error(cudaDeviceSynchronize(), "cuda::grid_executor::bulk_invoke(): cudaDeviceSynchronize");
#  endif
#endif
    }


  private:
    // case where we have actual inner & outer shared parameters 
    template<class Function, class T1, class T2>
    __host__ __device__
    void bulk_invoke_with_shared_args(Function f, shape_type shape, const T1& outer_shared_arg, const T2& inner_shared_arg)
    {
      // make outer shared argument
      auto outer_shared_arg_ptr = detail::make_unique<T1>(stream(), outer_shared_arg);

      // wrap up f in a thing that will marshal the shared arguments to it
      auto g = detail::function_with_shared_arguments<Function, T1, T2>(f, outer_shared_arg_ptr.get(), inner_shared_arg);

      return bulk_invoke(g, shape);
    }

    // case where we have only inner shared parameter
    template<class Function, class T>
    __host__ __device__
    void bulk_invoke_with_shared_args(Function f, shape_type shape, agency::detail::ignore_t ignore, const T& inner_shared_arg)
    {
      // wrap up f in a thing that will marshal the shared arguments to it
      auto g = detail::function_with_shared_arguments<Function, agency::detail::ignore_t, T>(f, ignore, inner_shared_arg);

      return bulk_invoke(g, shape);
    }

    // case where we have only outer shared parameter
    template<class Function, class T>
    __host__ __device__
    void bulk_invoke_with_shared_args(Function f, shape_type shape, const T& outer_shared_arg, agency::detail::ignore_t ignore)
    {
      // make outer shared argument
      auto outer_shared_arg_ptr = detail::make_unique<T>(stream(), outer_shared_arg);

      // wrap up f in a thing that will marshal the shared arguments to it
      auto g = detail::function_with_shared_arguments<Function, T, agency::detail::ignore_t>(f, outer_shared_arg_ptr.get(), ignore);

      return bulk_invoke(g, shape);
    }

    // case where we have no actual shared parameters
    template<class Function>
    __host__ __device__
    void bulk_invoke_with_shared_args(Function f, shape_type shape, agency::detail::ignore_t, agency::detail::ignore_t)
    {
      return bulk_invoke(f, shape);
    }


  public:
    template<class Function, class Tuple>
    __host__ __device__
    void bulk_invoke(Function f, shape_type shape, Tuple shared_arg_tuple)
    {
      // XXX bulk_invoke() needs to be implemented with a lowering to bulk_async
      //     do this when we switch to executor-specific futures instead of std::future

      auto outer_shared_arg = agency::detail::get<0>(shared_arg_tuple);
      auto inner_shared_arg = agency::detail::get<1>(shared_arg_tuple);

      return bulk_invoke_with_shared_args(f, shape, outer_shared_arg, inner_shared_arg);
    }

    // this is exposed because it's necessary if a client wants to compute occupancy
    // alternatively, cuda_executor could report occupancy of a Function for a given block size
    template<class Function>
    __host__ __device__
    static decltype(&detail::grid_executor_kernel<ThisIndexFunction,Function>) global_function_pointer()
    {
      return &detail::grid_executor_kernel<ThisIndexFunction, Function>;
    }


  private:
    template<class Function>
    __host__ __device__
    void launch(Function f, shape_type shape)
    {
      launch(f, shape, shared_memory_size());
    }

    template<class Function>
    __host__ __device__
    void launch(Function f, shape_type shape, int shared_memory_size)
    {
      launch(f, shape, shared_memory_size, stream());
    }

    template<class Function>
    __host__ __device__
    void launch(Function f, shape_type shape, int shared_memory_size, cudaStream_t stream)
    {
      launch(f, shape, shared_memory_size, stream, gpu());
    }

    template<class Function>
    __host__ __device__
    void launch(Function f, shape_type shape, int shared_memory_size, cudaStream_t stream, gpu_id gpu)
    {
      void* kernel = reinterpret_cast<void*>(global_function_pointer<Function>());

      uint3 outer_shape = agency::detail::shape_cast<uint3>(agency::detail::get<0>(shape));
      uint3 inner_shape = agency::detail::shape_cast<uint3>(agency::detail::get<1>(shape));

      ::dim3 grid_dim{outer_shape[0], outer_shape[1], outer_shape[2]};
      ::dim3 block_dim{inner_shape[0], inner_shape[1], inner_shape[2]};

      detail::checked_launch_kernel_on_device(kernel, grid_dim, block_dim, shared_memory_size, stream, gpu.native_handle(), f);
    }

    int shared_memory_size_;
    cudaStream_t stream_;
    gpu_id gpu_;
};


struct this_index_1d
{
  __device__
  agency::uint2 operator()() const
  {
    return agency::uint2{blockIdx.x, threadIdx.x};
  }
};


struct this_index_2d
{
  __device__
  agency::point<agency::uint2,2> operator()() const
  {
    auto block = agency::uint2{blockIdx.x, blockIdx.y};
    auto thread = agency::uint2{threadIdx.x, threadIdx.y};
    return agency::point<agency::uint2,2>{block, thread};
  }
};


} // end detail


class grid_executor : public detail::basic_grid_executor<agency::uint2, agency::uint2, detail::this_index_1d>
{
  public:
    using detail::basic_grid_executor<agency::uint2, agency::uint2, detail::this_index_1d>::basic_grid_executor;

    template<class Function>
    __host__ __device__
    shape_type max_shape(Function) const
    {
      shape_type result = {0,0};

      auto fun_ptr = global_function_pointer<Function>();
      detail::workaround_unused_variable_warning(fun_ptr);

#if __cuda_lib_has_cudart
      // record the current device
      int current_device = 0;
      detail::throw_on_error(cudaGetDevice(&current_device), "cuda::grid_executor::max_shape(): cudaGetDevice()");
      if(current_device != gpu().native_handle())
      {
#  ifndef __CUDA_ARCH__
        detail::throw_on_error(cudaSetDevice(gpu().native_handle()), "cuda::grid_executor::max_shape(): cudaSetDevice()");
#  else
        detail::throw_on_error(cudaErrorNotSupported, "cuda::grid_executor::max_shape(): cudaSetDevice only allowed in __host__ code");
#  endif // __CUDA_ARCH__
      }

      int max_block_dimension_x = 0;
      detail::throw_on_error(cudaDeviceGetAttribute(&max_block_dimension_x, cudaDevAttrMaxBlockDimX, gpu().native_handle()),
                             "cuda::grid_executor::max_shape(): cudaDeviceGetAttribute");

      cudaFuncAttributes attr{};
      detail::throw_on_error(cudaFuncGetAttributes(&attr, fun_ptr),
                             "cuda::grid_executor::max_shape(): cudaFuncGetAttributes");

      // restore current device
      if(current_device != gpu().native_handle())
      {
#  ifndef __CUDA_ARCH__
        detail::throw_on_error(cudaSetDevice(current_device), "cuda::grid_executor::max_shape(): cudaSetDevice()");
#  else
        detail::throw_on_error(cudaErrorNotSupported, "cuda::grid_executor::max_shape(): cudaSetDevice only allowed in __host__ code");
#  endif // __CUDA_ARCH__
      }

      result = shape_type{static_cast<unsigned int>(max_block_dimension_x), static_cast<unsigned int>(attr.maxThreadsPerBlock)};
#endif // __cuda_lib_has_cudart

      return result;
    }
};


template<class Function, class... Args>
__host__ __device__
void bulk_invoke(grid_executor& ex, typename grid_executor::shape_type shape, Function&& f, Args&&... args)
{
  auto g = detail::bind(std::forward<Function>(f), thrust::placeholders::_1, std::forward<Args>(args)...);
  ex.bulk_invoke(g, shape);
}


class grid_executor_2d : public detail::basic_grid_executor<
  point<agency::uint2,2>,
  point<agency::uint2,2>,
  detail::this_index_2d
>
{
  public:
    using detail::basic_grid_executor<
      point<agency::uint2,2>,
      point<agency::uint2,2>,
      detail::this_index_2d
    >::basic_grid_executor;

    // XXX implement max_shape()
};


template<class Function, class... Args>
__host__ __device__
void bulk_invoke(grid_executor_2d& ex, typename grid_executor_2d::shape_type shape, Function&& f, Args&&... args)
{
  auto g = detail::bind(std::forward<Function>(f), thrust::placeholders::_1, std::forward<Args>(args)...);
  ex.bulk_invoke(g, shape);
}


namespace detail
{


template<class Function>
struct flattened_grid_executor_functor
{
  Function f_;
  std::size_t shape_;
  cuda::grid_executor::shape_type partitioning_;

  __host__ __device__
  flattened_grid_executor_functor(const Function& f, std::size_t shape, cuda::grid_executor::shape_type partitioning)
    : f_(f),
      shape_(shape),
      partitioning_(partitioning)
  {}

  template<class T>
  __device__
  void operator()(cuda::grid_executor::index_type idx, T&& shared_params)
  {
    auto flat_idx = agency::detail::get<0>(idx) * agency::detail::get<1>(partitioning_) + agency::detail::get<1>(idx);

    if(flat_idx < shape_)
    {
      f_(flat_idx, agency::detail::get<0>(shared_params));
    }
  }

  inline __device__
  void operator()(cuda::grid_executor::index_type idx)
  {
    auto flat_idx = agency::detail::get<0>(idx) * agency::detail::get<1>(partitioning_) + agency::detail::get<1>(idx);

    if(flat_idx < shape_)
    {
      f_(flat_idx);
    }
  }
};


} // end detail
} // end cuda


// specialize agency::flattened_executor<grid_executor>
// to add __host__ __device__ to its functions and avoid lambdas
template<>
class flattened_executor<cuda::grid_executor>
{
  public:
    using execution_category = parallel_execution_tag;
    using base_executor_type = cuda::grid_executor;

    // XXX maybe use whichever of the first two elements of base_executor_type::shape_type has larger dimensionality?
    using shape_type = size_t;

    // XXX initialize outer_subscription_ correctly
    __host__ __device__
    flattened_executor(const base_executor_type& base_executor = base_executor_type())
      : outer_subscription_(2),
        base_executor_(base_executor)
    {}

    template<class Function>
    std::future<void> bulk_async(Function f, shape_type shape)
    {
      // create a dummy function for partitioning purposes
      auto dummy_function = cuda::detail::flattened_grid_executor_functor<Function>{f, shape, partition_type{}};

      // partition up the iteration space
      auto partitioning = partition(dummy_function, shape);

      // create a function to execute
      auto execute_me = cuda::detail::flattened_grid_executor_functor<Function>{f, shape, partitioning};

      return base_executor().bulk_async(execute_me, partitioning);
    }

    template<class Function, class T>
    std::future<void> bulk_async(Function f, shape_type shape, T shared_arg)
    {
      // create a dummy function for partitioning purposes
      auto dummy_function = cuda::detail::flattened_grid_executor_functor<Function>{f, shape, partition_type{}};

      // partition up the iteration space
      auto partitioning = partition(dummy_function, shape);

      // create a shared initializer
      auto shared_init = agency::detail::make_tuple(shared_arg, agency::detail::ignore);
      using shared_param_type = typename executor_traits<base_executor_type>::template shared_param_type<decltype(shared_init)>;

      // create a function to execute
      auto execute_me = cuda::detail::flattened_grid_executor_functor<Function>{f, shape, partitioning};

      return base_executor().bulk_async(execute_me, partitioning, shared_init);
    }

    __host__ __device__
    const base_executor_type& base_executor() const
    {
      return base_executor_;
    }

    __host__ __device__
    base_executor_type& base_executor()
    {
      return base_executor_;
    }

  private:
    using partition_type = typename executor_traits<base_executor_type>::shape_type;

    // returns (outer size, inner size)
    template<class Function>
    __host__ __device__
    partition_type partition(Function f, shape_type shape) const
    {
      using outer_shape_type = typename std::tuple_element<0,partition_type>::type;
      using inner_shape_type = typename std::tuple_element<1,partition_type>::type;

      auto max_shape = base_executor().max_shape(f);

      // make the inner groups as large as possible
      inner_shape_type inner_size = agency::detail::get<1>(max_shape);

      outer_shape_type outer_size = (shape + inner_size - 1) / inner_size;

      assert(outer_size <= agency::detail::get<0>(max_shape));

      return partition_type{outer_size, inner_size};
    }

    size_t min_inner_size_;
    size_t outer_subscription_;
    base_executor_type base_executor_;
};


} // end agency

