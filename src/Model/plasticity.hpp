#pragma once

#include <Kernels/precision.hpp>
#include "Model/common_datastructures.hpp"
#include <cmath>
#include <string>

namespace seissol::model {
// plasticity information per cell
template <typename Real_t = real>
struct PlasticityData {
  // initial loading (stress tensor)
  Real_t initialLoading[6];
  Real_t cohesionTimesCosAngularFriction;
  Real_t sinAngularFriction;
  Real_t mufactor;

  PlasticityData(const Plasticity& plasticity, const Material* material) {
    initialLoading[0] = plasticity.s_xx;
    initialLoading[1] = plasticity.s_yy;
    initialLoading[2] = plasticity.s_zz;
    initialLoading[3] = plasticity.s_xy;
    initialLoading[4] = plasticity.s_yz;
    initialLoading[5] = plasticity.s_xz;

    const double angularFriction = std::atan(plasticity.bulkFriction);

    cohesionTimesCosAngularFriction = plasticity.plastCo * std::cos(angularFriction);
    sinAngularFriction = std::sin(angularFriction);

    mufactor = 1.0 / (2.0 * material->getMu());
  }

  static constexpr std::size_t NumberOfQuantities = 7;
  static constexpr std::size_t NumberPerMechanism = 0;
  static const inline std::string Text = "plasticity";
};

} // namespace seissol::model
