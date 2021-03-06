#pragma once

#include <agency/detail/config.hpp>
#include <agency/memory/allocator/detail/allocator_traits.hpp>
#include <agency/memory/allocator/detail/allocator_traits/check_for_member_functions.hpp>
#include <agency/detail/iterator/forwarding_iterator.hpp>
#include <memory>

namespace agency
{
namespace detail
{
namespace allocator_traits_detail
{


__agency_exec_check_disable__
template<class Alloc, class T, class... Args>
__AGENCY_ANNOTATION
typename std::enable_if<
  has_construct<Alloc,T*,Args...>::value
>::type
  construct(Alloc& a, T* p, Args&&... args)
{
  a.construct(p, std::forward<Args>(args)...);
} // end construct()


__agency_exec_check_disable__
template<class Alloc, class T, class... Args>
__AGENCY_ANNOTATION
typename std::enable_if<
  !has_construct<Alloc,T*,Args...>::value
>::type
  construct(Alloc&, T* p, Args&&... args)
{
  ::new(p) T(std::forward<Args>(args)...);
} // end construct()


} // end allocator_traits_detail


template<class Alloc>
  template<class T, class... Args>
__AGENCY_ANNOTATION
void allocator_traits<Alloc>
  ::construct(Alloc& alloc, T* p, Args&&... args)
{
  allocator_traits_detail::construct(alloc, p, std::forward<Args>(args)...);
} // end allocator_traits::construct()


} // end detail
} // end agency

