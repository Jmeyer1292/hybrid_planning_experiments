#include <hybrid_planning_common/conversions.h>
#include <hybrid_planning_common/load_environment.h>
#include <hybrid_planning_common/path_execution.h>
#include <hybrid_planning_common/path_types.h>
#include <hybrid_planning_common/path_visualization.h>
#include <hybrid_planning_common/abb_4600_kinematic_params.h>
#include <hybrid_planning_common/simple_hybrid_planner.h>

// Descartes
#include <descartes_light/position_sampler.h>
#include <descartes_light/rail_position_sampler.h>

#include <ros/ros.h>

using hybrid_planning_common::ToolPath;
using hybrid_planning_common::ToolPass;
using hybrid_planning_common::makeIrb4600_205_60;


ToolPath makePath(bool tilt = true)
{
  ToolPath path;

  // Reference Pose
  auto origin = Eigen::Isometry3d::Identity();
  origin.translation() = Eigen::Vector3d(0.5, 0, 0.55);

  const int n_passes = 5;

  // For each pass
  for (int r = 0; r < n_passes; ++r)
  {
     ToolPass this_pass;

     // For each pose in the pass
    for (int i = -10; i <= 10; ++i)
    {
      const double percent_along_pass = (10 + i) / 20.0;
      const double arc_height = std::sin(M_PI * percent_along_pass) * 0.5;
      // The last operation flips the pose around so +Z is into the part
      auto p = origin * Eigen::Translation3d(r * 0.1, i * 0.05, arc_height) * Eigen::AngleAxisd(M_PI, Eigen::Vector3d::UnitY());
      this_pass.push_back(p);
    }

    // For odd passes, we want the robot to move the other direction (so the robot doesn't "carriage return")
    if (r % 2 != 0)
    {
      std::reverse(this_pass.begin(), this_pass.end());
      // Also keep x along the direction of travel (important when tilting)
      for (auto& p : this_pass) p = p * Eigen::AngleAxisd(M_PI, Eigen::Vector3d::UnitZ());
    }

    path.push_back(this_pass);
  }

  // If the user wants it, tilt the poses so the tool "digs" into the surface
  const double tilt_angle = -10 * M_PI / 180.;
  if (tilt)
  {
    for (auto& pass : path)
      for (auto& p : pass)
        p = p * Eigen::AngleAxisd(tilt_angle, Eigen::Vector3d::UnitX()) * Eigen::Translation3d(0, 0.05, 0);
  }

  return path;
}

std::vector<std::vector<descartes_light::PositionSamplerPtr>>
makeSamplers(const ToolPath& path, descartes_light::CollisionInterfacePtr coll_env)
{
  // The current setup requires that our cartesian sampler is aware of the robot
  // kinematics
  opw_kinematics::Parameters<double> kin_params = makeIrb4600_205_60<double>();
  auto tip_to_tool = hybrid_planning_common::sanderTool0ToTCP();
  auto world_to_base = Eigen::Isometry3d::Identity() * Eigen::Translation3d(0.0, 0.0, 2.75) *
                       Eigen::AngleAxisd(M_PI, Eigen::Vector3d::UnitX());

  descartes_light::RailedKinematicsInterface kin_interface (kin_params, world_to_base, tip_to_tool);

  std::vector<std::vector<descartes_light::PositionSamplerPtr>> result (path.size());

  for (std::size_t i = 0; i < path.size(); ++i)
  {
    const auto& pass = path[i];
    for (const auto& pose : pass)
    {
      auto collision_clone = descartes_light::CollisionInterfacePtr(coll_env->clone());
      auto sampler = std::make_shared<descartes_light::RailedAxialSymmetricSampler>(pose, kin_interface, M_PI / 4.0,
                                                                                    collision_clone);
      result[i].push_back(std::move(sampler));
    }
  }

  return result;
}

trajopt::TrajOptProbPtr makeProblem(const hybrid_planning_common::EnvironmentDefinition& env,
                                    const hybrid_planning_common::ToolPass& pass,
                                    const hybrid_planning_common::JointPass& seed)
{
  trajopt::ProblemConstructionInfo pci (env.environment);

  // Populate Basic Info
  pci.basic_info.n_steps = pass.size();
  pci.basic_info.manip = env.group_name;
  pci.basic_info.start_fixed = false;

  // Create Kinematic Object
  pci.kin = pci.env->getManipulator(pci.basic_info.manip);
  const auto dof = pci.kin->numJoints();

  pci.init_info.type = trajopt::InitInfo::GIVEN_TRAJ;
  pci.init_info.data = hybrid_planning_common::jointTrajectoryToTrajopt(seed);

  // Populate Cost Info
  auto jv = std::make_shared<trajopt::JointVelTermInfo>();
  jv->coeffs = std::vector<double>(dof, 2.5);
  jv->name = "joint_vel";
  jv->term_type = trajopt::TT_COST;
  pci.cost_infos.push_back(jv);

  auto ja = std::make_shared<trajopt::JointAccTermInfo>();
  ja->coeffs = std::vector<double>(dof, 5.0);
  ja->name = "joint_acc";
  ja->term_type = trajopt::TT_COST;
  pci.cost_infos.push_back(ja);

  auto collision = std::make_shared<trajopt::CollisionTermInfo>();
  collision->name = "collision";
  collision->term_type = trajopt::TT_COST;
  collision->continuous = false;
  collision->first_step = 0;
  collision->last_step = pci.basic_info.n_steps - 1;
  collision->gap = 1;
  collision->info = trajopt::createSafetyMarginDataVector(pci.basic_info.n_steps, 0.025, 20);

  //  Apply a special cost between the sander_disks and the part
  for (auto& c : collision->info)
  {
    c->SetPairSafetyMarginData("sander_disk", "part", -0.01, 20.0);
    c->SetPairSafetyMarginData("sander_shaft", "part", 0.0, 20.0);
  }

  pci.cost_infos.push_back(collision);

  auto to_wxyz = [](const Eigen::Isometry3d& p) {
    Eigen::Quaterniond q (p.linear());
    Eigen::Vector4d wxyz;
    wxyz(0) = q.w();
    wxyz(1) = q.x();
    wxyz(2) = q.y();
    wxyz(3) = q.z();
    return wxyz;
  };

  // Populate Constraints
  for (std::size_t i = 0; i < pass.size(); ++i)
  {
    auto pose = std::make_shared<trajopt::CartPoseTermInfo>();
    pose->term_type = trajopt::TT_CNT;
    pose->name = "waypoint_cart_" + std::to_string(i);
    pose->link = "sander_tcp";
    pose->timestep = i;
    pose->xyz = pass[i].translation();
    pose->wxyz = to_wxyz(pass[i]);
    pose->pos_coeffs = Eigen::Vector3d(10, 10, 10);
    pose->rot_coeffs = Eigen::Vector3d(10, 10, 0);
    pci.cnt_infos.push_back(pose);
  }

  return trajopt::ConstructProblem(pci);
}

static bool addObject(tesseract::tesseract_ros::KDLEnv& env)
{
  auto obj = std::make_shared<tesseract::AttachableObject>();

  auto box = std::make_shared<shapes::Box>(1.0, 1.0, 1.0);

  obj->name = "part";
  obj->visual.shapes.push_back(box);
  obj->visual.shape_poses.push_back(Eigen::Isometry3d::Identity());
  obj->collision.shapes.push_back(box);
  obj->collision.shape_poses.push_back(Eigen::Isometry3d::Identity());
  obj->collision.collision_object_types.push_back(tesseract::CollisionObjectType::UseShapeType);

  // This call adds the object to the scene's "database" but does not actuall connect it
  env.addAttachableObject(obj);

  // To include the object in collision checks, you have to attach it
  tesseract::AttachedBodyInfo attached_body;
  attached_body.object_name = "part";
  attached_body.parent_link_name = "world_frame";
  attached_body.transform.setIdentity();
  attached_body.transform.translate(Eigen::Vector3d(1.0, 0, 0));

  env.attachBody(attached_body);
  return true;
}

int main(int argc, char** argv)
{
  ros::init(argc, argv, "workcell1_demo");
  ros::NodeHandle pnh;
  ros::AsyncSpinner spinner (1); spinner.start();

  // Stage 1: Load & Prepare Environment
  tesseract::tesseract_ros::KDLEnvPtr env;
  if (!hybrid_planning_common::loadEnvironment(env))
  {
    return 1;
  }

  addObject(*env);

  const std::string group_name = "manipulator_rail_tool";

  hybrid_planning_common::EnvironmentDefinition env_def;
  env_def.environment = env;
  env_def.group_name = group_name;

  // Stage 2: Define the problem
  hybrid_planning_common::PathDefinition path_def;
  path_def.path = makePath(true);
  path_def.speed = 0.2;

  // Visualize
  hybrid_planning_common::Republisher<geometry_msgs::PoseArray> pose_pub
      ("poses", hybrid_planning_common::toPoseArray(hybrid_planning_common::flatten(path_def.path), "world_frame"),
       ros::Rate(10));

  hybrid_planning_common::SamplerConfiguration sampler_config;
  auto collision_iface =
      std::make_shared<descartes_light::TesseractCollision>(env_def.environment, env_def.group_name);
  sampler_config.samplers = makeSamplers(path_def.path, collision_iface);

  hybrid_planning_common::OptimizationConfiguration optimizer_config;
  optimizer_config.problem_creator = makeProblem;

      // Stage 3: Apply the solvers
  hybrid_planning_common::ProblemDefinition problem;
  problem.env = env_def;
  problem.path = path_def;
  problem.sampler_config = sampler_config;
  problem.optimizer_config = optimizer_config;
  hybrid_planning_common::ProblemResult result = hybrid_planning_common::simpleHybridPlanner(problem);


  // Stage 4: Visualize the results
  if (result.succeeded)
  {
    if (result.sampled_traj)
    {
      std::cout << "Displaying sampled trajectory...\n";
      hybrid_planning_common::executeTrajectory(*result.sampled_traj);
      std::cout << "Sampled trajectory done\n";
    }

    if (result.optimized_traj)
    {
      std::cout << "Displaying optimized trajectory...\n";
      hybrid_planning_common::executeTrajectory(*result.optimized_traj);
      std::cout << "Optimized trajectory done\n";
    }
  }

  ros::waitForShutdown();
}
