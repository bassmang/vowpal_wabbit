// Copyright (c) by respective owners including Yahoo!, Microsoft, and
// individual contributors. All rights reserved. Released under a BSD (revised)
// license as described in the file LICENSE.

#pragma once

#include <cstdint>
#ifndef _WIN32
#  include <sys/mman.h>
#endif

#include "memory.h"

typedef float weight;

template <typename T>
class dense_iterator
{
private:
  T* _current;
  T* _begin;
  uint32_t _stride;

public:
  typedef std::forward_iterator_tag iterator_category;
  typedef T value_type;
  typedef std::ptrdiff_t difference_type;
  typedef T* pointer;
  typedef T& reference;

  dense_iterator(T* current, T* begin, uint32_t stride) : _current(current), _begin(begin), _stride(stride) {}

  T& operator*() { return *_current; }

  size_t index() { return _current - _begin; }

  dense_iterator& operator++()
  {
    _current += _stride;
    return *this;
  }

  bool operator==(const dense_iterator& rhs) const { return _current == rhs._current; }
  bool operator!=(const dense_iterator& rhs) const { return _current != rhs._current; }
};

class dense_parameters
{
private:
  weight* _begin;
  uint64_t _weight_mask;  // (stride*(1 << num_bits) -1)
  uint32_t _stride_shift;
  bool _seeded;  // whether the instance is sharing model state with others

public:
  typedef dense_iterator<weight> iterator;
  typedef dense_iterator<const weight> const_iterator;
  dense_parameters(size_t length, uint32_t stride_shift = 0)
      : _begin(calloc_mergable_or_throw<weight>(length << stride_shift))
      , _weight_mask((length << stride_shift) - 1)
      , _stride_shift(stride_shift)
      , _seeded(false)
  {
  }

  dense_parameters() : _begin(nullptr), _weight_mask(0), _stride_shift(0), _seeded(false) {}

  bool not_null() { return (_weight_mask > 0 && _begin != nullptr); }

  dense_parameters(const dense_parameters& other) = delete;
  dense_parameters& operator=(const dense_parameters& other) = delete;
  dense_parameters& operator=(dense_parameters&&) noexcept = delete;
  dense_parameters(dense_parameters&&) noexcept = delete;

  weight* first()
  {
    return _begin;
  }  // TODO: Temporary fix for allreduce.
     // iterator with stride
  iterator begin() { return iterator(_begin, _begin, stride()); }
  iterator end() { return iterator(_begin + _weight_mask + 1, _begin, stride()); }

  // const iterator
  const_iterator cbegin() { return const_iterator(_begin, _begin, stride()); }
  const_iterator cend() { return const_iterator(_begin + _weight_mask + 1, _begin, stride()); }

  inline const weight& operator[](size_t i) const { return _begin[i & _weight_mask]; }
  inline weight& operator[](size_t i) { return _begin[i & _weight_mask]; }

  void shallow_copy(const dense_parameters& input)
  {
    if (!_seeded) free(_begin);
    _begin = input._begin;
    _weight_mask = input._weight_mask;
    _stride_shift = input._stride_shift;
    _seeded = true;
  }

  inline weight& strided_index(size_t index) { return operator[](index << _stride_shift); }

  template <typename Lambda>
  void set_default(Lambda&& default_func)
  {
    auto iter = begin();
    for (size_t i = 0; iter != end(); ++iter, i += stride())
    {
      // Types are required to be weight* and uint64_t.
      default_func(&(*iter), iter.index());
    }
  }

  void set_zero(size_t offset)
  {
    for (iterator iter = begin(); iter != end(); ++iter) (&(*iter))[offset] = 0;
  }

  void swap_offsets(size_t offset_1, size_t offset_2, size_t params_per_problem)
  {
    float temp = 0.f;
    assert(offset_1 < offset_2);
    for (iterator iter = begin(); iter != end(); ++iter)
    {
      // assert(*iter == _begin[(count << stride_shift()) & _weight_mask]);
      size_t actual_index = iter.index() >> stride_shift();
      size_t actual_type = actual_index & (params_per_problem - 1);
      if (actual_type == offset_1)
      {
        float* other = &_begin[(actual_index + (offset_2 - offset_1)) << stride_shift()];

        if (*other != 0.f || *iter != 0.f)
        {
          temp = *other;
          (&(*other))[0] = (&(*iter))[0];
          (&(*iter))[0] = temp;
        }
      }
    }
  }

  void copy_offsets(size_t from, size_t to, size_t params_per_problem)
  {
    std::int64_t diff = to - from;
    // assert(from < to);
    for (iterator iter = begin(); iter != end(); ++iter)
    {
      // assert(*iter == _begin[(count << stride_shift()) & _weight_mask]);
      size_t actual_index = iter.index() >> stride_shift();
      size_t actual_type = actual_index & (params_per_problem - 1);
      if (actual_type == from)
      {
        float* other = &_begin[(actual_index + diff) << stride_shift()];

        if (*other != 0.f || *iter != 0.f) { (&(*other))[0] = (&(*iter))[0]; }
      }
    }
  }

  // void copy_offsets(size_t from, size_t to, size_t width = 1, size_t params_per_problem = 1)
  // {
  //   for (iterator iter = begin(); iter != end(); ++iter)
  //   {
  //     for (size_t j = 0; j < width; j++) { (&(*iter))[to + j] = (&(*iter))[from + j]; }
  //   }
  // }

  void clear_offset(size_t offset, size_t params_per_problem)
  {
    for (iterator iter = begin(); iter != end(); ++iter)
    {
      size_t actual = (iter.index() >> stride_shift()) & (params_per_problem - 1);
      if (actual == offset && *iter != 0.f) { (&(*iter))[0] = 0; }
    }
  }

  uint64_t mask() const { return _weight_mask; }

  uint64_t seeded() const { return _seeded; }

  uint32_t stride() const { return 1 << _stride_shift; }

  uint32_t stride_shift() const { return _stride_shift; }

  void stride_shift(uint32_t stride_shift) { _stride_shift = stride_shift; }

#ifndef _WIN32
#  ifndef DISABLE_SHARED_WEIGHTS
  void share(size_t length)
  {
    float* shared_weights = static_cast<float*>(mmap(
        nullptr, (length << _stride_shift) * sizeof(float), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
    size_t float_count = length << _stride_shift;
    weight* dest = shared_weights;
    memcpy(dest, _begin, float_count * sizeof(float));
    free(_begin);
    _begin = dest;
  }
#  endif
#endif

  ~dense_parameters()
  {
    if (_begin != nullptr && !_seeded)  // don't free weight vector if it is shared with another instance
    {
      free(_begin);
      _begin = nullptr;
    }
  }
};
