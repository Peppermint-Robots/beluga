// Copyright 2023 Ekumen, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef BELUGA_MOTION_OMNIDIRECTIONAL_DRIVE_MODEL_HPP
#define BELUGA_MOTION_OMNIDIRECTIONAL_DRIVE_MODEL_HPP

#include <optional>
#include <random>
#include <tuple>
#include <type_traits>

#include <beluga/type_traits/tuple_traits.hpp>

#include <sophus/se2.hpp>
#include <sophus/so2.hpp>

/**
 * \file
 * \brief Implementation of an omnidirectional drive odometry motion model.
 */

namespace beluga {

/// Parameters to construct an OmnidirectionalDriveModel instance.
struct OmnidirectionalDriveModelParam {
  /// Rotational noise from rotation
  /**
   * How much rotational noise is generated by the relative rotation between the last two odometry updates.
   * Also known as `alpha1`.
   */
  double rotation_noise_from_rotation;
  /// Rotational noise from translation
  /**
   * How much rotational noise is generated by the relative translation between the last two odometry updates.
   * Also known as `alpha2`.
   */
  double rotation_noise_from_translation;
  /// Translational noise from translation
  /**
   * How much translational in longitudinal noise is generated by the relative translation between
   * the last two odometry updates.
   * Also known as `alpha3`.
   */
  double translation_noise_from_translation;
  /// Translational noise from rotation
  /**
   * How much translational noise is generated by the relative rotation between the last two odometry updates.
   * Also known as `alpha4`.
   */
  double translation_noise_from_rotation;
  /// Translational strafe noise from translation
  /**
   * How much translational noise in strafe is generated by the relative translation between the last two
   * odometry updates.
   * Also known as `alpha5`.
   */
  double strafe_noise_from_translation;

  /// Distance threshold to detect in-place rotation.
  double distance_threshold = 0.01;
};

/// Sampled odometry model for an omnidirectional drive.
/**
 * This class satisfies \ref MotionModelPage.
 */
class OmnidirectionalDriveModel {
 public:
  /// Current and previous odometry estimates as motion model control action.
  using control_type = std::tuple<Sophus::SE2d, Sophus::SE2d>;
  /// 2D pose as motion model state (to match that of the particles).
  using state_type = Sophus::SE2d;

  /// Parameter type that the constructor uses to configure the motion model.
  using param_type = OmnidirectionalDriveModelParam;

  /// Constructs an OmnidirectionalDriveModel instance.
  /**
   * \param params Parameters to configure this instance.
   *  See beluga::OmnidirectionalDriveModelParam for details.
   */
  explicit OmnidirectionalDriveModel(const param_type& params) : params_{params} {}

  /// Computes a state sampling function conditioned on a given control action.
  /**
   * \tparam Control A tuple-like container matching the model's `control_action_type`.
   * \param action Control action to condition the motion model with.
   * \return a callable satisfying \ref StateSamplingFunctionPage.
   */
  template <class Control, typename = common_tuple_type_t<Control, control_type>>
  [[nodiscard]] auto operator()(Control&& action) const {
    const auto& [pose, previous_pose] = action;

    const auto translation = pose.translation() - previous_pose.translation();
    const double distance = translation.norm();
    const double distance_variance = distance * distance;

    const auto& previous_orientation = previous_pose.so2();
    const auto& current_orientation = pose.so2();
    const auto rotation = current_orientation * previous_orientation.inverse();

    const auto heading_rotation = Sophus::SO2d{std::atan2(translation.y(), translation.x())};
    const auto first_rotation =
        distance > params_.distance_threshold ? heading_rotation * previous_orientation.inverse() : Sophus::SO2d{};

    using DistributionParam = typename std::normal_distribution<double>::param_type;
    const auto rotation_params = DistributionParam{
        rotation.log(), std::sqrt(
                            params_.rotation_noise_from_rotation * rotation_variance(rotation) +
                            params_.rotation_noise_from_translation * distance_variance)};

    const auto translation_params = DistributionParam{
        distance, std::sqrt(
                      params_.translation_noise_from_translation * distance_variance +
                      params_.translation_noise_from_rotation * rotation_variance(rotation))};

    const auto strafe_params = DistributionParam{
        0.0, std::sqrt(
                 params_.strafe_noise_from_translation * distance_variance +
                 params_.translation_noise_from_rotation * rotation_variance(rotation))};

    return [=](const state_type& state, auto& gen) {
      static thread_local auto distribution = std::normal_distribution<double>{};
      // This is an implementation based on the same set of parameters that is used in
      // nav2's omni_motion_model. To simplify the implementation, the following
      // variable substitutions were performed:
      // - first_rotation = delta_bearing - previous_orientation
      // - second_rotation = delta_rot_hat - first_rotation
      const auto second_rotation = Sophus::SO2d{distribution(gen, rotation_params)} * first_rotation.inverse();
      const auto translation =
          Eigen::Vector2d{distribution(gen, translation_params), -distribution(gen, strafe_params)};
      return state * Sophus::SE2d{first_rotation, Eigen::Vector2d{0.0, 0.0}} *
             Sophus::SE2d{second_rotation, translation};
    };
  }

 private:
  param_type params_;

  static double rotation_variance(const Sophus::SO2d& rotation) {
    // Treat backward and forward motion symmetrically for the noise models.
    const auto flipped_rotation = rotation * Sophus::SO2d{Sophus::Constants<double>::pi()};
    const auto delta = std::min(std::abs(rotation.log()), std::abs(flipped_rotation.log()));
    return delta * delta;
  }
};

}  // namespace beluga

#endif
