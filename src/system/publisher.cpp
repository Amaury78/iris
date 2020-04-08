#include "vllm/system/publisher.hpp"

namespace vllm
{
bool Publisher::pop(Publication& p)
{
  std::lock_guard<std::mutex> lock(mtx);

  if (flag[(id + 1) % 2] == false) {
    return false;
  }

  p = publication[(id + 1) % 2];
  flag[(id + 1) % 2] = false;
  return true;
}

// NOTE: There are many redundant copies
void Publisher::push(
    const Eigen::Matrix4f& T_align,
    const Eigen::Matrix4f& vllm_camera,
    const Eigen::Matrix4f& offset_camera,
    const KeypointsWithNormal& raw_keypoints,
    const std::vector<Eigen::Vector3f>& vllm_trajectory,
    const std::vector<Eigen::Vector3f>& offset_trajectory,
    const pcl::CorrespondencesPtr& corre,
    const map::Info& localmap_info)
{
  Publication& tmp = publication[id];

  tmp.vllm_camera = normalizePose(vllm_camera);
  tmp.offset_camera = normalizePose(offset_camera);
  tmp.vllm_trajectory = vllm_trajectory;
  tmp.offset_trajectory = offset_trajectory;
  tmp.localmap_info = localmap_info;
  *tmp.correspondences = *corre;

  pcl::transformPointCloud(*raw_keypoints.cloud, *tmp.cloud, T_align);
  vllm::transformNormals(*raw_keypoints.normals, *tmp.normals, T_align);

  {
    std::lock_guard<std::mutex> lock(mtx);
    flag[id] = true;
    id = (id + 1) % 2;
  }
}
}  // namespace vllm