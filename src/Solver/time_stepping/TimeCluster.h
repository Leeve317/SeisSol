/******************************************************************************
** Copyright (c) 2015, Intel Corporation                                     **
** All rights reserved.                                                      **
**                                                                           **
** Redistribution and use in source and binary forms, with or without        **
** modification, are permitted provided that the following conditions        **
** are met:                                                                  **
** 1. Redistributions of source code must retain the above copyright         **
**    notice, this list of conditions and the following disclaimer.          **
** 2. Redistributions in binary form must reproduce the above copyright      **
**    notice, this list of conditions and the following disclaimer in the    **
**    documentation and/or other materials provided with the distribution.   **
** 3. Neither the name of the copyright holder nor the names of its          **
**    contributors may be used to endorse or promote products derived        **
**    from this software without specific prior written permission.          **
**                                                                           **
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS       **
** "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT         **
** LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR     **
** A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT      **
** HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,    **
** SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED  **
** TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR    **
** PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF    **
** LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING      **
** NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS        **
** SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.              **
******************************************************************************/
/* Alexander Heinecke (Intel Corp.)
******************************************************************************/
/**
 * @file
 * This file is part of SeisSol.
 *
 * @author Alex Breuer (breuer AT mytum.de, http://www5.in.tum.de/wiki/index.php/Dipl.-Math._Alexander_Breuer)
 * 
 * @section LICENSE
 * Copyright (c) 2013-2015, SeisSol Group
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
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * @section DESCRIPTION
 * LTS cluster in SeisSol.
 **/

#ifndef TIMECLUSTER_H_
#define TIMECLUSTER_H_

#ifdef USE_MPI
#include <mpi.h>
#include <list>
#endif

#include <Initializer/typedefs.hpp>
#include <SourceTerm/typedefs.hpp>
#include <utils/logger.h>
#include <Initializer/LTS.h>
#include <Initializer/tree/LTSTree.hpp>

#include <Kernels/Time.h>
#include <Kernels/Local.h>
#include <Kernels/Neighbor.h>
#include <Kernels/DynamicRupture.h>
#include <Kernels/Plasticity.h>
#include <Kernels/TimeCommon.h>
#include <Solver/FreeSurfaceIntegrator.h>
#include <Monitoring/LoopStatistics.h>
#include <Monitoring/ActorStateStatistics.h>

#include "AbstractTimeCluster.h"

#ifdef ACL_DEVICE
#include <device.h>
#include <Solver/Pipeline/DrPipeline.h>
#endif

namespace seissol {
  namespace time_stepping {
    class TimeCluster;
  }

  namespace kernels {
    class ReceiverCluster;
  }
}

/**
 * Time cluster, which represents a collection of elements having the same time step width.
 **/
class seissol::time_stepping::TimeCluster : public seissol::time_stepping::AbstractTimeCluster
{
private:
    // Last correction time of the neighboring cluster with higher dt
    double lastSubTime;

    void handleAdvancedPredictionTimeMessage(const NeighborCluster& neighborCluster) override;
    void handleAdvancedCorrectionTimeMessage(const NeighborCluster& neighborCluster) override;
    void start() override {}
    void predict() override;
    void correct() override;
    bool usePlasticity;

    //! number of time steps
    unsigned long m_numberOfTimeSteps;

    /*
     * integrators
     */
    //! time kernel
    kernels::Time m_timeKernel;

    //! local kernel
    kernels::Local m_localKernel;

    //! neighbor kernel
    kernels::Neighbor m_neighborKernel;
    
    kernels::DynamicRupture m_dynamicRuptureKernel;

  /*
   * global data
   */
     //! global data structures
    GlobalData *m_globalDataOnHost{nullptr};
    GlobalData *m_globalDataOnDevice{nullptr};
#ifdef ACL_DEVICE
    device::DeviceInstance& device = device::DeviceInstance::getInstance();
    dr::pipeline::DrPipeline drPipeline;
#endif

    /*
     * element data
     */     
    seissol::initializers::Layer* m_clusterData;
    seissol::initializers::Layer* dynRupInteriorData;
    seissol::initializers::Layer* dynRupCopyData;
    seissol::initializers::LTS*         m_lts;
    seissol::initializers::DynamicRupture* m_dynRup;
    dr::friction_law::FrictionSolver* frictionSolver;
    std::unique_ptr<dr::friction_law::FrictionSolver> frictionSolverLocalInterior;
    std::unique_ptr<dr::friction_law::FrictionSolver> frictionSolverLocalCopy;
    dr::output::OutputManager* faultOutputManager;

    //! Mapping of cells to point sources
    sourceterm::CellToPointSourcesMapping const* m_cellToPointSources;

    //! Number of mapping of cells to point sources
    unsigned m_numberOfCellToPointSourcesMappings;

    //! Point sources
    sourceterm::PointSources const* m_pointSources;

    enum class ComputePart {
      Local = 0,
      Neighbor,
      DRNeighbor,
      DRFrictionLawInterior,
      DRFrictionLawCopy,
      PlasticityCheck,
      PlasticityYield,
      NUM_COMPUTE_PARTS
    };

    long long m_flops_nonZero[static_cast<int>(ComputePart::NUM_COMPUTE_PARTS)];
    long long m_flops_hardware[static_cast<int>(ComputePart::NUM_COMPUTE_PARTS)];
    
    //! Tv parameter for plasticity
    double m_tv;
    
    //! Relax time for plasticity
    double m_oneMinusIntegratingFactor;
    
    //! Stopwatch of TimeManager
    LoopStatistics* m_loopStatistics;
    ActorStateStatistics* actorStateStatistics;
    unsigned        m_regionComputeLocalIntegration;
    unsigned        m_regionComputeNeighboringIntegration;
    unsigned        m_regionComputeDynamicRupture;

    kernels::ReceiverCluster* m_receiverCluster;

    /**
     * Writes the receiver output if applicable (receivers present, receivers have to be written).
     **/
    void writeReceivers();

    /**
     * Computes the source terms if applicable.
     **/
    void computeSources();

    /**
     * Computes dynamic rupture.
     **/
    void computeDynamicRupture( seissol::initializers::Layer&  layerData );

    static void collectiveComputeDynamicRupture(const std::vector<TimeCluster*>& clusters);

    /**
     * Computes all cell local integration.
     *
     * This are:
     *  * time integration
     *  * volume integration
     *  * local boundary integration
     *
     * Remark: After this step the DOFs are only updated half with the boundary contribution
     *         of the neighborings cells missing.
     *
     * @param i_numberOfCells number of cells.
     * @param i_cellInformation cell local information.
     * @param i_cellData cell data.
     * @param io_buffers time integration buffers.
     * @param io_derivatives time derivatives.
     * @param io_dofs degrees of freedom.
     **/
    void computeLocalIntegration( seissol::initializers::Layer&  i_layerData, bool resetBuffers);

    static void collectiveComputeLocalIntegration(const std::vector<TimeCluster*>& clusters);
    static void collectiveComputeSources(const std::vector<TimeCluster*>& clusters);

    /**
     * Computes the contribution of the neighboring cells to the boundary integral.
     *
     * Remark: After this step (in combination with the local integration) the DOFs are at the next time step.
     * TODO: This excludes dynamic rupture contribution.
     *
     * @param i_numberOfCells number of cells.
     * @param i_cellInformation cell local information.
     * @param i_cellData cell data.
     * @param i_faceNeighbors pointers to neighboring time buffers or derivatives.
     * @param io_dofs degrees of freedom.
     **/
    void computeNeighboringIntegration( seissol::initializers::Layer&  i_layerData, double subTimeStart );

    void computeLocalIntegrationFlops(seissol::initializers::Layer& layerData);
#ifndef ACL_DEVICE
    template<bool usePlasticity>
    std::pair<long, long> computeNeighboringIntegrationImplementation(seissol::initializers::Layer& i_layerData,
                                                                      double subTimeStart) {
      if (i_layerData.getNumberOfCells() == 0) return {0,0};
      SCOREP_USER_REGION( "computeNeighboringIntegration", SCOREP_USER_REGION_TYPE_FUNCTION )

      m_loopStatistics->begin(m_regionComputeNeighboringIntegration);

      real* (*faceNeighbors)[4] = i_layerData.var(m_lts->faceNeighbors);
      CellDRMapping (*drMapping)[4] = i_layerData.var(m_lts->drMapping);
      CellLocalInformation* cellInformation = i_layerData.var(m_lts->cellInformation);
      PlasticityData* plasticity = i_layerData.var(m_lts->plasticity);
      real (*pstrain)[7 * NUMBER_OF_ALIGNED_BASIS_FUNCTIONS] = i_layerData.var(m_lts->pstrain);
      unsigned numberOTetsWithPlasticYielding = 0;

      kernels::NeighborData::Loader loader;
      loader.load(*m_lts, i_layerData);

      real *l_timeIntegrated[4];
      real *l_faceNeighbors_prefetch[4];

#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none) private(l_timeIntegrated, l_faceNeighbors_prefetch) shared(cellInformation, loader, faceNeighbors, pstrain, i_layerData, plasticity, drMapping, subTimeStart) reduction(+:numberOTetsWithPlasticYielding)
#endif
      for( unsigned int l_cell = 0; l_cell < i_layerData.getNumberOfCells(); l_cell++ ) {
        auto data = loader.entry(l_cell);
        seissol::kernels::TimeCommon::computeIntegrals(m_timeKernel,
                                                       data.cellInformation.ltsSetup,
                                                       data.cellInformation.faceTypes,
                                                       subTimeStart,
                                                       timeStepSize(),
                                                       faceNeighbors[l_cell],
#ifdef _OPENMP
                                                       *reinterpret_cast<real (*)[4][tensor::I::size()]>(&(m_globalDataOnHost->integrationBufferLTS[omp_get_thread_num()*4*tensor::I::size()])),
#else
            *reinterpret_cast<real (*)[4][tensor::I::size()]>(m_globalData->integrationBufferLTS),
#endif
                                                       l_timeIntegrated);

        l_faceNeighbors_prefetch[0] = (cellInformation[l_cell].faceTypes[1] != FaceType::dynamicRupture) ?
                                      faceNeighbors[l_cell][1] :
                                      drMapping[l_cell][1].godunov;
        l_faceNeighbors_prefetch[1] = (cellInformation[l_cell].faceTypes[2] != FaceType::dynamicRupture) ?
                                      faceNeighbors[l_cell][2] :
                                      drMapping[l_cell][2].godunov;
        l_faceNeighbors_prefetch[2] = (cellInformation[l_cell].faceTypes[3] != FaceType::dynamicRupture) ?
                                      faceNeighbors[l_cell][3] :
                                      drMapping[l_cell][3].godunov;

        // fourth face's prefetches
        if (l_cell < (i_layerData.getNumberOfCells()-1) ) {
          l_faceNeighbors_prefetch[3] = (cellInformation[l_cell+1].faceTypes[0] != FaceType::dynamicRupture) ?
                                        faceNeighbors[l_cell+1][0] :
                                        drMapping[l_cell+1][0].godunov;
        } else {
          l_faceNeighbors_prefetch[3] = faceNeighbors[l_cell][3];
        }

        m_neighborKernel.computeNeighborsIntegral( data,
                                                   drMapping[l_cell],
                                                   l_timeIntegrated, l_faceNeighbors_prefetch
        );

        if constexpr (usePlasticity) {
          updateRelaxTime();
          numberOTetsWithPlasticYielding += seissol::kernels::Plasticity::computePlasticity( m_oneMinusIntegratingFactor,
                                                                                             timeStepSize(),
                                                                                             m_tv,
                                                                                             m_globalDataOnHost,
                                                                                             &plasticity[l_cell],
                                                                                             data.dofs,
                                                                                             pstrain[l_cell] );
        }
#ifdef INTEGRATE_QUANTITIES
        seissol::SeisSol::main.postProcessor().integrateQuantities( m_timeStepWidth,
                                                              i_layerData,
                                                              l_cell,
                                                              dofs[l_cell] );
#endif // INTEGRATE_QUANTITIES
      }

      const long long nonZeroFlopsPlasticity =
          i_layerData.getNumberOfCells() * m_flops_nonZero[static_cast<int>(ComputePart::PlasticityCheck)] +
          numberOTetsWithPlasticYielding * m_flops_nonZero[static_cast<int>(ComputePart::PlasticityYield)];
      const long long hardwareFlopsPlasticity =
          i_layerData.getNumberOfCells() * m_flops_hardware[static_cast<int>(ComputePart::PlasticityCheck)] +
          numberOTetsWithPlasticYielding * m_flops_hardware[static_cast<int>(ComputePart::PlasticityYield)];

      m_loopStatistics->end(m_regionComputeNeighboringIntegration, i_layerData.getNumberOfCells(), m_profilingId);

      return {nonZeroFlopsPlasticity, hardwareFlopsPlasticity};
    }

    template<bool usePlasticity>
    static void collectiveComputeNeighboringIntegrationImplementation(const std::vector<TimeCluster*>& clusters) {
      SCOREP_USER_REGION( "computeNeighboringIntegration", SCOREP_USER_REGION_TYPE_FUNCTION )

      for (TimeCluster* cluster : clusters) {
        cluster->m_loopStatistics->begin(cluster->m_regionComputeNeighboringIntegration);
      }

      std::vector<kernels::NeighborData::Loader> loaders(clusters.size());

    std::vector<size_t> clusterOffset(clusters.size() + 1);

    std::vector<double> subTimeStart(clusters.size());

    for (size_t i = 0; i < clusters.size(); ++i) {
      subTimeStart[i] = clusters[i]->ct.correctionTime - clusters[i]->lastSubTime;

      loaders[i].load(*clusters[i]->m_lts, *clusters[i]->m_clusterData);

      clusterOffset[i+1] = clusterOffset[i] + clusters[i]->m_clusterData->getNumberOfCells();
    }

      

      real *l_timeIntegrated[4];
      real *l_faceNeighbors_prefetch[4];

      auto totalCellCount = *clusterOffset.rbegin();

#ifdef _OPENMP
#pragma omp parallel for schedule(static) private(l_timeIntegrated, l_faceNeighbors_prefetch)
#endif
      for (size_t i = 0; i < totalCellCount; ++i) {
        // https://en.cppreference.com/w/cpp/algorithm/upper_bound
        auto idElement = std::upper_bound(clusterOffset.begin(), clusterOffset.end(), i);
        size_t clusterId = std::distance(clusterOffset.begin(), idElement) - 1;

        assert(i > clusterOffset[clusterId]);
        unsigned l_cell = i - clusterOffset[clusterId];

        auto data = loaders[clusterId].entry(l_cell);

        real* (*faceNeighbors)[4] = clusters[clusterId]->m_clusterData->var(clusters[clusterId]->m_lts->faceNeighbors);
        CellDRMapping (*drMapping)[4] = clusters[clusterId]->m_clusterData->var(clusters[clusterId]->m_lts->drMapping);
        CellLocalInformation* cellInformation = clusters[clusterId]->m_clusterData->var(clusters[clusterId]->m_lts->cellInformation);
        PlasticityData* plasticity = clusters[clusterId]->m_clusterData->var(clusters[clusterId]->m_lts->plasticity);
        real (*pstrain)[7 * NUMBER_OF_ALIGNED_BASIS_FUNCTIONS] = clusters[clusterId]->m_clusterData->var(clusters[clusterId]->m_lts->pstrain);

        seissol::kernels::TimeCommon::computeIntegrals(clusters[clusterId]->m_timeKernel,
                                                       data.cellInformation.ltsSetup,
                                                       data.cellInformation.faceTypes,
                                                       subTimeStart[clusterId],
                                                       clusters[clusterId]->timeStepSize(),
                                                       faceNeighbors[l_cell],
#ifdef _OPENMP
                                                       *reinterpret_cast<real (*)[4][tensor::I::size()]>(&(clusters[clusterId]->m_globalDataOnHost->integrationBufferLTS[omp_get_thread_num()*4*tensor::I::size()])),
#else
            *reinterpret_cast<real (*)[4][tensor::I::size()]>(clusters[clusterId]->m_globalData->integrationBufferLTS),
#endif
                                                       l_timeIntegrated);

        l_faceNeighbors_prefetch[0] = (cellInformation[l_cell].faceTypes[1] != FaceType::dynamicRupture) ?
                                      faceNeighbors[l_cell][1] :
                                      drMapping[l_cell][1].godunov;
        l_faceNeighbors_prefetch[1] = (cellInformation[l_cell].faceTypes[2] != FaceType::dynamicRupture) ?
                                      faceNeighbors[l_cell][2] :
                                      drMapping[l_cell][2].godunov;
        l_faceNeighbors_prefetch[2] = (cellInformation[l_cell].faceTypes[3] != FaceType::dynamicRupture) ?
                                      faceNeighbors[l_cell][3] :
                                      drMapping[l_cell][3].godunov;

        // fourth face's prefetches
        if (l_cell < (clusters[clusterId]->m_clusterData->getNumberOfCells()-1) ) {
          // get the next face of the same cluster
          l_faceNeighbors_prefetch[3] = (cellInformation[l_cell+1].faceTypes[0] != FaceType::dynamicRupture) ?
                                        faceNeighbors[l_cell+1][0] :
                                        drMapping[l_cell+1][0].godunov;
        } else if (clusterId < clusters.size()-1) {
          // we'll handle another cluster in the next iteration... So prefetch the first face from there already now.
          real* (*nextFaceNeighbors)[4] = clusters[clusterId+1]->m_clusterData->var(clusters[clusterId+1]->m_lts->faceNeighbors);
          CellDRMapping (*nextDrMapping)[4] = clusters[clusterId+1]->m_clusterData->var(clusters[clusterId+1]->m_lts->drMapping);
          CellLocalInformation* nextCellInformation = clusters[clusterId+1]->m_clusterData->var(clusters[clusterId+1]->m_lts->cellInformation);
          l_faceNeighbors_prefetch[3] = (nextCellInformation[0].faceTypes[0] != FaceType::dynamicRupture) ?
                                        nextFaceNeighbors[0][0] :
                                        nextDrMapping[0][0].godunov;
        } else {
          // we've hit the very last element in the iteration. Nothing more to prefetch.
          l_faceNeighbors_prefetch[3] = faceNeighbors[l_cell][3];
        }

        clusters[clusterId]->m_neighborKernel.computeNeighborsIntegral( data,
                                                   drMapping[l_cell],
                                                   l_timeIntegrated, l_faceNeighbors_prefetch
        );

        if constexpr (usePlasticity) {
          clusters[clusterId]->updateRelaxTime();
          seissol::kernels::Plasticity::computePlasticity( clusters[clusterId]->m_oneMinusIntegratingFactor,
                                                                                             clusters[clusterId]->timeStepSize(),
                                                                                             clusters[clusterId]->m_tv,
                                                                                             clusters[clusterId]->m_globalDataOnHost,
                                                                                             &plasticity[l_cell],
                                                                                             data.dofs,
                                                                                             pstrain[l_cell] );
        }
#ifdef INTEGRATE_QUANTITIES
        seissol::SeisSol::main.postProcessor().integrateQuantities( clusters[clusterId]->m_timeStepWidth,
                                                              clusters[clusterId]->m_clusterData,
                                                              l_cell,
                                                              dofs[l_cell] );
#endif // INTEGRATE_QUANTITIES
      }

      for (TimeCluster* cluster : clusters) {
        cluster->m_loopStatistics->end(cluster->m_regionComputeNeighboringIntegration, cluster->m_clusterData->getNumberOfCells(), cluster->m_profilingId);
      }
    }
#endif // ACL_DEVICE

    void computeLocalIntegrationFlops(unsigned numberOfCells,
                                      CellLocalInformation const* cellInformation,
                                      long long& nonZeroFlops,
                                      long long& hardwareFlops);

    void computeNeighborIntegrationFlops(seissol::initializers::Layer &layerData);

    void computeDynamicRuptureFlops(seissol::initializers::Layer &layerData,
                                    long long& nonZeroFlops,
                                    long long& hardwareFlops);
                                          
    void computeFlops();
    
    //! Update relax time for plasticity
    void updateRelaxTime() {
      m_oneMinusIntegratingFactor = (m_tv > 0.0) ? 1.0 - exp(-timeStepSize() / m_tv) : 1.0;
    }

  const LayerType layerType;
  //! time of the next receiver output
  double m_receiverTime;

  //! print status every 100th timestep
  bool printProgress;
  //! cluster id on this rank
  const unsigned int m_clusterId;

  //! global cluster cluster id
  const unsigned int m_globalClusterId;

  //! id used to identify this cluster (including layer type) when profiling
  const unsigned int m_profilingId;

  DynamicRuptureScheduler* dynamicRuptureScheduler;

  void printTimeoutMessage(std::chrono::seconds timeSinceLastUpdate) override;

public:
  ActResult act() override;

  /**
   * Constructs a new LTS cluster.
   *
   * @param i_clusterId id of this cluster with respect to the current rank.
   * @param i_globalClusterId global id of this cluster.
   * @param usePlasticity true if using plasticity
   **/
  TimeCluster(unsigned int i_clusterId, unsigned int i_globalClusterId, unsigned int profilingId, bool usePlasticity,
              LayerType layerType, double maxTimeStepSize,
              long timeStepRate, bool printProgress,
              DynamicRuptureScheduler* dynamicRuptureScheduler, CompoundGlobalData i_globalData,
              seissol::initializers::Layer *i_clusterData, seissol::initializers::Layer* dynRupInteriorData,
              seissol::initializers::Layer* dynRupCopyData, seissol::initializers::LTS* i_lts,
              seissol::initializers::DynamicRupture* i_dynRup,
              seissol::dr::friction_law::FrictionSolver* i_FrictionSolver,
              dr::output::OutputManager* i_faultOutputManager, LoopStatistics* i_loopStatistics,
              ActorStateStatistics* actorStateStatistics);

  /**
   * Destructor of a LTS cluster.
   * TODO: Currently prints only statistics in debug mode.
   **/
  ~TimeCluster() override;

  /**
   * Sets the pointer to the cluster's point sources
   *
   * @param i_cellToPointSources Contains mappings of 1 cell offset to m point sources
   * @param i_numberOfCellToPointSourcesMappings Size of i_cellToPointSources
   * @param i_pointSources pointer to all point sources used on this cluster
   */
  void setPointSources( sourceterm::CellToPointSourcesMapping const* i_cellToPointSources,
                        unsigned i_numberOfCellToPointSourcesMappings,
                        sourceterm::PointSources const* i_pointSources );

  void setReceiverCluster( kernels::ReceiverCluster* receiverCluster) {
    m_receiverCluster = receiverCluster;
  }

  void setFaultOutputManager(dr::output::OutputManager* outputManager) {
    faultOutputManager = outputManager;
  }

  /**
   * Set Tv constant for plasticity.
   */
  void setTv(double tv) {
    m_tv = tv;
    updateRelaxTime();
  }


  void reset() override;

  [[nodiscard]] unsigned int getClusterId() const;
  [[nodiscard]] unsigned int getGlobalClusterId() const;
  [[nodiscard]] LayerType getLayerType() const;
  void setReceiverTime(double receiverTime);

  static void collectivePredict(const std::vector<TimeCluster*>& clusters);
  static void collectiveCorrect(const std::vector<TimeCluster*>& clusters);

  static std::vector<TimeCluster*> collectMayPredict(const std::vector<TimeCluster*>& highPriority, const std::vector<TimeCluster*>& lowPriority, size_t threshold);
  static std::vector<TimeCluster*> collectMayCorrect(const std::vector<TimeCluster*>& highPriority, const std::vector<TimeCluster*>& lowPriority, size_t threshold);
};

#endif
