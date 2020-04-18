#include "vllm/bridge/bridge.hpp"
#include "vllm/core/config.hpp"
#include "vllm/map/map.hpp"
#include "vllm/system/system.hpp"
#include "vllm/viewer/pangolin_viewer.hpp"
#include "vllm_ros/communication.hpp"
#include <chrono>
#include <cv_bridge/cv_bridge.h>
#include <fstream>
#include <image_transport/image_transport.h>
#include <opencv2/opencv.hpp>
#include <popl.hpp>
#include <ros/ros.h>
#include <tf/transform_broadcaster.h>
#include <visualization_msgs/Marker.h>

int main(int argc, char* argv[])
{
  // Initialzie ROS & subscriber
  ros::init(argc, argv, "vllm_node");

  // Analyze arugments
  popl::OptionParser op("Allowed options");
  auto config_file_path = op.add<popl::Value<std::string>>("c", "config", "config file path");
  try {
    op.parse(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << e.what() << std::endl;
    exit(EXIT_FAILURE);
  }
  if (!config_file_path->is_set()) {
    std::cerr << "invalid arguments" << std::endl;
    std::cout << op.help() << std::endl;
    exit(EXIT_FAILURE);
  }

  // Setup subscriber
  ros::NodeHandle nh;
  image_transport::ImageTransport it(nh);
  cv::Mat subscribed_image;
  image_transport::Subscriber image_subscriber = it.subscribe("camera/color/image_raw", 1, vllm_ros::imageCallbackGenerator(subscribed_image));

  // Setup publisher
  ros::Publisher target_pc_publisher = nh.advertise<pcl::PointCloud<pcl::PointXYZI>>("vllm/target_pointcloud", 1);
  ros::Publisher source_pc_publisher = nh.advertise<pcl::PointCloud<pcl::PointXYZI>>("vllm/source_pointcloud", 1);
  ros::Publisher vllm_trajectory_publisher = nh.advertise<visualization_msgs::Marker>("vllm/vllm_trajectory", 1);
  ros::Publisher vslam_trajectory_publisher = nh.advertise<visualization_msgs::Marker>("vllm/vslam_trajectory", 1);
  ros::Publisher correspondences_publisher = nh.advertise<visualization_msgs::Marker>("vllm/correspondences", 1);
  image_transport::Publisher image_publisher = it.advertise("vllm/image", 1);
  vllm::Publication publication;

  // Initialize config
  vllm::Config config(config_file_path->value());

  // Load LiDAR map
  vllm::map::Parameter map_param(
      config.pcd_file, config.voxel_grid_leaf, config.normal_search_leaf, config.submap_grid_leaf);
  std::shared_ptr<vllm::map::Map> map = std::make_shared<vllm::map::Map>(map_param);

  // Initialize system
  std::shared_ptr<vllm::System> system = std::make_shared<vllm::System>(config, map);
  std::chrono::system_clock::time_point m_start;

  // Setup for OpenVSLAM
  vllm::pcXYZ::Ptr vslam_points(new vllm::pcXYZ);
  vllm::pcNormal::Ptr vslam_normals(new vllm::pcNormal);
  std::vector<float> vslam_weights;
  vllm::BridgeOpenVSLAM bridge;
  bridge.setup(config);

  ros::Rate loop_10Hz(10);
  int loop_count = 0;
  float accuracy = 0.5f;

  // Main loop
  while (ros::ok()) {
    if (!subscribed_image.empty()) {
      m_start = std::chrono::system_clock::now();

      // process OpenVSLAM
      bridge.execute(subscribed_image);
      bridge.setCriteria(30, accuracy);
      bridge.getLandmarksAndNormals(vslam_points, vslam_normals, vslam_weights);
      subscribed_image = cv::Mat();  // Reset input

      // Update threshold to adjust the number of points
      if (vslam_points->size() < 300 && accuracy > 0.10) accuracy -= 0.01f;
      if (vslam_points->size() > 500 && accuracy < 0.90) accuracy += 0.01f;

      // Execution
      system->execute(
          bridge.getState(),
          bridge.getCameraPose().inverse(),
          vslam_points,
          vslam_normals,
          vslam_weights);

      // Publish for rviz
      system->popPublication(publication);
      vllm_ros::publishImage(image_publisher, bridge.getFrame());
      vllm_ros::publishPointcloud(source_pc_publisher, publication.cloud);
      vllm_ros::publishTrajectory(vllm_trajectory_publisher, publication.vllm_trajectory, 0);
      vllm_ros::publishTrajectory(vslam_trajectory_publisher, publication.offset_trajectory, 1);
      vllm_ros::publishCorrespondences(correspondences_publisher, publication.cloud, map->getTargetCloud(), publication.correspondences);
      vllm_ros::publishPose(publication.offset_camera, "vslam_pose");
      vllm_ros::publishPose(publication.vllm_camera, "vllm_pose");

      // Inform processing time
      std::stringstream ss;
      ss << "processing time= \033[35m"
         << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - m_start).count()
         << "\033[m ms";
      ROS_INFO("%s", ss.str().c_str());
    }

    // Publish target pointcloud map
    if (++loop_count >= 50) {
      loop_count = 0;
      vllm_ros::publishPointcloud(target_pc_publisher, map->getTargetCloud());
    }

    // Spin and wait
    ros::spinOnce();
    loop_10Hz.sleep();
  }

  ROS_INFO("Finalize the system");
  return 0;
}