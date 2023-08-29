/**
 * @file
 * This file is part of SeisSol.
 *
 * @author Carsten Uphoff (c.uphoff AT tum.de, http://www5.in.tum.de/wiki/index.php/Carsten_Uphoff,_M.Sc.)
 * @author Stephanie Wollherr (wollherr AT geophysik.uni-muenchen.de, https://www.geophysik.uni-muenchen.de/Members/wollherr)
 *
 * @section LICENSE
 * Copyright (c) 2017, SeisSol Group
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
 * Plasticity kernel of SeisSol.
 **/

#include "Plasticity.h"

#include <Common/cellconfigconv.hpp>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <generated_code/kernel.h>
#include <generated_code/init.h>
#include "common.hpp"
#include "Common/configtensor.hpp"

#ifdef ACL_DEVICE
#include "device.h"
#include "DeviceAux/PlasticityAux.h"
using namespace device;
#endif

namespace seissol::kernels {
  template<typename Config>
  unsigned Plasticity<Config>::computePlasticity(double oneMinusIntegratingFactor,
                                         double timeStepWidth,
                                         double T_v,
                                         GlobalData<Config> const *global,
                                         seissol::model::PlasticityData<RealT> const *plasticityData,
                                         RealT degreesOfFreedom[Yateto<Config>::Tensor::Q::size()],
                                         RealT *pstrain) {
    assert(reinterpret_cast<uintptr_t>(degreesOfFreedom) % Alignment == 0);
    assert(reinterpret_cast<uintptr_t>(global->vandermondeMatrix) % Alignment == 0);
    assert(reinterpret_cast<uintptr_t>(global->vandermondeMatrixInverse) % Alignment == 0);

    constexpr auto NUMBER_OF_ALIGNED_BASIS_FUNCTIONS = seissol::kernels::NumberOfAlignedBasisFunctions<RealT>(Config::ConvergenceOrder);

    RealT QStressNodal[Yateto<Config>::Tensor::QStressNodal::size()] __attribute__((aligned(Alignment)));
    RealT QEtaNodal[Yateto<Config>::Tensor::QEtaNodal::size()] __attribute__((aligned(Alignment)));
    RealT QEtaModal[Yateto<Config>::Tensor::QEtaModal::size()] __attribute__((aligned(Alignment)));
    RealT meanStress[Yateto<Config>::Tensor::meanStress::size()] __attribute__((aligned(Alignment)));
    RealT secondInvariant[Yateto<Config>::Tensor::secondInvariant::size()] __attribute__((aligned(Alignment)));
    RealT tau[Yateto<Config>::Tensor::secondInvariant::size()] __attribute__((aligned(Alignment)));
    RealT taulim[Yateto<Config>::Tensor::meanStress::size()] __attribute__((aligned(Alignment)));
    RealT yieldFactor[Yateto<Config>::Tensor::yieldFactor::size()] __attribute__((aligned(Alignment)));
    RealT dudt_pstrain[Yateto<Config>::Tensor::QStress::size()] __attribute__((aligned(Alignment)));

    static_assert(Yateto<Config>::Tensor::secondInvariant::size() == Yateto<Config>::Tensor::meanStress::size(),
                  "Second invariant tensor and mean stress tensor must be of the same size().");
    static_assert(Yateto<Config>::Tensor::yieldFactor::size() <= Yateto<Config>::Tensor::meanStress::size(),
                  "Yield factor tensor must be smaller than mean stress tensor.");

    //copy dofs for later comparison, only first dof of stresses required
    // @todo multiple sims
    RealT prev_degreesOfFreedom[6 * NUMBER_OF_ALIGNED_BASIS_FUNCTIONS];
    for (unsigned q = 0; q < 6 * NUMBER_OF_ALIGNED_BASIS_FUNCTIONS; ++q) {
      prev_degreesOfFreedom[q] = degreesOfFreedom[q];
    }

    /* Convert modal to nodal and add sigma0.
     * Stores s_{ij} := sigma_{ij} + sigma0_{ij} for every node.
     * sigma0 is constant */
    typename Yateto<Config>::Kernel::plConvertToNodal m2nKrnl;
    m2nKrnl.v = global->vandermondeMatrix;
    m2nKrnl.QStress = degreesOfFreedom;
    m2nKrnl.QStressNodal = QStressNodal;
    m2nKrnl.replicateInitialLoading = Yateto<Config>::Init::replicateInitialLoading::Values;
    m2nKrnl.initialLoading = plasticityData->initialLoading;
    m2nKrnl.execute();

    // Computes m = s_{ii} / 3.0 for every node
    typename Yateto<Config>::Kernel::plComputeMean cmKrnl;
    cmKrnl.meanStress = meanStress;
    cmKrnl.QStressNodal = QStressNodal;
    cmKrnl.selectBulkAverage = Yateto<Config>::Init::selectBulkAverage::Values;
    cmKrnl.execute();

    /* Compute s_{ij} := s_{ij} - m delta_{ij},
     * where delta_{ij} = 1 if i == j else 0.
     * Thus, s_{ij} contains the deviatoric stresses. */
    typename Yateto<Config>::Kernel::plSubtractMean smKrnl;
    smKrnl.meanStress = meanStress;
    smKrnl.QStressNodal = QStressNodal;
    smKrnl.selectBulkNegative = Yateto<Config>::Init::selectBulkNegative::Values;
    smKrnl.execute();

    // Compute I_2 = 0.5 s_{ij} s_ji for every node
    typename Yateto<Config>::Kernel::plComputeSecondInvariant siKrnl;
    siKrnl.secondInvariant = secondInvariant;
    siKrnl.QStressNodal = QStressNodal;
    siKrnl.weightSecondInvariant = Yateto<Config>::Init::weightSecondInvariant::Values;
    siKrnl.execute();

    // tau := sqrt(I_2) for every node
    for (unsigned ip = 0; ip < Yateto<Config>::Tensor::secondInvariant::size(); ++ip) {
      tau[ip] = sqrt(secondInvariant[ip]);
    }

    // Compute tau_c for every node
    for (unsigned ip = 0; ip < Yateto<Config>::Tensor::meanStress::size(); ++ip) {
      taulim[ip] = std::max((RealT) 0.0, plasticityData->cohesionTimesCosAngularFriction -
                                        meanStress[ip] * plasticityData->sinAngularFriction);
    }

    bool adjust = false;
    for (unsigned ip = 0; ip < Yateto<Config>::Tensor::yieldFactor::size(); ++ip) {
      // Compute yield := (t_c / tau - 1) r for every node,
      // where r = 1 - exp(-timeStepWidth / T_v)
      if (tau[ip] > taulim[ip]) {
        adjust = true;
        yieldFactor[ip] = (taulim[ip] / tau[ip] - 1.0) * oneMinusIntegratingFactor;
      } else {
        yieldFactor[ip] = 0.0;
      }
    }

    if (adjust) {
      /**
       * Compute sigma_{ij} := sigma_{ij} + yield s_{ij} for every node
       * and store as modal basis.
       *
       * Remark: According to Wollherr et al., the update formula (13) should be
       *
       * sigmaNew_{ij} := f^* s_{ij} + m delta_{ij} - sigma0_{ij}
       *
       * where f^* = r tau_c / tau + (1 - r) = 1 + yield. Adding 0 to (13) gives
       *
       * sigmaNew_{ij} := f^* s_{ij} + m delta_{ij} - sigma0_{ij}
       *                  + sigma_{ij} + sigma0_{ij} - sigma_{ij} - sigma0_{ij}
       *                = f^* s_{ij} + sigma_{ij} - s_{ij}
       *                = sigma_{ij} + (f^* - 1) s_{ij}
       *                = sigma_{ij} + yield s_{ij}
       */
      typename Yateto<Config>::Kernel::plAdjustStresses adjKrnl;
      adjKrnl.QStress = degreesOfFreedom;
      adjKrnl.vInv = global->vandermondeMatrixInverse;
      adjKrnl.QStressNodal = QStressNodal;
      adjKrnl.yieldFactor = yieldFactor;
      adjKrnl.execute();

      // calculate plastic strain with first dof only (for now)
      for (unsigned q = 0; q < 6 * NUMBER_OF_ALIGNED_BASIS_FUNCTIONS; ++q) {
        /**
         * Equation (10) from Wollherr et al.:
         *
         * d/dt strain_{ij} = (sigma_{ij} + sigma0_{ij} - P_{ij}(sigma)) / (2mu T_v)
         *
         * where (11)
         *
         * P_{ij}(sigma) = { tau_c/tau s_{ij} + m delta_{ij}         if     tau >= taulim
         *                 { sigma_{ij} + sigma0_{ij}                else
         *
         * Thus,
         *
         * d/dt strain_{ij} = { (1 - tau_c/tau) / (2mu T_v) s_{ij}   if     tau >= taulim
         *                    { 0                                    else
         *
         * Consider tau >= taulim first. We have (1 - tau_c/tau) = -yield / r. Therefore,
         *
         * d/dt strain_{ij} = -1 / (2mu T_v r) yield s_{ij}
         *                  = -1 / (2mu T_v r) (sigmaNew_{ij} - sigma_{ij})
         *                  = (sigma_{ij} - sigmaNew_{ij}) / (2mu T_v r)
         *
         * If tau < taulim, then sigma_{ij} - sigmaNew_{ij} = 0.
         */
        RealT factor = plasticityData->mufactor / (T_v * oneMinusIntegratingFactor);
        dudt_pstrain[q] = factor * (prev_degreesOfFreedom[q] - degreesOfFreedom[q]);
        // Integrate with explicit Euler
        pstrain[q] += timeStepWidth * dudt_pstrain[q];
      }
      /* Convert modal to nodal */
      typename Yateto<Config>::Kernel::plConvertToNodalNoLoading m2nKrnl_dudt_pstrain;
      m2nKrnl_dudt_pstrain.v = global->vandermondeMatrix;
      m2nKrnl_dudt_pstrain.QStress = dudt_pstrain;
      m2nKrnl_dudt_pstrain.QStressNodal = QStressNodal;
      m2nKrnl_dudt_pstrain.execute();

      for (unsigned q = 0; q < NUMBER_OF_ALIGNED_BASIS_FUNCTIONS; ++q) {
        QEtaModal[q] = pstrain[6 * NUMBER_OF_ALIGNED_BASIS_FUNCTIONS + q];
      }

      /* Convert modal to nodal */
      typename Yateto<Config>::Kernel::plConvertEtaModal2Nodal m2n_eta_Krnl;
      m2n_eta_Krnl.v = global->vandermondeMatrix;
      m2n_eta_Krnl.QEtaModal = QEtaModal;
      m2n_eta_Krnl.QEtaNodal = QEtaNodal;
      m2n_eta_Krnl.execute();

      auto QStressNodalView = Yateto<Config>::Init::QStressNodal::view::create(QStressNodal);
      unsigned numNodes = QStressNodalView.shape(0);
      for (unsigned i = 0; i < numNodes; ++i) {
        // eta := int_0^t sqrt(0.5 dstrain_{ij}/dt dstrain_{ij}/dt) dt
        // Approximate with eta += timeStepWidth * sqrt(0.5 dstrain_{ij}/dt dstrain_{ij}/dt)
        QEtaNodal[i] = std::max((RealT) 0.0, QEtaNodal[i]) + 
                       timeStepWidth * sqrt(0.5 * (QStressNodalView(i, 0) * QStressNodalView(i, 0)  + QStressNodalView(i, 1) * QStressNodalView(i, 1)
                                                  + QStressNodalView(i, 2) * QStressNodalView(i, 2)  + QStressNodalView(i, 3) * QStressNodalView(i, 3)
                                                  + QStressNodalView(i, 4) * QStressNodalView(i, 4)  + QStressNodalView(i, 5) * QStressNodalView(i, 5)));
      }
 
      /* Convert nodal to modal */
      typename Yateto<Config>::Kernel::plConvertEtaNodal2Modal n2m_eta_Krnl;
      n2m_eta_Krnl.vInv = global->vandermondeMatrixInverse;
      n2m_eta_Krnl.QEtaNodal = QEtaNodal;
      n2m_eta_Krnl.QEtaModal = QEtaModal;
      n2m_eta_Krnl.execute();
      for (unsigned q = 0; q < NUMBER_OF_ALIGNED_BASIS_FUNCTIONS; ++q) {
        pstrain[6 * NUMBER_OF_ALIGNED_BASIS_FUNCTIONS + q] = QEtaModal[q];
      }
      return 1;
    }

    return 0;
  }

  template<typename Config>
  unsigned Plasticity<Config>::computePlasticityBatched(double oneMinusIntegratingFactor,
                                                double timeStepWidth,
                                                double T_v,
                                                GlobalData<Config> const *global,
                                                initializers::recording::ConditionalPointersToRealTsTable &table,
                                                seissol::model::PlasticityData<RealT> *plasticityData) {
#ifdef ACL_DEVICE
    static_assert(Yateto<Config>::Tensor::Q::Shape[0] == Yateto<Config>::Tensor::QStressNodal::Shape[0],
                  "modal and nodal dofs must have the same leading dimensions");
    static_assert(Yateto<Config>::Tensor::Q::Shape[0] == Yateto<Config>::Tensor::v::Shape[0],
                  "modal dofs and vandermonde matrix must hage the same leading dimensions");

    DeviceInstance &device = DeviceInstance::getInstance();
    ConditionalKey key(*KernelNames::Plasticity);
    auto defaultStream = device.api->getDefaultStream();

    if (table.find(key) != table.end()) {
      unsigned stackMemCounter{0};
      auto& entry = table[key];
      const size_t numElements = (entry.get(inner_keys::Wp::Id::Dofs))->getSize();

      //copy dofs for later comparison, only first dof of stresses required
      constexpr unsigned dofsSize = Yateto<Config>::Tensor::Q::Size;
      const size_t prevDofsSize = dofsSize * numElements * sizeof(RealT);
      RealT *prevDofs = reinterpret_cast<RealT*>(device.api->getStackMemory(prevDofsSize));
      ++stackMemCounter;

      RealT** dofsPtrs = (entry.get(inner_keys::Wp::Id::Dofs))->getDeviceDataPtr();
      device.algorithms.copyScatterToUniform(dofsPtrs, prevDofs, dofsSize, dofsSize, numElements, defaultStream);


      // Convert modal to nodal
      RealT** modalStressTensors = (entry.get(inner_keys::Wp::Id::Dofs))->getDeviceDataPtr();
      RealT** nodalStressTensors = (entry.get(inner_keys::Wp::Id::NodalStressTensor))->getDeviceDataPtr();

      assert(global->replicateStresses != nullptr && "replicateStresses has not been initialized");
      static_assert(Yateto<Config>::Kernel::gpu_plConvertToNodal::TmpMaxMemRequiredInBytes == 0);
      RealT** initLoad = (entry.get(inner_keys::Wp::Id::InitialLoad))->getDeviceDataPtr();
      typename Yateto<Config>::Kernel::gpu_plConvertToNodal m2nKrnl;
      m2nKrnl.v = global->vandermondeMatrix;
      m2nKrnl.QStress = const_cast<const RealT**>(modalStressTensors);
      m2nKrnl.QStressNodal = nodalStressTensors;
      m2nKrnl.replicateInitialLoadingM = global->replicateStresses;
      m2nKrnl.initialLoadingM = const_cast<const RealT**>(initLoad);
      m2nKrnl.streamPtr = defaultStream;
      m2nKrnl.numElements = numElements;
      m2nKrnl.execute();

      // adjust deviatoric tensors
      auto *isAdjustableVector =
          reinterpret_cast<unsigned*>(device.api->getStackMemory(numElements * sizeof(unsigned)));
      ++stackMemCounter;

      device::aux::plasticity::adjustDeviatoricTensors(nodalStressTensors,
                                                       isAdjustableVector,
                                                       plasticityData,
                                                       oneMinusIntegratingFactor,
                                                       numElements,
                                                       defaultStream);

      // count how many elements needs to be adjusted
      unsigned numAdjustedElements = device.algorithms.reduceVector(isAdjustableVector,
                                                                    numElements,
                                                                    ::device::ReductionType::Add,
                                                                    defaultStream);

      // convert back to modal (taking into account the adjustment)
      static_assert(typename Yateto<Config>::Kernel::gpu_plConvertToModal::TmpMaxMemRequiredInBytes == 0);
      typename Yateto<Config>::Kernel::gpu_plConvertToModal n2mKrnl;
      n2mKrnl.vInv = global->vandermondeMatrixInverse;
      n2mKrnl.QStressNodal = const_cast<const RealT**>(nodalStressTensors);
      n2mKrnl.QStress = modalStressTensors;
      n2mKrnl.streamPtr = defaultStream;
      n2mKrnl.flags = isAdjustableVector;
      n2mKrnl.numElements = numElements;
      n2mKrnl.execute();


      // prepare memory
      const size_t QEtaNodalSize = Yateto<Config>::Tensor::QEtaNodal::Size * numElements * sizeof(RealT);
      RealT *QEtaNodal = reinterpret_cast<RealT*>(device.api->getStackMemory(QEtaNodalSize));
      RealT **QEtaNodalPtrs = reinterpret_cast<RealT**>(device.api->getStackMemory(numElements * sizeof(RealT*)));

      const size_t QEtaModalSize = Yateto<Config>::Tensor::QEtaModal::Size * numElements * sizeof(RealT);
      RealT *QEtaModal = reinterpret_cast<RealT*>(device.api->getStackMemory(QEtaModalSize));
      RealT **QEtaModalPtrs = reinterpret_cast<RealT**>(device.api->getStackMemory(numElements * sizeof(RealT*)));

      static_assert(Yateto<Config>::Tensor::QStress::Size == Yateto<Config>::Tensor::QStressNodal::Size);
      const size_t dUdTpstrainSize = Yateto<Config>::Tensor::QStressNodal::Size * numElements * sizeof(RealT);
      RealT *dUdTpstrain = reinterpret_cast<RealT*>(device.api->getStackMemory(dUdTpstrainSize));
      RealT **dUdTpstrainPtrs = reinterpret_cast<RealT**>(device.api->getStackMemory(numElements * sizeof(RealT*)));

      stackMemCounter += 6;

      device::aux::plasticity::adjustPointers(QEtaNodal,
                                              QEtaNodalPtrs,
                                              QEtaModal,
                                              QEtaModalPtrs,
                                              dUdTpstrain,
                                              dUdTpstrainPtrs,
                                              numElements,
                                              defaultStream);

      // ------------------------------------------------------------------------------
      RealT **pstrains = entry.get(inner_keys::Wp::Id::Pstrains)->getDeviceDataPtr();
      RealT **dofs = modalStressTensors;
      device::aux::plasticity::computePstrains(pstrains,
                                               plasticityData,
                                               dofs,
                                               prevDofs,
                                               dUdTpstrainPtrs,
                                               T_v,
                                               oneMinusIntegratingFactor,
                                               timeStepWidth,
                                               isAdjustableVector,
                                               numElements,
                                               defaultStream);


      // Convert modal to nodal
      static_assert(typename Yateto<Config>::Kernel::gpu_plConvertToNodalNoLoading::TmpMaxMemRequiredInBytes == 0);
      typename Yateto<Config>::Kernel::gpu_plConvertToNodalNoLoading m2nKrnl_dudt_pstrain;
      m2nKrnl_dudt_pstrain.v = global->vandermondeMatrix;
      m2nKrnl_dudt_pstrain.QStress = const_cast<const RealT**>(dUdTpstrainPtrs);
      m2nKrnl_dudt_pstrain.QStressNodal = nodalStressTensors;
      m2nKrnl_dudt_pstrain.streamPtr = defaultStream;
      m2nKrnl_dudt_pstrain.flags = isAdjustableVector;
      m2nKrnl_dudt_pstrain.numElements = numElements;
      m2nKrnl_dudt_pstrain.execute();

      device::aux::plasticity::pstrainToQEtaModal(pstrains,
                                                  QEtaModalPtrs,
                                                  isAdjustableVector,
                                                  numElements,
                                                  defaultStream);

      // Convert modal to nodal
      static_assert(typename Yateto<Config>::Kernel::gpu_plConvertEtaModal2Nodal::TmpMaxMemRequiredInBytes == 0);
      typename Yateto<Config>::Kernel::gpu_plConvertEtaModal2Nodal m2n_eta_Krnl;
      m2n_eta_Krnl.v = global->vandermondeMatrix;
      m2n_eta_Krnl.QEtaModal = const_cast<const RealT**>(QEtaModalPtrs);
      m2n_eta_Krnl.QEtaNodal = QEtaNodalPtrs;
      m2n_eta_Krnl.streamPtr = defaultStream;
      m2n_eta_Krnl.flags = isAdjustableVector;
      m2n_eta_Krnl.numElements = numElements;
      m2n_eta_Krnl.execute();

      // adjust: QEtaNodal
      device::aux::plasticity::updateQEtaNodal(QEtaNodalPtrs,
                                               nodalStressTensors,
                                               timeStepWidth,
                                               isAdjustableVector,
                                               numElements,
                                               defaultStream);

      // Convert nodal to modal
      static_assert(typename Yateto<Config>::Kernel::gpu_plConvertEtaNodal2Modal::TmpMaxMemRequiredInBytes == 0);
      typename Yateto<Config>::Kernel::gpu_plConvertEtaNodal2Modal n2m_eta_Krnl;
      n2m_eta_Krnl.vInv = global->vandermondeMatrixInverse;
      n2m_eta_Krnl.QEtaNodal = const_cast<const RealT**>(QEtaNodalPtrs);
      n2m_eta_Krnl.QEtaModal = QEtaModalPtrs;
      n2m_eta_Krnl.streamPtr = defaultStream;
      n2m_eta_Krnl.flags = isAdjustableVector;
      n2m_eta_Krnl.numElements = numElements;
      n2m_eta_Krnl.execute();

      // copy: QEtaModal -> pstrain
      device::aux::plasticity::qEtaModalToPstrain(QEtaModalPtrs,
                                                  pstrains,
                                                  isAdjustableVector,
                                                  numElements,
                                                  defaultStream);


      // NOTE: Temp memory must be properly clean after using negative signed integers
      // This kind of memory is mainly used for floating-point numbers. Negative signed ints might corrupt
      // the most significant bits. We came to this conclusion by our first-hand experience
      device.algorithms.fillArray(reinterpret_cast<char*>(isAdjustableVector),
                                  static_cast<char>(0),
                                  numElements * sizeof(int),
                                  defaultStream);

      for (unsigned i = 0; i < stackMemCounter; ++i) {
        device.api->popStackMemory();
      }
      return numAdjustedElements;
    }
    else {
      return 0;
    }

#else
    assert(false && "no implementation provided");
    return 0;
#endif // ACL_DEVICE
  }

  template<typename Config>
  void Plasticity<Config>::flopsPlasticity(long long &o_NonZeroFlopsCheck,
                                   long long &o_HardwareFlopsCheck,
                                   long long &o_NonZeroFlopsYield,
                                   long long &o_HardwareFlopsYield) {
    // reset flops
    o_NonZeroFlopsCheck = 0;
    o_HardwareFlopsCheck = 0;
    o_NonZeroFlopsYield = 0;
    o_HardwareFlopsYield = 0;

    // flops from checking, i.e. outside if (adjust) {}
    o_NonZeroFlopsCheck += Yateto<Config>::Kernel::plConvertToNodal::NonZeroFlops;
    o_HardwareFlopsCheck += Yateto<Config>::Kernel::plConvertToNodal::HardwareFlops;

    // compute mean stress
    o_NonZeroFlopsCheck += Yateto<Config>::Kernel::plComputeMean::NonZeroFlops;
    o_HardwareFlopsCheck += Yateto<Config>::Kernel::plComputeMean::HardwareFlops;

    // subtract mean stress
    o_NonZeroFlopsCheck += Yateto<Config>::Kernel::plSubtractMean::NonZeroFlops;
    o_HardwareFlopsCheck += Yateto<Config>::Kernel::plSubtractMean::HardwareFlops;

    // compute second invariant
    o_NonZeroFlopsCheck += Yateto<Config>::Kernel::plComputeSecondInvariant::NonZeroFlops;
    o_HardwareFlopsCheck += Yateto<Config>::Kernel::plComputeSecondInvariant::HardwareFlops;

    // compute taulim (1 add, 1 mul, max NOT counted)
    o_NonZeroFlopsCheck += 2 * Yateto<Config>::Tensor::meanStress::size();
    o_HardwareFlopsCheck += 2 * Yateto<Config>::Tensor::meanStress::size();

    // check for yield (NOT counted, as it would require counting the number of yielding points)

    // flops from plastic yielding, i.e. inside if (adjust) {}
    o_NonZeroFlopsYield += Yateto<Config>::Kernel::plAdjustStresses::NonZeroFlops;
    o_HardwareFlopsYield += Yateto<Config>::Kernel::plAdjustStresses::HardwareFlops;
  }
} // namespace seissol::kernels

namespace seissol::_definitions {
  const seissol::DeclareForAllConfigs<seissol::kernels::Plasticity> declPlasticity;
}
