#pragma once

#include <type_traits>
#include <agency/detail/invoke.hpp>
#include <agency/future.hpp>

// this type falls into no executor categories
struct not_an_executor {};

class bulk_continuation_executor
{
  public:
    template<class Function, class Future, class ResultFactory, class SharedFactory>
    std::future<
      typename std::result_of<ResultFactory(size_t)>::type
    >
    bulk_then_execute(Function f, size_t n, Future& predecessor, ResultFactory result_factory, SharedFactory shared_factory)
    {
      return bulk_then_execute_impl(f, n, predecessor, result_factory, shared_factory);
    }

  private:
    template<class Function, class Future, class ResultFactory, class SharedFactory>
    std::future<agency::detail::result_of_t<ResultFactory(size_t)>>
      bulk_then_execute_impl(Function f, size_t n, Future& predecessor, ResultFactory result_factory, SharedFactory shared_factory,
                             typename std::enable_if<
                               !std::is_void<
                                 typename agency::future_traits<Future>::value_type
                               >::value
                             >::type* = 0)
    {
      if(n > 0)
      {
        using predecessor_type = typename agency::future_traits<Future>::value_type;

        return agency::detail::monadic_then(predecessor, std::launch::async, [=](predecessor_type& predecessor) mutable
        {
          // put all the shared parameters on the first thread's stack
          auto result = result_factory(n);
          auto shared_parameter = shared_factory(n);

          // create a lambda to handle parameter passing
          auto g = [&,f](size_t idx)
          {
            agency::detail::invoke(f, idx, predecessor, result, shared_parameter);
          };

          size_t mid = n / 2;

          std::future<void> left = agency::detail::make_ready_future();
          if(0 < mid)
          {
            left = this->async(g, 0, mid);
          }

          std::future<void> right = agency::detail::make_ready_future();
          if(mid + 1 < n)
          {
            right = this->async(g, mid + 1, n);
          }

          g(mid);

          left.wait();
          right.wait();

          return std::move(result);
        });
      }

      return agency::detail::make_ready_future(result_factory(n));
    }

    template<class Function, class Future, class ResultFactory, class SharedFactory>
    std::future<agency::detail::result_of_t<ResultFactory(size_t)>>
      bulk_then_execute_impl(Function f, size_t n, Future& predecessor, ResultFactory result_factory, SharedFactory shared_factory,
                             typename std::enable_if<
                               std::is_void<
                                 typename agency::future_traits<Future>::value_type
                               >::value
                             >::type* = 0)
    {
      if(n > 0)
      {
        return agency::detail::monadic_then(predecessor, std::launch::async, [=]() mutable
        {
          // put all the shared parameters on the first thread's stack
          auto result = result_factory(n);
          auto shared_parameter = shared_factory(n);

          // create a lambda to handle parameter passing
          auto g = [&,f](size_t idx)
          {
            agency::detail::invoke(f, idx, result, shared_parameter);
          };

          size_t mid = n / 2;

          std::future<void> left = agency::detail::make_ready_future();
          if(0 < mid)
          {
            left = this->async(g, 0, mid);
          }

          std::future<void> right = agency::detail::make_ready_future();
          if(mid + 1 < n)
          {
            right = this->async(g, mid + 1, n);
          }

          g(mid);

          left.wait();
          right.wait();

          return std::move(result);
        });
      }

      return agency::detail::make_ready_future(result_factory(n));
    }

    // first must be less than last
    template<class Function>
    std::future<void> async(Function f, size_t first, size_t last)
    {
      return std::async(std::launch::async, [=]() mutable
      {
        size_t mid = (last + first) / 2;

        std::future<void> left = agency::detail::make_ready_future();
        if(first < mid)
        {
          left = this->async(f, first, mid);
        }

        std::future<void> right = agency::detail::make_ready_future();
        if(mid + 1 < last)
        {
          right = this->async(f, mid + 1, last);
        }

        agency::detail::invoke(f,mid);

        left.wait();
        right.wait();
      });
    }
};


class bulk_synchronous_executor
{
  public:
    template<class Function, class ResultFactory, class SharedFactory>
    typename std::result_of<ResultFactory(size_t)>::type
    bulk_execute(Function f, size_t n, ResultFactory result_factory, SharedFactory shared_factory)
    {
      auto result = result_factory(n);
      auto shared_parm = shared_factory(n);

      for(size_t i = 0; i < n; ++i)
      {
        f(i, result, shared_parm);
      }

      return std::move(result);
    }
};


class bulk_asynchronous_executor
{
  public:
    template<class Function, class ResultFactory, class SharedFactory>
    std::future<
      typename std::result_of<ResultFactory(size_t)>::type
    >
    bulk_async_execute(Function f, size_t n, ResultFactory result_factory, SharedFactory shared_factory)
    {
      return std::async(std::launch::async, [=]
      {
        auto result = result_factory(n);
        auto shared_parm = shared_factory(n);

        for(size_t i = 0; i < n; ++i)
        {
          f(i, result, shared_parm);
        }

        return std::move(result);
      });
    }
};


// these executor types fall into two categories
struct not_a_bulk_synchronous_executor : bulk_asynchronous_executor, bulk_continuation_executor {};
struct not_a_bulk_asynchronous_executor : bulk_synchronous_executor, bulk_continuation_executor {};
struct not_a_bulk_continuation_executor : bulk_synchronous_executor, bulk_asynchronous_executor {};


// this executor type falls into three categories
struct complete_bulk_executor : bulk_synchronous_executor, bulk_asynchronous_executor, bulk_continuation_executor {};





