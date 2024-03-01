// Copyright (c) 2008, Willow Garage, Inc.
// All rights reserved.
//
// Software License Agreement (BSD License 2.0)
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above
//    copyright notice, this list of conditions and the following
//    disclaimer in the documentation and/or other materials provided
//    with the distribution.
//  * Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
// FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
// COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
// BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
// ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include <limits>
#include <memory>
#include <string>
#include <math.h>

#include "image_geometry/stereo_camera_model.h"
#include "message_filters/subscriber.h"
#include "message_filters/synchronizer.h"
#include "message_filters/sync_policies/approximate_time.h"
#include "message_filters/sync_policies/exact_time.h"
#include "rcutils/logging_macros.h"

#include <image_transport/image_transport.hpp>
#include <image_transport/subscriber_filter.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp_components/register_node_macro.hpp>
#include <sensor_msgs/image_encodings.hpp>
#include <sensor_msgs/msg/point_cloud2.hpp>
#include <sensor_msgs/point_cloud2_iterator.hpp>
#include <stereo_msgs/msg/disparity_image.hpp>

namespace stereo_image_processing
{

class PointCloudNode : public rclcpp::Node
{
public:
  explicit PointCloudNode(const rclcpp::NodeOptions & options);

private:
  // Subscriptions
  image_transport::SubscriberFilter sub_l_image_;
  message_filters::Subscriber<sensor_msgs::msg::CameraInfo> sub_l_info_, sub_r_info_;
  message_filters::Subscriber<stereo_msgs::msg::DisparityImage> sub_disparity_;
  using ExactPolicy = message_filters::sync_policies::ExactTime<
    sensor_msgs::msg::Image,
    sensor_msgs::msg::CameraInfo,
    sensor_msgs::msg::CameraInfo,
    stereo_msgs::msg::DisparityImage>;
  using ApproximatePolicy = message_filters::sync_policies::ApproximateTime<
    sensor_msgs::msg::Image,
    sensor_msgs::msg::CameraInfo,
    sensor_msgs::msg::CameraInfo,
    stereo_msgs::msg::DisparityImage>;
  using ExactSync = message_filters::Synchronizer<ExactPolicy>;
  using ApproximateSync = message_filters::Synchronizer<ApproximatePolicy>;
  std::shared_ptr<ExactSync> exact_sync_;
  std::shared_ptr<ApproximateSync> approximate_sync_;

  // Publications
  std::shared_ptr<rclcpp::Publisher<sensor_msgs::msg::PointCloud2>> pub_points2_;

  // Processing state (note: only safe because we're single-threaded!)
  image_geometry::StereoCameraModel model_;
  cv::Mat_<cv::Vec3f> points_mat_;  // scratch buffer

  std::string point_cloud_frame_;

  int image_height_;
  int image_width_;
  double distance_threshold_;

  inline bool distancePoint(const cv::Vec3f & pt);

  void connectCb();

  void imageCb(
    const sensor_msgs::msg::Image::ConstSharedPtr & l_image_msg,
    const sensor_msgs::msg::CameraInfo::ConstSharedPtr & l_info_msg,
    const sensor_msgs::msg::CameraInfo::ConstSharedPtr & r_info_msg,
    const stereo_msgs::msg::DisparityImage::ConstSharedPtr & disp_msg);
};

PointCloudNode::PointCloudNode(const rclcpp::NodeOptions & options)
: rclcpp::Node("point_cloud_node", options)
{
  using namespace std::placeholders;

  // Declare/read parameters
  int queue_size = this->declare_parameter("queue_size", 5);
  bool approx = this->declare_parameter("approximate_sync", false);
  this->declare_parameter("use_system_default_qos", false);
  rcl_interfaces::msg::ParameterDescriptor descriptor;
  // TODO(ivanpauno): Confirm if using point cloud padding in `sensor_msgs::msg::PointCloud2`
  // can improve performance in some cases or not.
  descriptor.description =
    "This parameter avoids using alignment padding in the generated point cloud."
    "This reduces bandwidth requirements, as the point cloud size is halved."
    "Using point clouds without alignment padding might degrade performance for some algorithms.";
  this->declare_parameter("avoid_point_cloud_padding", false, descriptor);
  this->declare_parameter("use_color", true);
  this->declare_parameter("image_width", 640);
  this->declare_parameter("image_height", 480);
  distance_threshold_ = this->declare_parameter("distance_threshold", 400.0);
  point_cloud_frame_ = this->declare_parameter("point_cloud_frame", "camera_frame");

  // Synchronize callbacks
  if (approx) {
    approximate_sync_.reset(
      new ApproximateSync(
        ApproximatePolicy(queue_size),
        sub_l_image_, sub_l_info_,
        sub_r_info_, sub_disparity_));
    approximate_sync_->registerCallback(
      std::bind(&PointCloudNode::imageCb, this, _1, _2, _3, _4));
  } else {
    exact_sync_.reset(
      new ExactSync(
        ExactPolicy(queue_size),
        sub_l_image_, sub_l_info_,
        sub_r_info_, sub_disparity_));
    exact_sync_->registerCallback(
      std::bind(&PointCloudNode::imageCb, this, _1, _2, _3, _4));
  }

  // Update the publisher options to allow reconfigurable qos settings.
  rclcpp::PublisherOptions pub_opts;
  pub_opts.qos_overriding_options = rclcpp::QosOverridingOptions::with_default_policies();
  pub_points2_ = create_publisher<sensor_msgs::msg::PointCloud2>("points2", 1, pub_opts);

  // TODO(jacobperron): Replace this with a graph event.
  //                    Only subscribe if there's a subscription listening to our publisher.
  connectCb();
}

// Handles (un)subscribing when clients (un)subscribe
void PointCloudNode::connectCb()
{
  // TODO(jacobperron): Add unsubscribe logic when we use graph events
  image_transport::TransportHints hints(this, "raw");
  const bool use_system_default_qos = this->get_parameter("use_system_default_qos").as_bool();
  rclcpp::QoS image_sub_qos = rclcpp::SensorDataQoS();
  if (use_system_default_qos) {
    image_sub_qos = rclcpp::SystemDefaultsQoS();
  }
  const auto image_sub_rmw_qos = image_sub_qos.get_rmw_qos_profile();
  auto sub_opts = rclcpp::SubscriptionOptions();
  sub_opts.qos_overriding_options = rclcpp::QosOverridingOptions::with_default_policies();
  sub_l_image_.subscribe(
    this, "left/image_rect_color", hints.getTransport(), image_sub_rmw_qos, sub_opts);
  sub_l_info_.subscribe(this, "left/camera_info", image_sub_rmw_qos, sub_opts);
  sub_r_info_.subscribe(this, "right/camera_info", image_sub_rmw_qos, sub_opts);
  sub_disparity_.subscribe(this, "disparity", image_sub_rmw_qos, sub_opts);
}

inline bool isValidPoint(const cv::Vec3f & pt)
{
  // Check both for disparities explicitly marked as invalid (where OpenCV maps pt.z to MISSING_Z)
  // and zero disparities (point mapped to infinity).
  return (pt[2] != image_geometry::StereoCameraModel::MISSING_Z && !std::isinf(pt[2]));
}

inline bool PointCloudNode::distancePoint(const cv::Vec3f & pt)
{
  // Check both for disparities explicitly marked as invalid (where OpenCV maps pt.z to MISSING_Z)
  // and zero disparities (point mapped to infinity).
  return (sqrt(pow(pt[0], 2) + pow(pt[1], 2) + pow(pt[2], 2)) <= distance_threshold_);
}

void PointCloudNode::imageCb(
  const sensor_msgs::msg::Image::ConstSharedPtr & l_image_msg,
  const sensor_msgs::msg::CameraInfo::ConstSharedPtr & l_info_msg,
  const sensor_msgs::msg::CameraInfo::ConstSharedPtr & r_info_msg,
  const stereo_msgs::msg::DisparityImage::ConstSharedPtr & disp_msg)
{
  // If there are no subscriptions for the point cloud, do nothing
  if (pub_points2_->get_subscription_count() == 0u) {
    return;
  }

  image_height_ = this->get_parameter("image_height").as_int();
  image_width_ = this->get_parameter("image_width").as_int();

  sensor_msgs::msg::CameraInfo l_info_msg_c;
  sensor_msgs::msg::CameraInfo r_info_msg_c;

  l_info_msg_c.header = l_info_msg->header;
  l_info_msg_c.distortion_model= l_info_msg->distortion_model;
  l_info_msg_c.d = l_info_msg->d;
  l_info_msg_c.k = l_info_msg->k;
  l_info_msg_c.r = l_info_msg->r;
  l_info_msg_c.p = l_info_msg->p;
  l_info_msg_c.binning_x = l_info_msg->binning_x;
  l_info_msg_c.binning_y = l_info_msg->binning_y;
  l_info_msg_c.roi = l_info_msg->roi;

  r_info_msg_c.header = r_info_msg->header;
  r_info_msg_c.header.frame_id = l_info_msg_c.header.frame_id;
  r_info_msg_c.distortion_model= r_info_msg->distortion_model;
  r_info_msg_c.d = r_info_msg->d;
  r_info_msg_c.k = r_info_msg->k;
  r_info_msg_c.r = r_info_msg->r;
  r_info_msg_c.p = r_info_msg->p;
  // r_info_msg_c.p[3] = 34.999;
  r_info_msg_c.binning_x = r_info_msg->binning_x;
  r_info_msg_c.binning_y = r_info_msg->binning_y;
  r_info_msg_c.roi = r_info_msg->roi;
  
  l_info_msg_c.height = image_height_;
  l_info_msg_c.width = image_width_;
  r_info_msg_c.height = image_height_;
  r_info_msg_c.width = image_width_;

  double scale_x = (double)image_width_ / l_info_msg->width;
  double scale_y = (double)image_height_ / l_info_msg->height;

  l_info_msg_c.k[0] *= scale_x;
  l_info_msg_c.k[2] *= scale_x;
  l_info_msg_c.k[4] *= scale_y;
  l_info_msg_c.k[5] *= scale_y;
  l_info_msg_c.p[0] *= scale_x;
  l_info_msg_c.p[2] *= scale_x;
  l_info_msg_c.p[5] *= scale_y;
  l_info_msg_c.p[6] *= scale_y;

  r_info_msg_c.k[0] *= scale_x;
  r_info_msg_c.k[2] *= scale_x;
  r_info_msg_c.k[4] *= scale_y;
  r_info_msg_c.k[5] *= scale_y;
  r_info_msg_c.p[0] *= scale_x;
  r_info_msg_c.p[2] *= scale_x;
  r_info_msg_c.p[5] *= scale_y;
  r_info_msg_c.p[6] *= scale_y;

  sensor_msgs::msg::CameraInfo::SharedPtr l_info_msg_ptr;
  sensor_msgs::msg::CameraInfo::SharedPtr r_info_msg_ptr;
  l_info_msg_ptr = std::make_shared<sensor_msgs::msg::CameraInfo>(l_info_msg_c);
  r_info_msg_ptr = std::make_shared<sensor_msgs::msg::CameraInfo>(r_info_msg_c);

  // Update the camera model
  model_.fromCameraInfo(l_info_msg_ptr, r_info_msg_ptr);

  // Calculate point cloud
  const sensor_msgs::msg::Image & dimage = disp_msg->image;
  // The cv::Mat_ constructor doesn't accept a const data data pointer
  // so we remove the constness before reinterpreting into float.
  // This is "safe" since our cv::Mat is const.
  float * data = reinterpret_cast<float *>(const_cast<uint8_t *>(&dimage.data[0]));

  const cv::Mat_<float> dmat(dimage.height, dimage.width, data, dimage.step);
  model_.projectDisparityImageTo3d(dmat, points_mat_, true);
  cv::Mat_<cv::Vec3f> mat = points_mat_;

  // Fill in new PointCloud2 message (2D image-like layout)
  auto points_msg = std::make_shared<sensor_msgs::msg::PointCloud2>();
  points_msg->header = disp_msg->header;
  points_msg->height = mat.rows;
  points_msg->width = mat.cols;
  points_msg->is_bigendian = false;
  points_msg->is_dense = false;  // there may be invalid points

  // Set frame_id
  if (point_cloud_frame_.size() > 0)
  {
    points_msg->header.frame_id = point_cloud_frame_;
  }

  sensor_msgs::PointCloud2Modifier pcd_modifier(*points_msg);

  if (!this->get_parameter("avoid_point_cloud_padding").as_bool()) {
    if (this->get_parameter("use_color").as_bool()) {
      // Data will be packed as (DC=don't care, each item is a float):
      //   x, y, z, DC, rgb, DC, DC, DC
      // Resulting step size: 32 bytes
      pcd_modifier.setPointCloud2FieldsByString(2, "xyz", "rgb");
    } else {
      // Data will be packed as:
      //   x, y, z, DC
      // Resulting step size: 16 bytes
      pcd_modifier.setPointCloud2FieldsByString(1, "xyz");
    }
  } else {
    if (this->get_parameter("use_color").as_bool()) {
      // Data will be packed as:
      //   x, y, z, rgb
      // Resulting step size: 16 bytes
      pcd_modifier.setPointCloud2Fields(
        4,
        "x", 1, sensor_msgs::msg::PointField::FLOAT32,
        "y", 1, sensor_msgs::msg::PointField::FLOAT32,
        "z", 1, sensor_msgs::msg::PointField::FLOAT32,
        "rgb", 1, sensor_msgs::msg::PointField::FLOAT32);
    } else {
      // Data will be packed as:
      //   x, y, z
      // Resulting step size: 12 bytes
      pcd_modifier.setPointCloud2Fields(
        3,
        "x", 1, sensor_msgs::msg::PointField::FLOAT32,
        "y", 1, sensor_msgs::msg::PointField::FLOAT32,
        "z", 1, sensor_msgs::msg::PointField::FLOAT32);
    }
  }

  sensor_msgs::PointCloud2Iterator<float> iter_x(*points_msg, "x");
  sensor_msgs::PointCloud2Iterator<float> iter_y(*points_msg, "y");
  sensor_msgs::PointCloud2Iterator<float> iter_z(*points_msg, "z");

  float bad_point = std::numeric_limits<float>::quiet_NaN();
  for (int v = 0; v < mat.rows; ++v) {
    for (int u = 0; u < mat.cols; ++u, ++iter_x, ++iter_y, ++iter_z) {
      if (isValidPoint(mat(v, u)) && distancePoint(mat(v, u))) {
        *iter_x = mat(v, u)[0];
        *iter_y = mat(v, u)[1];
        *iter_z = mat(v, u)[2];
      } else {
        *iter_x = *iter_y = *iter_z = bad_point;
      }
    }
  }

  if (this->get_parameter("use_color").as_bool()) {
    sensor_msgs::PointCloud2Iterator<uint8_t> iter_r(*points_msg, "r");
    sensor_msgs::PointCloud2Iterator<uint8_t> iter_g(*points_msg, "g");
    sensor_msgs::PointCloud2Iterator<uint8_t> iter_b(*points_msg, "b");

    // Fill in color
    namespace enc = sensor_msgs::image_encodings;
    const std::string & encoding = l_image_msg->encoding;
    if (encoding == enc::MONO8) {
      const cv::Mat_<uint8_t> color_original(
        l_image_msg->height, l_image_msg->width,
        const_cast<uint8_t *>(&l_image_msg->data[0]),
        l_image_msg->step);
        cv::Mat_<uint8_t> color;
        cv::resize(color_original, color, cv::Size(image_width_, image_height_), cv::INTER_LINEAR);
      for (int v = 0; v < mat.rows; ++v) {
        for (int u = 0; u < mat.cols; ++u, ++iter_r, ++iter_g, ++iter_b) {
          uint8_t g = color(v, u);
          *iter_r = *iter_g = *iter_b = g;
        }
      }
    } else if (encoding == enc::RGB8) {
      const cv::Mat_<cv::Vec3b> color_original(
        l_image_msg->height, l_image_msg->width,
        (cv::Vec3b *)(&l_image_msg->data[0]),
        l_image_msg->step);
        cv::Mat_<uint8_t> color;
        cv::resize(color_original, color, cv::Size(image_width_, image_height_), cv::INTER_LINEAR);
      for (int v = 0; v < mat.rows; ++v) {
        for (int u = 0; u < mat.cols; ++u, ++iter_r, ++iter_g, ++iter_b) {
          const cv::Vec3b & rgb = color(v, u);
          *iter_r = rgb[0];
          *iter_g = rgb[1];
          *iter_b = rgb[2];
        }
      }
    } else if (encoding == enc::BGR8) {
      const cv::Mat_<cv::Vec3b> color_original(
        l_image_msg->height, l_image_msg->width,
        (cv::Vec3b *)(&l_image_msg->data[0]),
        l_image_msg->step);
        cv::Mat_<uint8_t> color;
        cv::resize(color_original, color, cv::Size(image_width_, image_height_), cv::INTER_LINEAR);
      for (int v = 0; v < mat.rows; ++v) {
        for (int u = 0; u < mat.cols; ++u, ++iter_r, ++iter_g, ++iter_b) {
          const cv::Vec3b & bgr = color(v, u);
          *iter_r = bgr[2];
          *iter_g = bgr[1];
          *iter_b = bgr[0];
        }
      }
    } else {
      // Throttle duration in milliseconds
      RCUTILS_LOG_WARN_THROTTLE(
        RCUTILS_STEADY_TIME, 30000,
        "Could not fill color channel of the point cloud, "
        "unsupported encoding '%s'", encoding.c_str());
    }
  }

  pub_points2_->publish(*points_msg);
}

}  // namespace stereo_image_processing

// Register node
RCLCPP_COMPONENTS_REGISTER_NODE(stereo_image_processing::PointCloudNode)