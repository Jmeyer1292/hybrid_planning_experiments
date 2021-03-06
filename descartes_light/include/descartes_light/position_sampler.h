#ifndef DESCARTES_LIGHT_POSITION_SAMPLER_H
#define DESCARTES_LIGHT_POSITION_SAMPLER_H

#include "descartes_light/kinematic_interface.h"
#include "descartes_light/collision_checker.h"
#include <memory>

namespace descartes_light
{

// Section 1: Sampler
class PositionSampler
{
public:
  virtual ~PositionSampler() {}

  virtual bool sample(std::vector<double>& solution_set) = 0;
};

class CartesianPointSampler : public PositionSampler
{
public:
  CartesianPointSampler(const Eigen::Isometry3d& tool_pose,
                        const KinematicsInterface& robot_kin,
                        const CollisionInterfacePtr collision);

  bool sample(std::vector<double>& solution_set) override;

private:
  bool isCollisionFree(const double* vertex);

  Eigen::Isometry3d tool_pose_;
  KinematicsInterface kin_;
  CollisionInterfacePtr collision_;
};

class AxialSymmetricSampler : public PositionSampler
{
public:
  AxialSymmetricSampler(const Eigen::Isometry3d& tool_pose,
                        const KinematicsInterface& robot_kin,
                        const double radial_sample_resolution,
                        const CollisionInterfacePtr collision);

  bool sample(std::vector<double>& solution_set) override;

private:
  bool isCollisionFree(const double* vertex);

  Eigen::Isometry3d tool_pose_;
  KinematicsInterface kin_;
  CollisionInterfacePtr collision_;
  double radial_sample_res_;
};

using PositionSamplerPtr = std::shared_ptr<PositionSampler>;


}

#endif // DESCARTES_LIGHT_POSITION_SAMPLER_H
