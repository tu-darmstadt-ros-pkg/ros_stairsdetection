#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cmath>
#include <pthread.h>

#include <pcl/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/segmentation/sac_segmentation.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/filters/extract_indices.h>
#include <pcl/common/angles.h>

#include <pcl_ros/transforms.h>

#include <yaml-cpp/yaml.h>

#include <ros_stairsdetection/ExportStairs.h>
#include <ros_stairsdetection/ImportStairs.h>
#include <ros_stairsdetection/ClearStairs.h>

#include "stairway.hpp"
#include "ros_context.hpp"
#include "step.hpp"
#include "transform_helper.hpp"
#include "print_helpers.hpp"

#include <tf/transform_listener.h>

using namespace std;

// forward declarations
bool isStartingStep(Step &step);
bool isNextStep(Stairway &stairway, Step &step);
bool alreadyKnown(Stairway &stairway);

tf::TransformListener* tfl_;

pthread_mutex_t stairwaysMutex;
vector<Stairway> stairways;
ROSContext rc;

bool sortSteps(Step a, Step b) {
	return (a.getMin().x < b.getMin().x);
}

void callback(const sensor_msgs::PointCloud2ConstPtr &input) {
	ROS_INFO("========================================================================");
	ROS_INFO("========================================================================");
	ROS_INFO("New input data received.");


    if (tfl_ == 0){
      tfl_ = new tf::TransformListener();
      sleep (1);
    }

	// convert from ros::pointcloud2 to pcl::pointcloud2
    //pcl::PCLPointCloud2* unfilteredCloud = new pcl::PCLPointCloud2;
    //pcl::PCLPointCloud2ConstPtr unfilteredCloudPtr(unfilteredCloud);
    pcl::PointCloud<pcl::PointXYZ>::Ptr unfilteredCloud = boost::make_shared<pcl::PointCloud<pcl::PointXYZ> >();
    pcl::PointCloud<pcl::PointXYZ>::Ptr filteredCloud = boost::make_shared<pcl::PointCloud<pcl::PointXYZ> >();


    pcl::fromROSMsg(*input, *unfilteredCloud);

    //pcl_conversions::toPCL(*input, *unfilteredCloud);

	// downsample the input data to speed things up.
    //pcl::PCLPointCloud2::Ptr filteredCloud(new pcl::PCLPointCloud2);

    tf::StampedTransform cam_to_world_transform;

    if (tfl_->waitForTransform("world", input->header.frame_id, input->header.stamp, ros::Duration(1.0))){
      tfl_->lookupTransform("world", input->header.frame_id, input->header.stamp, cam_to_world_transform);
    }else{
      ROS_WARN("tfl timed out, returning!");
    }

    Eigen::Matrix4f cam_to_world_transform_eigen;
    pcl_ros::transformAsMatrix(cam_to_world_transform, cam_to_world_transform_eigen);

    pcl::PointCloud<pcl::PointXYZ>::Ptr unfiltered_cloud_world = boost::make_shared<pcl::PointCloud<pcl::PointXYZ> >();

    pcl::transformPointCloud(*unfilteredCloud, *unfiltered_cloud_world, cam_to_world_transform_eigen);


    pcl::PointCloud<pcl::PointXYZ>::Ptr cloud(new pcl::PointCloud<pcl::PointXYZ>);

    pcl::VoxelGrid<pcl::PointXYZ> sor;
    sor.setInputCloud(unfiltered_cloud_world);
    float leaf_size = 0.05f;
    sor.setLeafSize(leaf_size, leaf_size, leaf_size);								// default: sor.setLeafSize(0.01f, 0.01f, 0.01f);
    sor.filter(*cloud);

	// convert to pointcloud
    //
    //pcl::fromPCLPointCloud2(*filteredCloud, *cloud);

	// Do the parametric segmentation
	pcl::ModelCoefficients::Ptr coefficients(new pcl::ModelCoefficients);
	pcl::PointIndices::Ptr inliers(new pcl::PointIndices);


    pcl::SACSegmentation<pcl::PointXYZ> seg;
    seg.setOptimizeCoefficients(true);

    seg.setEpsAngle(pcl::deg2rad (15.0));
    seg.setAxis(Eigen::Vector3f(0.0f, 0.0f, 1.0f));
    seg.setDistanceThreshold(0.05);

    seg.setModelType(pcl::SACMODEL_PERPENDICULAR_PLANE);
	seg.setMethodType(pcl::SAC_RANSAC);
	seg.setMaxIterations(rc.getSegmentationIterationSetting());
	seg.setDistanceThreshold(rc.getSegmentationThresholdSetting());

	pcl::ExtractIndices<pcl::PointXYZ> extract;
	pcl::PointCloud<pcl::PointXYZ>::Ptr cloud1(new pcl::PointCloud<pcl::PointXYZ>),
			cloud2(new pcl::PointCloud<pcl::PointXYZ>);
	unsigned int pointsAtStart = cloud->points.size(), id = -1;

	vector<Step> steps;

    std::vector<pcl::PointCloud<pcl::PointXYZ>::Ptr > segmented_cloud_vector;

	// Extract a model and repeat while 10% of the original cloud is still present
	while (cloud->points.size() > 0.1 * pointsAtStart) {
		id++;

		seg.setInputCloud(cloud);
		seg.segment(*inliers, *coefficients);

		if (inliers->indices.size() == 0) {
			ROS_WARN("Could not estimate a planar model for the given dataset.");
			break;
		}

		// extract the inliers
		extract.setInputCloud(cloud);
		extract.setIndices(inliers);
    extract.setNegative(false);
    extract.filter(*cloud1);
    int num_step_points = (int)cloud1->size();
    ROS_INFO("num points: %d", (int)cloud1->size());

		extract.setNegative(true);
		extract.filter(*cloud2);
		cloud.swap(cloud2);

		// calculate AABB and transform to world coordinates
		// PCL points are automatically transformed to ROS points while calculating AABB
		Step step;
        rc.getTransformHelper().getAABB(cloud1, step);
        //rc.getTransformHelper().transformToRobotCoordinates(step);

		// Heigh enough?
    if (step.getHeight() < rc.getMinStepHeightSetting() || step.getHeight() > rc.getMaxStepHeightSetting()
        || step.getWidth() < rc.getMinStepWidthSetting() || step.getWidth() > rc.getMaxStepWidthSetting()
        || step.getDepth() < rc.getMinStepDepthSetting() || step.getDepth() > rc.getMaxStepDepthSetting()
        || num_step_points < rc.getMinNumPointsSetting() || num_step_points > rc.getMaxNumPointsSetting()) {
        continue;
		}

        /*
        if ((id ==0) && rc.cloudRequested()){
          sensor_msgs::PointCloud2 cloud_out;
          pcl::toROSMsg(*cloud1, cloud_out);
          cloud_out.header.frame_id = "world";
          cloud_out.header.stamp = ros::Time::now();
          rc.publishCloud(cloud_out);
        }
        */

        segmented_cloud_vector.push_back(cloud1);
		steps.push_back(step);
	}

    if ( rc.cloudRequested()){

      pcl::PointCloud<pcl::PointXYZRGB>::Ptr concat_cloud = boost::make_shared<pcl::PointCloud<pcl::PointXYZRGB> >();

      size_t total_size = 0;
      for (size_t i = 0; i < segmented_cloud_vector.size(); ++i){
        total_size += segmented_cloud_vector[i]->size();
      }

      concat_cloud->reserve(total_size);

      for (size_t i = 0; i < segmented_cloud_vector.size(); ++i){

        pcl::PointCloud<pcl::PointXYZ>& curr = *segmented_cloud_vector[i];

        pcl::PointXYZRGB point (static_cast<double>(i)/static_cast<double>(segmented_cloud_vector.size()) , 1.0, 1.0);

        for (size_t p = 0; p < curr.size(); ++p){

          point.x = curr[p].x;
          point.y = curr[p].y;
          point.z = curr[p].z;
          concat_cloud->push_back(point);
        }
      }

      sensor_msgs::PointCloud2 cloud_out;
      pcl::toROSMsg(*concat_cloud, cloud_out);
      cloud_out.header.frame_id = "world";
      cloud_out.header.stamp = ros::Time::now();
      rc.publishCloud(cloud_out);


      /*
          sensor_msgs::PointCloud2 cloud_out;
      pcl::toROSMsg(*cloud1, cloud_out);
      cloud_out.header.frame_id = "world";
      cloud_out.header.stamp = ros::Time::now();
      rc.publishCloud(cloud_out);
      */
    }


	/**
	 * Order Steps by distance and print
	 */
	std::sort(steps.begin(), steps.end(), sortSteps);
	print(steps);

	/**
	 * Publish steps?
	 */
	if (rc.getPublishStepsSetting()) {
		ROS_INFO("-----------------------------------------------------------------");
		ROS_INFO("Publishing %d step(s)", (int) steps.size());

		std::vector<Step> out = steps;
        //rc.getTransformHelper().transformToCameraCoordinates(out);
		rc.publishSteps(out);
	}

	/**
	 * Not looking for stairways -> return!
	 */
	if (!rc.getPublishStairwaysSetting()) {
		return;
	}

	pthread_mutex_lock(&stairwaysMutex);
	//stairways.clear();
	

    ROS_INFO("-----------------------------------------------------------------");
	/*
	 * Try to build (multiple) stairways out of the steps
	 */

	// Look for starting steps.
	// Every new starting step starts a new stairway and is erased from the list
	for (vector<Step>::iterator it = steps.begin(); it != steps.end();) {

		if (isStartingStep(*it)) {
			ROS_INFO("Found starting step: %s", it->toString().c_str());

			// Create stairway and add this step to the newly created stairway
			Stairway s;
			s.getSteps().push_back(*it);

			// Stairway already known?
			// only add to global list if not
			if (!alreadyKnown(s)) {
				stairways.push_back(s);
				ROS_INFO("Found new Stairway!");
			} else {
				ROS_INFO("Stairway already known!");
			}

			// Remove from steps list
			it = steps.erase(it);
		} else {
			++it;
		}
	}

	// Look for more steps:
	// Repeat adding steps to the stairways until there are no more addable steps
	while (true) {
		bool found_global = false;

		for (vector<Step>::iterator it = steps.begin(); it != steps.end();) {
			
			// Does this step belong to a stairway that has already been found?
			// Iterate stairways...
			bool found = false;
			for (vector<Stairway>::iterator jt = stairways.begin(); jt != stairways.end(); jt++) {
				if (isNextStep(*jt, *it)) {

					// Add step to stairway
					jt->getSteps().push_back(*it);

					// Remove step from steps list
					it = steps.erase(it);

					found = true;
					found_global = true;
					break;
				}
			}

			if (!found) {
				++it;
			}
		}

		if (!found_global) {
			break;
		}
	}

	/**
	 * Publish stairways?
	 */
	if (rc.getPublishStairwaysSetting()) {
		std::vector<Stairway> out = stairways;
		rc.getTransformHelper().transformToCameraCoordinates(out);
		rc.publishStairways(out);
		ROS_INFO("Published %d stairways", (int) stairways.size());
	}

	pthread_mutex_unlock(&stairwaysMutex);
}

bool alreadyKnown(Stairway &stairway) {

	for (vector<Stairway>::iterator it = stairways.begin(); it != stairways.end(); it++) {
		if (it->almostEquals(stairway)) {
			return true;
		}
	}

	return false;
}

/**
 * Starting steps have to meet the requirements defined in the launch file.
 *  - Must be "near" the ground
 *  - The height must not be higher (or lower) as the maximal (minimal) height setting
 *  - The step's width must be at least the width of the robot
 */
bool isStartingStep(Step &step) {
	const double maxHeightAboveGround = 0.05;

	// step is too heigh above the ground
	if (step.getHeightAboveGround() > maxHeightAboveGround) {return false;}

	// step is to heigh or to less heigh
	const double top = step.getHeight();
	if (top < rc.getMinStepHeightSetting() || top > rc.getMaxStepHeightSetting()) {return false;}

	// step is too narrow
	if (step.getWidth() < rc.getMinStepWidthSetting()) {return false;}

	return true;
}

bool isNextStep(Stairway &stairway, Step &step) {
	const double curTop = stairway.getSteps().back().getCenterTop().z;
	
	return (
		// step at least more than the minimum step height higher than the last step of the stairway
		curTop + rc.getMinStepHeightSetting() < step.getCenterTop().z

		// step ist not more than the maximum step height higher than the last step of the stairway
		&& curTop + rc.getMaxStepHeightSetting() > step.getCenterTop().z
	);
}

bool exportStairs(ros_stairsdetection::ExportStairs::Request &req,
		ros_stairsdetection::ExportStairs::Response &res) {

	pthread_mutex_lock(&stairwaysMutex);

	// traverse located stairways
	YAML::Node stairwaysNode;
	for (vector<Stairway>::iterator it = stairways.begin(); it != stairways.end(); it++) {
		YAML::Node stairway;
		YAML::Node first;

		// Step count and dimensions
		stairway["count"] = it->getSteps().size();
		stairway["width"] = it->getSteps().front().getWidth();
		stairway["height"] = it->getSteps().front().getHeight();

		// Depth
		stairway["depth"] = (it->getSteps().size() > 1)
				? fabs(it->getSteps().front().getCenterBottom().x - it->getSteps().at(1).getCenterBottom().x)
				: 0.f;

		// Center-bottom point of the first step
		geometry_msgs::Point cb = it->getSteps().front().getCenterBottom();
		//rc.getTransformHelper().transformToWorldCoordinates(cb);
		first["x"] = cb.x;
		first["y"] = cb.y;
		first["z"] = cb.z;
		stairway["first_step"] = first;

		stairwaysNode["stairways"].push_back(stairway);
	}

	pthread_mutex_unlock(&stairwaysMutex);

	const string path = req.path;
	ofstream fout(path.c_str());
	fout << stairwaysNode << '\n';
	res.result = "Written succesfully to " + path + ".";
	return true;
}

void buildStep(const double width, const double height, const double depth, const int i, Step &step) {
	geometry_msgs::Point p1;
}

bool importStairs(ros_stairsdetection::ImportStairs::Request &req,
		ros_stairsdetection::ImportStairs::Response &res) {

	pthread_mutex_lock(&stairwaysMutex);

	// Clear current data and load file
	stairways.clear();
	ROS_INFO("Stairways cleared...");
	YAML::Node root = YAML::LoadFile(req.path);

	// Iterate stairways
	for (YAML::const_iterator it = root["stairways"].begin(); it != root["stairways"].end(); it++) {
		Stairway stairway;

		const double width  = (*it)["width"].as<double>(),
					 height = (*it)["height"].as<double>(),
					 depth  = (*it)["depth"].as<double>();
		ROS_INFO("Width: %f", width);
		geometry_msgs::Point bottomCenter;
		bottomCenter.x = (*it)["first_step"]["x"].as<double>();
		bottomCenter.y = (*it)["first_step"]["y"].as<double>();
		bottomCenter.z = (*it)["first_step"]["z"].as<double>();

		print(bottomCenter);

		// Build n steps
		for (unsigned int i = 0; i < (*it)["count"].as<int>(); i++) {
			Step step = Step(bottomCenter, width, height, depth, i);
			stairway.getSteps().push_back(step);
		}

		stairways.push_back(stairway);
		ROS_INFO("Added a stairway with %d steps", (int) stairway.getSteps().size());
	}

	pthread_mutex_unlock(&stairwaysMutex);
	return true;
}

bool clearStairs(ros_stairsdetection::ClearStairs::Request &req, ros_stairsdetection::ClearStairs::Response &res) {
	pthread_mutex_lock(&stairwaysMutex);
	
	stairways.clear();
	rc.publishStairways(stairways);

	pthread_mutex_unlock(&stairwaysMutex);
	return true;
}

int main(int argc, char **argv) {

    tfl_ = 0;

	pthread_mutex_init(&stairwaysMutex, NULL);

	ROS_INFO("Starting up...");
	rc.init(argc, argv, &callback, &exportStairs, &importStairs, &clearStairs);
	ROS_INFO("Initiated ROSContext successfully.");

	return 0;
}
