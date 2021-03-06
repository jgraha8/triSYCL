#ifndef TRISYCL_SYCL_BUFFER_DETAIL_BUFFER_HPP
#define TRISYCL_SYCL_BUFFER_DETAIL_BUFFER_HPP

/** \file The OpenCL SYCL buffer<> detail implementation

    Ronan at Keryell point FR

    This file is distributed under the University of Illinois Open Source
    License. See LICENSE.TXT for details.
*/

#include <cstddef>
#include <memory>

#include <boost/multi_array.hpp>
// \todo Use C++17 optional when it is mainstream
#include <boost/optional.hpp>

#include "CL/sycl/access.hpp"
#include "CL/sycl/buffer/detail/accessor.hpp"
#include "CL/sycl/buffer/detail/buffer_base.hpp"
#include "CL/sycl/buffer/detail/buffer_waiter.hpp"
#include "CL/sycl/range.hpp"

namespace cl {
namespace sycl {
namespace detail {


/** \addtogroup data Data access and storage in SYCL
    @{
*/

/** A SYCL buffer is a multidimensional variable length array (à la C99
    VLA or even Fortran before) that is used to store data to work on.

    In the case we initialize it from a pointer, for now we just wrap the
    data with boost::multi_array_ref to provide the VLA semantics without
    any storage.
*/
template <typename T,
          int Dimensions = 1>
class buffer : public detail::buffer_base,
               public detail::debug<buffer<T, Dimensions>> {
public:

  // Extension to SYCL: provide pieces of STL container interface
  using element = T;
  using value_type = T;
  /* Even if the buffer is read-only use a non-const type so at
     least the current implementation can copy the data too */
  using non_const_value_type = std::remove_const_t<value_type>;

private:

  // \todo Replace U and D somehow by T and Dimensions
  // To allow allocation access
  template <typename U,
            int D,
            access::mode Mode,
            access::target Target /* = access::global_buffer */>
    friend class detail::accessor;

  /** The allocator to be used when some memory is needed

      \todo Implement user-provided allocator
  */
  std::allocator<non_const_value_type> alloc;

  /** This is the multi-dimensional interface to the data that may point
      to either allocation in the case of storage managed by SYCL itself
      or to some other memory location in the case of host memory or
      storage<> abstraction use
  */
  boost::multi_array_ref<value_type, Dimensions> access;

  /** If some allocation is requested on the host for the buffer
      memory, this is where the memory is attached to.

      Note that this is uninitialized memory, as stated in SYCL
      specification.
  */
  non_const_value_type *allocation = nullptr;

  /* How to copy back data on buffer destruction, can be modified with
     set_final_data( ... )
   */
  boost::optional<std::function<void(void)>> final_write_back;

  // Keep the shared pointer used to create the buffer
  shared_ptr_class<T> input_shared_pointer;


  // Track if the buffer memory is provided as host memory
  bool data_host = false;

  // Track if data should be copied if a modification occurs
  bool copy_if_modified = false;

  // Track if data have been modified
  bool modified = false;

public:

  /// Create a new read-write buffer of size \param r
  buffer(const range<Dimensions> &r) : access { allocate_buffer(r) } {}


  /** Create a new read-write buffer from \param host_data of size
      \param r without further allocation */
  buffer(T *host_data, const range<Dimensions> &r) :
    access { host_data, r },
    data_host { true }
  {}


  /** Create a new read-only buffer from \param host_data of size \param r
      without further allocation

      If the buffer is non const, use a copy-on-write mechanism with
      internal writable memory.

      \todo Clarify the semantics in the spec. What happens if the
      host change the host_data after buffer creation?

      Only enable this constructor if the value type is not constant,
      because if it is constant, the buffer is constant too.
  */
  template <typename Dependent = T,
            typename = std::enable_if_t<!std::is_const<Dependent>::value>>
  buffer(const T *host_data, const range<Dimensions> &r) :
    /* The buffer is read-only, even if the internal multidimensional
       wrapper is not. If a write accessor is requested, there should
       be a copy on write. So this pointer should not be written and
       this const_cast should be acceptable. */
    access { const_cast<T *>(host_data), r },
    data_host { true },
    /* Set copy_if_modified to true, so that if an accessor with write
       access is created, data are copied before to be modified. */
    copy_if_modified { true }
  {}


  /** Create a new buffer with associated memory, using the data in
      host_data

      The ownership of the host_data is shared between the runtime and the
      user. In order to enable both the user application and the SYCL
      runtime to use the same pointer, a cl::sycl::mutex_class is
      used.
  */
  buffer(shared_ptr_class<T> &host_data, const range<Dimensions> &r) :
    access { host_data.get(), r },
    input_shared_pointer { host_data },
    data_host { true }
  {}


  /** Create a new buffer with associated memory, using the data owned in
      a unique pointer

      SYCL's runtime has full ownership of the host_data.
  */
  template<typename Deleter>
  buffer(unique_ptr_class<T, Deleter> &&host_data,
         const range<Dimensions> &r) :
    access { host_data.get(), r },
      /* Use the fact that there is an implicit constructor of a \c
         std::shared_ptr from a \c std::unique_ptr to avoid storing
         the unique pointer. Doing so would need to implement
         ourselves some type erasure on the \c Deleter to avoid it
         leaking out of the \c buffer type and \c accessor type.

         It still works as expected since, if we own a shared pointer,
         the \c Deleter is correctly handled and if we own it and its
         use-count is 1, we are the only owner and we can skip the
         copy-back later.
       */
    input_shared_pointer { std::move(host_data) },
    data_host { true }
  {}


  /// Create a new allocated 1D buffer from the given elements
  template <typename Iterator>
  buffer(Iterator start_iterator, Iterator end_iterator) :
    access { allocate_buffer(std::distance(start_iterator, end_iterator)) }
    {
      /* Then assign allocation since this is the only multi_array
         method with this iterator interface */
      access.assign(start_iterator, end_iterator);
    }


  /** Create a new sub-buffer without allocation to have separate
      accessors later

      \todo To implement and deal with reference counting
  buffer(buffer<T, Dimensions> b,
         index<Dimensions> base_index,
         range<Dimensions> sub_range)
  */

  /// \todo Allow CLHPP objects too?
  ///
  /*
  buffer(cl_mem mem_object,
         queue from_queue,
         event available_event)
  */


  /** The buffer content may be copied back on destruction to some
      final location */
  ~buffer() {
    if (modified && final_write_back)
      (*final_write_back)();
    // Allocate explicitly allocated memory if required
    deallocate_buffer();
  }


  /** Enforce the buffer to be considered as being modified.
      Same as creating an accessor with write access.
   */
  void mark_as_written() {
    modified = true;
  }


  /** This method is to be called whenever an accessor is created

      Its current purpose is to track if an accessor with write access
      is created and acting accordingly.
   */
  template <access::mode Mode,
            access::target Target = access::target::host_buffer>
  void track_access_mode() {
    // test if write access is required
    if (   Mode == access::mode::write
        || Mode == access::mode::read_write
        || Mode == access::mode::discard_write
        || Mode == access::mode::discard_read_write
        || Mode == access::mode::atomic
       ) {
      modified = true;
      if (copy_if_modified) {
        // Implement the allocate & copy-on-write optimization
        copy_if_modified = false;
        data_host = false;
        // Since \c allocate_buffer() changes \c access, keep a copy first
        auto current_access = access;
        /* The range is actually computed from \c access itself, so
           save it */
        auto current_range = get_range();
        allocate_buffer(current_range);
        /* Then move everything to the new place

           \todo Use std::uninitialized_move instead, when we switch
           to full C++17
        */
        std::copy(current_access.begin(),
                  current_access.end(),
                  access.begin());
      }
    }
  }


 /** Return a range object representing the size of the buffer in
      terms of number of elements in each dimension as passed to the
      constructor
  */
  auto get_range() const {
    /* Interpret the shape which is a pointer to the first element as an
       array of Dimensions elements so that the range<Dimensions>
       constructor is happy with this collection

       \todo Add also a constructor in range<> to accept a const
       std::size_t *?
    */
    return range<Dimensions> {
      *(const std::size_t (*)[Dimensions])(access.shape())
        };
  }


  /** Returns the total number of elements in the buffer

      Equal to get_range()[0] * ... * get_range()[Dimensions-1].
  */
  auto get_count() const {
    return access.num_elements();
  }


  /** Returns the size of the buffer storage in bytes

      \todo rename to something else. In
      http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2015/p0122r0.pdf
      it is named bytes() for example
  */
  auto get_size() const {
    return get_count()*sizeof(value_type);
  }


  /** Set the weak pointer as destination for write-back on buffer
      destruction
  */
  void set_final_data(std::weak_ptr<T> && final_data) {
    final_write_back = [=] {
      if (auto sptr = final_data.lock()) {
        std::copy_n(access.data(), access.num_elements(), sptr.get());
      }
    };
  }


  /** Provide destination for write-back on buffer destruction as a
      shared pointer.
   */
  void set_final_data(std::shared_ptr<T> && final_data) {
    final_write_back = [=] {
      std::copy_n(access.data(), access.num_elements(), final_data.get());
    };
  }


  /** Disable write-back on buffer destruction as an iterator.
  */
  void set_final_data(std::nullptr_t) {
    final_write_back = boost::none;
  }


  /** Provide destination for write-back on buffer destruction as an
      iterator
  */
  template <typename Iterator>
  void set_final_data(Iterator final_data) {
 /*   using type_ = typename iterator_value_type<Iterator>::value_type;
    static_assert(std::is_same<type_, T>::value, "buffer type mismatch");
    static_assert(!(std::is_const<type_>::value),
                  "const iterator is not allowed");*/
    final_write_back = [=] {
      std::copy_n(access.data(), access.num_elements(), final_data);
    };
  }


private:

  /// Allocate uninitialized buffer memory
  auto allocate_buffer(const range<Dimensions> &r) {
    auto count = r.get_count();
    // Allocate uninitialized memory
    allocation = alloc.allocate(count);
    return boost::multi_array_ref<value_type, Dimensions> { allocation, r };
  }


  /// Deallocate buffer memory if required
  void deallocate_buffer() {
    if (allocation)
      alloc.deallocate(allocation, access.num_elements());
  }


  /** Get a \c future to wait from inside the \c cl::sycl::buffer in
      case there is something to copy back to the host

      \return A \c future in the \c optional if there is something to
      wait for, otherwise an empty \c optional
  */
  boost::optional<std::future<void>> get_destructor_future() {
    /* If there is only 1 shared_ptr user of the buffer, this is the
       caller of this function, the \c buffer_waiter, so there is no
       need to get a \ future otherwise there will be a dead-lock if
       there is only 1 thread waiting for itself.

       Since \c use_count() is applied to a \c shared_ptr just created
       for this purpose, it actually increase locally the count by 1,
       so check for 1 + 1 use count instead...
    */
    // If the buffer's destruction triggers a write-back, wait
    if ((shared_from_this().use_count() > 2) &&
        modified && (final_write_back || data_host)) {
      // Create a promise to wait for
      notify_buffer_destructor = std::promise<void> {};
      // And return the future to wait for it
      return notify_buffer_destructor->get_future();
    }
    return boost::none;
  }


  // Allow buffer_waiter destructor to access get_destructor_future()
  // friend detail::buffer_waiter<T, Dimensions>::~buffer_waiter();
  /* \todo Work around to Clang bug
     https://llvm.org/bugs/show_bug.cgi?id=28873 cannot use destructor
     here */
  friend detail::buffer_waiter<T, Dimensions>;

};


/** Proxy function to avoid some circular type recursion

    \return a shared_ptr<task>

    \todo To remove with some refactoring
*/
template <typename BufferDetail>
static std::shared_ptr<detail::task>
buffer_add_to_task(BufferDetail buf,
                   handler *command_group_handler,
                   bool is_write_mode) {
    return buf->add_to_task(command_group_handler, is_write_mode);
  }

/// @} End the data Doxygen group

}
}
}

/*
    # Some Emacs stuff:
    ### Local Variables:
    ### ispell-local-dictionary: "american"
    ### eval: (flyspell-prog-mode)
    ### End:
*/

#endif // TRISYCL_SYCL_BUFFER_DETAIL_BUFFER_HPP
