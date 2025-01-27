/**
 * @file
 * This file is part of SeisSol.
 *
 * @author Alexander Breuer (breuer AT mytum.de,
 *http://www5.in.tum.de/wiki/index.php/Dipl.-Math._Alexander_Breuer)
 * @author Carsten Uphoff (c.uphoff AT tum.de,
 *http://www5.in.tum.de/wiki/index.php/Carsten_Uphoff,_M.Sc.)
 *
 * @section LICENSE
 * Copyright (c) 2014, SeisSol Group
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE  USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * @section DESCRIPTION
 * Common kernel-level functions
 **/

#ifndef COMMON_HPP_
#define COMMON_HPP_

#include <type_traits>
#include <utility>
#include <algorithm>
#include <Initializer/typedefs.hpp>
#include <generated_code/init.h>
#include <generated_code/kernel.h>
#include <cassert>

/**
 * Uses SFINAE to generate the following functions:
 *
 * has_NAME<T>::value -> true if class T has member NAME and false otherwise
 * set_NAME<T>(kernel, ptr) -> sets kernel.NAME = ptr if class T has member NAME and does nothing
 * otherwise get_static_ptr_NAME<T>() returns &T::NAME[0] if class T has member NAME and nullptr
 * otherwise get_ptr_NAME<T>(T& obj) returns &obj.NAME[0] if class T has member NAME and nullptr
 * otherwise
 */
#define GENERATE_HAS_MEMBER(NAME)                                                                  \
  namespace seissol {                                                                              \
  namespace kernels {                                                                              \
  template <typename T>                                                                            \
  struct has_##NAME {                                                                              \
    template <typename U>                                                                          \
    static constexpr decltype(std::declval<U>().NAME, bool()) test(int) {                          \
      return true;                                                                                 \
    }                                                                                              \
    template <typename U>                                                                          \
    static constexpr bool test(...) {                                                              \
      return false;                                                                                \
    }                                                                                              \
    static constexpr bool value = test<T>(int());                                                  \
  };                                                                                               \
  template <class T>                                                                               \
  auto set_##NAME(T& kernel, decltype(T::NAME) ptr) ->                                             \
      typename std::enable_if<has_##NAME<T>::value>::type {                                        \
    kernel.NAME = ptr;                                                                             \
  }                                                                                                \
  template <class T>                                                                               \
  auto set_##NAME(T&, void*) -> typename std::enable_if<!has_##NAME<T>::value>::type {}            \
  template <class T>                                                                               \
  constexpr auto get_static_ptr_##NAME() ->                                                        \
      typename std::enable_if<has_##NAME<T>::value, decltype(&T::NAME[0])>::type {                 \
    return &T::NAME[0];                                                                            \
  }                                                                                                \
  template <class T>                                                                               \
  constexpr auto get_static_ptr_##NAME() ->                                                        \
      typename std::enable_if<!has_##NAME<T>::value, void*>::type {                                \
    return nullptr;                                                                                \
  }                                                                                                \
  template <class T>                                                                               \
  constexpr auto get_ptr_##NAME(T& obj) ->                                                         \
      typename std::enable_if<has_##NAME<T>::value, decltype(&obj.NAME[0])>::type {                \
    return &obj.NAME[0];                                                                           \
  }                                                                                                \
  template <class T>                                                                               \
  constexpr auto get_ptr_##NAME(T&) ->                                                             \
      typename std::enable_if<!has_##NAME<T>::value, void*>::type {                                \
    return nullptr;                                                                                \
  }                                                                                                \
  }                                                                                                \
  }

namespace seissol {
namespace kernels {
/**
 * Gets the number of basis functions for the given convergence order.
 *
 * @param convergenceOrder convergence order.
 * @return number of basis funcitons.
 **/
constexpr unsigned int
    getNumberOfBasisFunctions(unsigned int convergenceOrder = CONVERGENCE_ORDER) {
  return convergenceOrder * (convergenceOrder + 1) * (convergenceOrder + 2) / 6;
}

/**
 * Gets the number of aligned reals, i.e. the number padded to the size of the alignment.
 *
 * @param alignment alignment in bytes.
 * @return aligned number of reals.
 **/
template <typename RealT = real>
constexpr unsigned int getNumberOfAlignedReals(unsigned int numberOfReals,
                                               unsigned int alignment = VECTORSIZE) {
  // in principle, we could simplify this formula by substituting alignment = alignment /
  // sizeof(real). However, this will cause errors, if alignment is not dividable by sizeof(real)
  // which could happen e.g. if alignment < sizeof(real), or if we have real == long double (if
  // there is ever such a use case, and if the alignment then still makes much sense).
  return (numberOfReals * sizeof(RealT) +
          (alignment - (numberOfReals * sizeof(RealT)) % alignment) % alignment) /
         sizeof(RealT);
}

/**
 * Get the # of basis functions aligned to the given boundaries.
 *
 * @param convergenceOrder convergence order.
 * @param alignment alignment in bytes.
 * @return aligned number of basis functions.
 **/
template <typename RealT = real>
constexpr unsigned int
    getNumberOfAlignedBasisFunctions(unsigned int convergenceOrder = CONVERGENCE_ORDER,
                                     unsigned int alignment = VECTORSIZE) {
  // return (numberOfBasisFunctions(O) * REAL_BYTES + (ALIGNMENT - (numberOfBasisFunctions(O) *
  // REAL_BYTES) % ALIGNMENT) % ALIGNMENT) / REAL_BYTES
  unsigned int numberOfBasisFunctions = getNumberOfBasisFunctions(convergenceOrder);
  return getNumberOfAlignedReals<RealT>(numberOfBasisFunctions);
}

/**
 * Get the # of derivatives of basis functions aligned to the given boundaries.
 *
 * @param convergenceOrder convergence order.
 * @param alignment alignment in bytes.
 * @return aligned number of basis functions.
 **/
constexpr unsigned
    getNumberOfAlignedDerivativeBasisFunctions(unsigned int convergenceOrder = CONVERGENCE_ORDER,
                                               unsigned int alignment = VECTORSIZE) {
  return (convergenceOrder > 0)
             ? getNumberOfAlignedBasisFunctions(convergenceOrder) +
                   getNumberOfAlignedDerivativeBasisFunctions(convergenceOrder - 1)
             : 0;
}

/**
 * Converts memory aligned degrees of freedom (with zero padding) to unaligned (compressed, without
 *zero padding) storage.
 *
 * @param alignedDofs aligned degrees of freedom (zero padding in the basis functions / columns).
 * @param o_unalignedDofs unaligned degrees of freedom.
 **/
template <typename real_from, typename real_to>
void convertAlignedDofs(const real_from alignedDofs[tensor::Q::size()],
                        real_to o_unalignedDofs[tensor::QFortran::size()]) {
  kernel::copyQToQFortran krnl;
  krnl.Q = alignedDofs;
#ifdef MULTIPLE_SIMULATIONS
  krnl.multSimToFirstSim = init::multSimToFirstSim::Values;
#endif

  if (std::is_same<real_from, real_to>::value) {
    krnl.QFortran = reinterpret_cast<real_from*>(o_unalignedDofs);
    krnl.execute();
  } else {
    real_from unalignedDofs[tensor::QFortran::size()];
    krnl.QFortran = unalignedDofs;
    krnl.execute();
    std::copy(unalignedDofs, unalignedDofs + tensor::QFortran::size(), o_unalignedDofs);
  }
}

/**
 * uses SFINAE to check if class T has a size() function.
 */
template <typename T>
struct has_size {
  template <typename U>
  static constexpr decltype(std::declval<U>().size(), bool()) test(int) {
    return true;
  }
  template <typename U>
  static constexpr bool test(...) {
    return false;
  }
  static constexpr bool value = test<T>(int());
};

/**
 * returns T::size() if T has size function and 0 otherwise
 */
template <class T>
constexpr auto size() -> typename std::enable_if<has_size<T>::value, unsigned>::type {
  return T::size();
}
template <class T>
constexpr auto size() -> typename std::enable_if<!has_size<T>::value, unsigned>::type {
  return 0;
}
} // namespace kernels

constexpr bool isDeviceOn() {
#ifdef ACL_DEVICE
  return true;
#endif
  return false;
}
} // namespace seissol

// for now, make these #defines constexprs. Soon, they should be namespaced.
constexpr unsigned int NUMBER_OF_BASIS_FUNCTIONS = seissol::kernels::getNumberOfBasisFunctions();
constexpr unsigned int NUMBER_OF_ALIGNED_BASIS_FUNCTIONS =
    seissol::kernels::getNumberOfAlignedBasisFunctions();
constexpr unsigned int NUMBER_OF_ALIGNED_DER_BASIS_FUNCTIONS =
    seissol::kernels::getNumberOfAlignedDerivativeBasisFunctions();

// for attenuation
constexpr unsigned int NUMBER_OF_ALIGNED_STRESS_DOFS = 6 * NUMBER_OF_ALIGNED_BASIS_FUNCTIONS;

#endif
