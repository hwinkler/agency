#pragma once

#include <agency/detail/config.hpp>
#include <agency/detail/requires.hpp>
#include <agency/bulk_invoke.hpp>
#include <agency/execution/execution_policy.hpp>
#include <agency/detail/type_traits.hpp>

namespace agency
{
namespace detail
{


struct copy_functor
{
  __agency_exec_check_disable__
  template<class Agent, class RandomAccessIterator1, class RandomAccessIterator2>
  __AGENCY_ANNOTATION
  void operator()(Agent& self, RandomAccessIterator1 first, RandomAccessIterator2 result)
  {
    auto i = self.rank();

    result[i] = first[i];
  }
};


template<class ExecutionPolicy, class RandomAccessIterator1, class RandomAccessIterator2,
         __AGENCY_REQUIRES(
           !policy_is_sequenced<decay_t<ExecutionPolicy>>::value and
           std::is_convertible<
             typename std::iterator_traits<RandomAccessIterator1>::iterator_category,
             std::random_access_iterator_tag
           >::value and
           std::is_convertible<
             typename std::iterator_traits<RandomAccessIterator2>::iterator_category,
             std::random_access_iterator_tag
           >::value
         )>
__AGENCY_ANNOTATION
RandomAccessIterator2 copy(ExecutionPolicy&& policy, RandomAccessIterator1 first, RandomAccessIterator1 last, RandomAccessIterator2 result)
{
  auto n = last - first;

  agency::bulk_invoke(policy(n), copy_functor(), first, result);
  
  return last + n;
}


template<class ExecutionPolicy, class InputIterator, class OutputIterator,
         __AGENCY_REQUIRES(
           policy_is_sequenced<decay_t<ExecutionPolicy>>::value or
           !std::is_convertible<
             typename std::iterator_traits<InputIterator>::iterator_category,
             std::random_access_iterator_tag
           >::value or
           !std::is_convertible<
             typename std::iterator_traits<OutputIterator>::iterator_category,
             std::random_access_iterator_tag
           >::value
         )>
__AGENCY_ANNOTATION
OutputIterator copy(ExecutionPolicy&&, InputIterator first, InputIterator last, OutputIterator result)
{
  // XXX we might wish to bulk_invoke a single agent and execute this loop inside

  for(; first != last; ++first, ++result)
  {
    *result = *first;
  }

  return result;
}


template<class InputIterator, class OutputIterator>
__AGENCY_ANNOTATION
OutputIterator copy(InputIterator first, InputIterator last, OutputIterator result)
{
  // pass this instead of agency::seq to work around the prohibition on
  // taking the address of a global constexpr object (i.e., agency::seq) from a CUDA __device__ function
  agency::sequenced_execution_policy seq;
  return detail::copy(seq, first, last, result);
}


} // end detail
} // end agency

