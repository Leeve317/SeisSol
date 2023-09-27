#ifndef SEISSOL_GPU_SLOWVELOCITYWEAKENINGLAW_H
#define SEISSOL_GPU_SLOWVELOCITYWEAKENINGLAW_H

#include "DynamicRupture/FrictionLaws/GpuImpl/RateAndState.h"

namespace seissol::dr::friction_law::gpu {
template <class Derived, class TPMethod>
class SlowVelocityWeakeningLaw
    : public RateAndStateBase<SlowVelocityWeakeningLaw<Derived, TPMethod>, TPMethod> {
  public:
  using RateAndStateBase<SlowVelocityWeakeningLaw, TPMethod>::RateAndStateBase;

  void copyLtsTreeToLocal(seissol::initializers::Layer& layerData,
                          seissol::initializers::DynamicRupture const* const dynRup,
                          real fullUpdateTime) {}

  // Note that we need double precision here, since single precision led to NaNs.
  void updateStateVariable(double timeIncrement) {
    static_cast<Derived*>(this)->updateStateVariable(timeIncrement);
  }

  #pragma omp declare target
  struct Details {
    decltype(SlowVelocityWeakeningLaw::a) a;
    decltype(SlowVelocityWeakeningLaw::sl0) sl0;
    decltype(dr::DRParameters::rsSr0) rsSr0;
    decltype(dr::DRParameters::rsF0) rsF0;
    decltype(dr::DRParameters::rsB) rsB;
  };

  Details getCurrentLtsLayerDetails() {
    Details details{};
    details.a = this->a;
    details.sl0 = this->sl0;
    details.rsSr0 = this->drParameters->rsSr0;
    details.rsF0 = this->drParameters->rsF0;
    details.rsB = this->drParameters->rsB;
    return details;
  }

  static double updateMu(double localSlipRateMagnitude,
                         double localStateVariable,
                         Details details,
                         size_t ltsFace,
                         size_t pointIndex) {
    const double localA = details.a[ltsFace][pointIndex];
    const double localSl0 = details.sl0[ltsFace][pointIndex];
    const double log1 = std::log(details.rsSr0 * localStateVariable / localSl0);
    const double x = 0.5 * (localSlipRateMagnitude / details.rsSr0) *
                     std::exp((details.rsF0 + details.rsB * log1) / localA);
    return localA * std::asinh(x);
  }

  static double updateMuDerivative(double localSlipRateMagnitude,
                                   double localStateVariable,
                                   Details details,
                                   size_t ltsFace,
                                   size_t pointIndex) {
    const double localA = details.a[ltsFace][pointIndex];
    const double localSl0 = details.sl0[ltsFace][pointIndex];
    const double log1 = std::log(details.rsSr0 * localStateVariable / localSl0);
    const double c =
        (0.5 / details.rsSr0) * std::exp((details.rsF0 + details.rsB * log1) / localA);
    return localA * c / std::sqrt(std::pow(localSlipRateMagnitude * c, 2) + 1.0);
  }

  /**
   * Resample the state variable. For Slow Velocity Weakening Laws,
   * we just copy the buffer into the member variable.
   */
  void resampleStateVar(real (*stateVariableBuffer)[misc::numPaddedPoints]) {
    const auto layerSize{this->currLayerSize};
    auto* stateVariable{this->stateVariable};

    #pragma omp distribute
      for (int ltsFace = 0; ltsFace < this->currLayerSize; ++ltsFace) {
        #pragma omp parallel for schedule(static, 1)
        for (int pointIndex = 0; pointIndex < misc::numPaddedPoints; ++pointIndex) {

        stateVariable[ltsFace][pointIndex] = stateVariableBuffer[ltsFace][pointIndex];
      }
    }
  }

  void executeIfNotConverged() {}
  #pragma omp end declare target
};
} // namespace seissol::dr::friction_law::gpu

#endif // SEISSOL_GPU_SLOWVELOCITYWEAKENINGLAW_H
