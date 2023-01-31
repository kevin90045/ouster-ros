/**
 * Copyright (c) 2018-2023, Ouster, Inc.
 * All rights reserved.
 *
 * @file os_ros.cpp
 * @brief A utilty file that contains helper methods
 */

// prevent clang-format from altering the location of "ouster_ros/ros.h", the
// header file needs to be the first include due to PCL_NO_PRECOMPILE flag
// clang-format off
#include "ouster_ros/os_ros.h"
// clang-format on

#include <pcl_conversions/pcl_conversions.h>
#include <tf2/LinearMath/Transform.h>

// TODO: once foxy is deprecated switch tf2_eigen.hpp
#include <tf2_eigen/tf2_eigen.h>

#include <chrono>
#include <string>
#include <vector>

namespace sensor = ouster::sensor;
using ouster_msgs::msg::PacketMsg;

namespace ouster_ros {

bool read_imu_packet(const sensor::client& cli, PacketMsg& pm,
                     const sensor::packet_format& pf) {
    pm.buf.resize(pf.imu_packet_size + 1);
    return read_imu_packet(cli, pm.buf.data(), pf);
}

bool read_lidar_packet(const sensor::client& cli, PacketMsg& pm,
                       const sensor::packet_format& pf) {
    pm.buf.resize(pf.lidar_packet_size + 1);
    return read_lidar_packet(cli, pm.buf.data(), pf);
}

sensor_msgs::msg::Imu packet_to_imu_msg(const PacketMsg& pm,
                                        const rclcpp::Time& timestamp,
                                        const std::string& frame,
                                        const sensor::packet_format& pf) {
    const double standard_g = 9.80665;
    sensor_msgs::msg::Imu m;
    const uint8_t* buf = pm.buf.data();

    m.header.stamp = timestamp;
    m.header.frame_id = frame;

    m.orientation.x = 0;
    m.orientation.y = 0;
    m.orientation.z = 0;
    m.orientation.w = 0;

    m.linear_acceleration.x = pf.imu_la_x(buf) * standard_g;
    m.linear_acceleration.y = pf.imu_la_y(buf) * standard_g;
    m.linear_acceleration.z = pf.imu_la_z(buf) * standard_g;

    m.angular_velocity.x = pf.imu_av_x(buf) * M_PI / 180.0;
    m.angular_velocity.y = pf.imu_av_y(buf) * M_PI / 180.0;
    m.angular_velocity.z = pf.imu_av_z(buf) * M_PI / 180.0;

    for (int i = 0; i < 9; i++) {
        m.orientation_covariance[i] = -1;
        m.angular_velocity_covariance[i] = 0;
        m.linear_acceleration_covariance[i] = 0;
    }
    for (int i = 0; i < 9; i += 4) {
        m.linear_acceleration_covariance[i] = 0.01;
        m.angular_velocity_covariance[i] = 6e-4;
    }

    return m;
}

struct read_and_cast {
    template <typename T, typename U>
    void operator()(Eigen::Ref<const ouster::img_t<T>> field,
                    ouster::img_t<U>& dest) {
        dest = field.template cast<U>();
    }
};

sensor::ChanField suitable_return(sensor::ChanField input_field, bool second) {
    switch (input_field) {
        case sensor::ChanField::RANGE:
        case sensor::ChanField::RANGE2:
            return second ? sensor::ChanField::RANGE2
                          : sensor::ChanField::RANGE;
        case sensor::ChanField::SIGNAL:
        case sensor::ChanField::SIGNAL2:
            return second ? sensor::ChanField::SIGNAL2
                          : sensor::ChanField::SIGNAL;
        case sensor::ChanField::REFLECTIVITY:
        case sensor::ChanField::REFLECTIVITY2:
            return second ? sensor::ChanField::REFLECTIVITY2
                          : sensor::ChanField::REFLECTIVITY;
        case sensor::ChanField::NEAR_IR:
            return sensor::ChanField::NEAR_IR;
        default:
            throw std::runtime_error("Unreachable");
    }
}

template <typename T>
inline ouster::img_t<T> get_or_fill_zero(sensor::ChanField f,
                                         const ouster::LidarScan& ls) {
    if (!ls.field_type(f)) {
        return ouster::img_t<T>::Zero(ls.h, ls.w);
    }

    ouster::img_t<T> result{ls.h, ls.w};
    ouster::impl::visit_field(ls, f, read_and_cast(), result);
    return result;
}

template <typename PointT, typename RangeT, typename ReflectivityT,
          typename NearIrT, typename SignalT>
void copy_scan_to_cloud(ouster_ros::Cloud& cloud, const ouster::LidarScan& ls,
                        std::chrono::nanoseconds scan_ts, const PointT& points,
                        const ouster::img_t<RangeT>& range,
                        const ouster::img_t<ReflectivityT>& reflectivity,
                        const ouster::img_t<NearIrT>& near_ir,
                        const ouster::img_t<SignalT>& signal) {
    auto timestamp = ls.timestamp();

    const auto rg = range.data();
    const auto rf = reflectivity.data();
    const auto nr = near_ir.data();
    const auto sg = signal.data();

#ifdef __OUSTER_UTILIZE_OPENMP__
#pragma omp parallel for collapse(2)
#endif
    for (auto u = 0; u < ls.h; u++) {
        for (auto v = 0; v < ls.w; v++) {
            const auto ts = std::min(
                std::chrono::nanoseconds(timestamp[v]) - scan_ts, scan_ts);
            const auto idx = u * ls.w + v;
            const auto xyz = points.row(idx);
            cloud.points[idx] = ouster_ros::Point{
                {static_cast<float>(xyz(0)), static_cast<float>(xyz(1)),
                 static_cast<float>(xyz(2)), 1.0f},
                static_cast<float>(sg[idx]),
                static_cast<uint32_t>(ts.count()),
                static_cast<uint16_t>(rf[idx]),
                static_cast<uint16_t>(u),
                static_cast<uint16_t>(nr[idx]),
                static_cast<uint32_t>(rg[idx]),
            };
        }
    }
}

void scan_to_cloud_f(ouster::PointsF& points,
                     const ouster::PointsF& lut_direction,
                     const ouster::PointsF& lut_offset,
                     ouster::LidarScan::ts_t scan_ts,
                     const ouster::LidarScan& ls, ouster_ros::Cloud& cloud,
                     int return_index) {
    bool second = (return_index == 1);

    assert(cloud.width == static_cast<std::uint32_t>(ls.w) &&
           cloud.height == static_cast<std::uint32_t>(ls.h) &&
           "point cloud and lidar scan size mismatch");

    // across supported lidar profiles range is always 32-bit
    auto range_channel_field =
        second ? sensor::ChanField::RANGE2 : sensor::ChanField::RANGE;
    ouster::img_t<uint32_t> range = ls.field<uint32_t>(range_channel_field);

    ouster::img_t<uint16_t> reflectivity = get_or_fill_zero<uint16_t>(
        suitable_return(sensor::ChanField::REFLECTIVITY, second), ls);

    ouster::img_t<uint32_t> signal = get_or_fill_zero<uint32_t>(
        suitable_return(sensor::ChanField::SIGNAL, second), ls);

    ouster::img_t<uint16_t> near_ir = get_or_fill_zero<uint16_t>(
        suitable_return(sensor::ChanField::NEAR_IR, second), ls);

    ouster::cartesianT(points, range, lut_direction, lut_offset);

    copy_scan_to_cloud(cloud, ls, scan_ts, points, range, reflectivity, near_ir,
                       signal);
}

sensor_msgs::msg::PointCloud2 cloud_to_cloud_msg(const Cloud& cloud,
                                                 const rclcpp::Time& timestamp,
                                                 const std::string& frame) {
    sensor_msgs::msg::PointCloud2 msg{};
    pcl::toROSMsg(cloud, msg);
    msg.header.frame_id = frame;
    msg.header.stamp = timestamp;
    return msg;
}

geometry_msgs::msg::TransformStamped transform_to_tf_msg(
    const ouster::mat4d& mat, const std::string& frame,
    const std::string& child_frame, rclcpp::Time timestamp) {
    Eigen::Affine3d aff;
    aff.linear() = mat.block<3, 3>(0, 0);
    aff.translation() = mat.block<3, 1>(0, 3) * 1e-3;

    geometry_msgs::msg::TransformStamped msg = tf2::eigenToTransform(aff);
    msg.header.stamp = timestamp;
    msg.header.frame_id = frame;
    msg.child_frame_id = child_frame;

    return msg;
}
}  // namespace ouster_ros