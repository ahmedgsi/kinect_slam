/* This file is part of RGBDSLAM.
 * 
 * RGBDSLAM is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * RGBDSLAM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with RGBDSLAM.  If not, see <http://www.gnu.org/licenses/>.
 */


//Documentation see header file
#include "pcl/ros/conversions.h"
#include <pcl/io/io.h>
#include "pcl/common/transform.h"
#include "pcl_ros/transforms.h"
#include "openni_listener.h"
#include <cv_bridge/CvBridge.h>
#include <opencv2/imgproc/imgproc.hpp>
#include <iostream>
#include <sstream>
#include <string>
#include <cv.h>
#include <ctime>
#include <sensor_msgs/PointCloud2.h>
#include <Eigen/Core>
#include <QMessageBox>
#include "node.h"
#include "globaldefinitions.h"


// #define EDGE_DETECTION

#ifdef EDGE_DETECTION
#include "pcl/range_image/range_image.h"
#include "pcl/io/pcd_io.h"
#include "pcl_visualization/range_image_visualizer.h"
#include "pcl_visualization/pcl_visualizer.h"
#include "pcl/features/range_image_border_extractor.h"
#include "pcl/keypoints/narf_keypoint.h"


using namespace pcl;
using namespace pcl_visualization;


#endif






OpenNIListener::OpenNIListener(ros::NodeHandle nh, GraphManager* graph_mgr,
		const char* visual_topic,
		const char* depth_topic, const char* cloud_topic,
		const char* detector_type, const char* extractor_type)
: graph_mgr_(graph_mgr),
  visual_sub_ (nh, visual_topic, global_subscriber_queue_size),
  depth_sub_(nh, depth_topic, global_subscriber_queue_size),
  cloud_sub_(nh, cloud_topic, global_subscriber_queue_size),
  sync_(MySyncPolicy(global_subscriber_queue_size),  visual_sub_, depth_sub_, cloud_sub_),
  depth_mono8_img_(cv::Mat()),
  nh_(nh),
  /*callback_counter_(0),*/
  save_bag_file(true),
  pause_(global_start_paused),
  getOneFrame_(false),
  first_frame_(true)
//pc_pub(nh.advertise<sensor_msgs::PointCloud2>("transformed_cloud", 2))
{
	// ApproximateTime takes a queue size as its constructor argument, hence MySyncPolicy(10)
	sync_.registerCallback(boost::bind(&OpenNIListener::cameraCallback, this, _1, _2, _3));
	ROS_INFO_STREAM("Listening to " << visual_topic << ", " << depth_topic \
			<< " and " << cloud_topic << "\n");
	detector_ = this->createDetector(detector_type);
	ROS_FATAL_COND(detector_.empty(), "No valid opencv keypoint detector!");
	extractor_ = this->createDescriptorExtractor(extractor_type);
	matcher_ = new cv::BruteForceMatcher<cv::L2<float> >() ;
	pub_cloud_ = nh.advertise<sensor_msgs::PointCloud2> (global_topic_reframed_cloud,
			global_publisher_queue_size);
	pub_transf_cloud_ = nh.advertise<sensor_msgs::PointCloud2> (
			global_topic_transformed_cloud, global_publisher_queue_size);
	pub_ref_cloud_ = nh.advertise<sensor_msgs::PointCloud2>(global_topic_first_cloud,
			global_publisher_queue_size);

	// save bag
	// create a nice name
	if (save_bag_file)
	{
		time_t rawtime; 
		struct tm * timeinfo;
		char buffer [80];

		time ( &rawtime );
		timeinfo = localtime ( &rawtime );
		strftime (buffer,80,"bags/%Y_%m_%d__%H_%M_%S.bag",timeinfo);

		bag.open(buffer, rosbag::bagmode::Write);
	} 
}

void OpenNIListener::cameraCallback (const sensor_msgs::ImageConstPtr& visual_img_msg, 
		const sensor_msgs::ImageConstPtr& depth_img_msg,
		const sensor_msgs::PointCloud2ConstPtr& point_cloud) {
	std::clock_t starttime=std::clock();
	ROS_DEBUG("Received data from kinect");

	//Get images into OpenCV format
	sensor_msgs::CvBridge bridge;
	cv::Mat depth_float_img = bridge.imgMsgToCv(depth_img_msg); 
	cv::Mat visual_img =  bridge.imgMsgToCv(visual_img_msg, "mono8");
	if(visual_img.rows != depth_float_img.rows ||
			visual_img.cols != depth_float_img.cols ||
			point_cloud->width != (uint32_t) visual_img.cols ||
			point_cloud->height != (uint32_t) visual_img.rows){
		ROS_ERROR("PointCloud, depth and visual image differ in size! Ignoring Data");
		return;
	}
	depthToCV8UC1(depth_float_img, depth_mono8_img_); //float can't be visualized or used as mask in float format TODO: reprogram keypoint detector to use float values with nan to mask


	Q_EMIT newVisualImage(cvMat2QImage(visual_img, 0)); //visual_idx=0
	Q_EMIT newDepthImage (cvMat2QImage(depth_mono8_img_,1));//overwrites last cvMat2QImage
	if(getOneFrame_) { //if getOneFrame_ is set, unset it and skip check for  pause
		getOneFrame_ = false;
	} else if(pause_) { //Visualization and nothing else
		return;
	}

	if (save_bag_file){
		// todo: make the names dynamic
		bag.write("/camera/rgb/points", ros::Time::now(), point_cloud);
		bag.write("/camera/rgb/image_mono", ros::Time::now(), visual_img_msg);
		bag.write("/camera/depth/image", ros::Time::now(), depth_img_msg);
	}

	//ROS_INFO_STREAM_COND_NAMED(( (std::clock()-starttime) / (double)CLOCKS_PER_SEC) > global_min_time_reported, "timings", "Callback runtime before addNode: "<< ( std::clock() - starttime ) / (double)CLOCKS_PER_SEC  <<"sec");

	//######### Main Work: create new node an add it to the graph ###############################
	Q_EMIT setGUIStatus("Computing Keypoints and Features");
	//TODO: make it an reference counting pointer object?
	std::clock_t node_creation_time=std::clock();
	Node* node_ptr = new Node(nh_,visual_img, detector_, extractor_, matcher_, point_cloud, depth_mono8_img_);




#ifdef EDGE_DETECTION
	// RangeImageVisualizer range_image_widget("Range image");


	float noise_level = 0.0;
	float min_range = 0.0f;
	int border_size = 1;

	float angular_resolution = deg2rad(1.0f);
	float support_size = 0.5f;

	RangeImage::CoordinateFrame coordinate_frame = RangeImage::LASER_FRAME;

	RangeImage range_image;
	Eigen::Affine3f scene_sensor_pose(Eigen::Affine3f::Identity());
	PointCloud<PointWithViewpoint> far_ranges;



	range_image.createFromPointCloud(node_ptr->pc_col, angular_resolution, deg2rad(360.0f), deg2rad(180.0f),
			scene_sensor_pose, coordinate_frame);//, noise_level, min_range, border_size);

	range_image.integrateFarRanges(far_ranges);


	range_image.setUnseenToMaxRange();
	printf("range image: %d x %d \n", (int)range_image.width,(int)range_image.height);





	PCLVisualizer viewer("3D Viewer");
	viewer.addCoordinateSystem(1.0f);
	viewer.addPointCloud(node_ptr->pc_col, "original point cloud");

	RangeImageBorderExtractor border_extractor(&range_image);
	PointCloud<BorderDescription> border_descriptions;
	border_extractor.compute(border_descriptions);

	// ----------------------------------
	// -----Show points in 3D viewer-----
	// ----------------------------------
	pcl::PointCloud<PointWithRange> border_points, veil_points, shadow_points;
	for (int y=0; y<(int)range_image.height; ++y)
	{
		for (int x=0; x<(int)range_image.width; ++x)
		{
			if (border_descriptions.points[y*range_image.width + x].traits[BORDER_TRAIT__OBSTACLE_BORDER])
				border_points.points.push_back(range_image.points[y*range_image.width + x]);
			if (border_descriptions.points[y*range_image.width + x].traits[BORDER_TRAIT__VEIL_POINT])
				veil_points.points.push_back(range_image.points[y*range_image.width + x]);
			if (border_descriptions.points[y*range_image.width + x].traits[BORDER_TRAIT__SHADOW_BORDER])
				shadow_points.points.push_back(range_image.points[y*range_image.width + x]);
		}
	}
	/*
	viewer.addPointCloud(border_points, "border points");
	viewer.setPointCloudRenderingProperties(pcl_visualization::PCL_VISUALIZER_POINT_SIZE, 7, "border points");
	viewer.addPointCloud(veil_points, "veil points");
	viewer.setPointCloudRenderingProperties(pcl_visualization::PCL_VISUALIZER_POINT_SIZE, 7, "veil points");
	viewer.addPointCloud(shadow_points, "shadow points");
	viewer.setPointCloudRenderingProperties(pcl_visualization::PCL_VISUALIZER_POINT_SIZE, 7, "shadow points");
*/

	// extract NARF-Points
	RangeImageBorderExtractor range_image_border_extractor;
	NarfKeypoint narf_keypoint_detector;

	narf_keypoint_detector.setRangeImageBorderExtractor(&range_image_border_extractor);
	narf_keypoint_detector.setRangeImage(&range_image);
	narf_keypoint_detector.getParameters().support_size = support_size;

	PointCloud<int> keypoint_indices;
	narf_keypoint_detector.compute(keypoint_indices);
	std::cout << "Found "<<keypoint_indices.points.size()<<" key points.\n";

	// show in 3D viewer
	PointCloud<PointXYZ> keypoints;
	keypoints.points.resize(keypoint_indices.points.size());
	for (size_t i=0; i<keypoint_indices.points.size(); ++i)
		keypoints.points[i].getVector3fMap() = range_image.points[keypoint_indices.points[i]].getVector3fMap();
	viewer.addPointCloud(keypoints, "keypoints");
	viewer.setPointCloudRenderingProperties(pcl_visualization::PCL_VISUALIZER_POINT_SIZE, 7, "keypoints");


	//-------------------------------------
	// -----Show points on range image-----
	// ------------------------------------
	/*	RangeImageVisualizer* range_image_borders_widget = NULL;
	range_image_borders_widget =
			RangeImageVisualizer::getRangeImageBordersWidget(range_image, -INFINITY, INFINITY, false,
					border_descriptions, "Range image with borders");
	 */
	while(!viewer.wasStopped()  )
	{
		// ImageWidgetWX::spinOnce();  // process GUI events
		viewer.spinOnce(100);
		usleep(100000);
	}



	return;
#endif // end of playing around with edge detection



	if((std::clock()-node_creation_time)/(double)CLOCKS_PER_SEC > 15.0){
		pause_ = true;
		setGUIInfo("<span style='color:red'>Node creation took more than 15 seconds. Paused processing to prevent CPU overload</span>");
		ROS_WARN("Node creation was really slow (>15s). Processing has been paused to prevent the computer from overload!");
		//delete node_ptr;
		//return;
	}
	std::clock_t parallel_wait_time=std::clock();
	future_.waitForFinished(); //Wait if GraphManager ist still computing. Does this skip on empty qfuture?
	ROS_INFO_STREAM_NAMED("timings", "waiting time: "<< ( std::clock() - parallel_wait_time ) / (double)CLOCKS_PER_SEC  <<"sec");
	Q_EMIT newVisualImage(cvMat2QImage(visual_img, 0)); //visual_idx=0
	Q_EMIT newDepthImage (cvMat2QImage(depth_mono8_img_,1));//overwrites last cvMat2QImage
	//processNode runs in the background and after finishing visualizes the results
	//#define CONCURRENT_NODE_CONSTRUCTION 1
#ifdef CONCURRENT_NODE_CONSTRUCTION
	ROS_WARN("Processing Node in parallel");
	future_ =  QtConcurrent::run(this, &OpenNIListener::processNode, visual_img, point_cloud, node_ptr);
#else
	processNode(visual_img, point_cloud, node_ptr);
#endif
	ROS_INFO_STREAM_COND_NAMED(( (std::clock()-starttime) / (double)CLOCKS_PER_SEC) > global_min_time_reported, "timings", __FUNCTION__ << "runtime: "<< ( std::clock() - starttime ) / (double)CLOCKS_PER_SEC  <<"sec");
}

void OpenNIListener::processNode(cv::Mat& visual_img,   // will be drawn to
		const sensor_msgs::PointCloud2ConstPtr point_cloud,
		Node* new_node){
	std::clock_t starttime=std::clock();
	Q_EMIT setGUIStatus("GraphSLAM");
	bool has_been_added = graph_mgr_->addNode(new_node);

	//######### Visualization code  #############################################
	if(has_been_added) graph_mgr_->drawFeatureFlow(visual_img);
	Q_EMIT newFeatureFlowImage(cvMat2QImage(visual_img, 2)); //include the feature flow now
	ROS_DEBUG("Sending PointClouds");
	//if node position was optimized: publish received pointcloud in new frame
	if (has_been_added && graph_mgr_->freshlyOptimized_ && (pub_cloud_.getNumSubscribers() > 0)){
		ROS_INFO("Sending original pointcloud with appropriatly positioned frame");
		sensor_msgs::PointCloud2 msg = *point_cloud;
		msg.header.stamp = graph_mgr_->time_of_last_transform_;
		msg.header.frame_id = "/slam_transform";
		pub_cloud_.publish(msg);
	}
	//slow, mainly for debugging (and only done if subscribed to): Transform pointcloud to fixed coordinate system and resend
	if (has_been_added && graph_mgr_->freshlyOptimized_ && (pub_transf_cloud_.getNumSubscribers() > 0)){
		ROS_WARN("Sending transformed pointcloud in fixed frame because /rgbdslam/transformed_slowdown_cloud was subscribed to. It's faster to subscribe to /rgbdslam/cloud instead");
		pcl::PointCloud<pcl::PointXYZRGB> pcl_cloud;
		pcl::fromROSMsg(*point_cloud, pcl_cloud);
		pcl_ros::transformPointCloud(pcl_cloud, pcl_cloud, graph_mgr_->world2cam_ );
		sensor_msgs::PointCloud2 msg;
		pcl::toROSMsg(pcl_cloud, msg);
		msg.header.frame_id = "/openni_camera";
		msg.header.stamp = ros::Time::now();
		pub_transf_cloud_.publish(msg);
	}
	//when first node has been added,  send it out once unchanged as reference
	if (first_frame_ && has_been_added && (pub_ref_cloud_.getNumSubscribers() > 0)){
		ROS_INFO("Sending Reference PointCloud once");
		pub_ref_cloud_.publish(*point_cloud);
		first_frame_ = false;
	}

	if(!has_been_added) delete new_node;
	ROS_INFO_STREAM_COND_NAMED(( (std::clock()-starttime) / (double)CLOCKS_PER_SEC) > global_min_time_reported, "timings", __FUNCTION__ << "runtime: "<< ( std::clock() - starttime ) / (double)CLOCKS_PER_SEC  <<"sec");
}


using namespace cv;
///Analog to opencv example file and modified to use adjusters
FeatureDetector* OpenNIListener::createDetector( const string& detectorType ) {
	FeatureDetector* fd = 0;
	if( !detectorType.compare( "FAST" ) ) {
		//fd = new FastFeatureDetector( 20/*threshold*/, true/*nonmax_suppression*/ );
		fd = new DynamicAdaptedFeatureDetector (new FastAdjuster(20,true),
				global_adjuster_min_keypoints,
				global_adjuster_max_keypoints,
				global_fast_adjuster_max_iterations);
	}
	else if( !detectorType.compare( "STAR" ) ) {
		fd = new StarFeatureDetector( 16/*max_size*/, 5/*response_threshold*/, 10/*line_threshold_projected*/,
				8/*line_threshold_binarized*/, 5/*suppress_nonmax_size*/ );
	}
	else if( !detectorType.compare( "SIFT" ) ) {
		fd = new SiftFeatureDetector(SIFT::DetectorParams::GET_DEFAULT_THRESHOLD(),
				SIFT::DetectorParams::GET_DEFAULT_EDGE_THRESHOLD());
		ROS_INFO("Default SIFT threshold: %f, Default SIFT Edge Threshold: %f",
				SIFT::DetectorParams::GET_DEFAULT_THRESHOLD(),
				SIFT::DetectorParams::GET_DEFAULT_EDGE_THRESHOLD());
	}
	else if( !detectorType.compare( "SURF" ) ) {
		fd = new DynamicAdaptedFeatureDetector(new SurfAdjuster(),
				global_adjuster_min_keypoints,
				global_adjuster_max_keypoints,
				global_surf_adjuster_max_iterations);
	}
	else if( !detectorType.compare( "MSER" ) ) {
		fd = new MserFeatureDetector( 1/*delta*/, 60/*min_area*/, 14400/*_max_area*/, 0.35f/*max_variation*/,
				0.2/*min_diversity*/, 200/*max_evolution*/, 1.01/*area_threshold*/, 0.003/*min_margin*/,
				5/*edge_blur_size*/ );
	}
	else if( !detectorType.compare( "GFTT" ) ) {
		fd = new GoodFeaturesToTrackDetector( 200/*maxCorners*/, 0.001/*qualityLevel*/, 1./*minDistance*/,
				5/*int _blockSize*/, true/*useHarrisDetector*/, 0.04/*k*/ );
	}
	else {
		ROS_ERROR("No valid detector-type given: %s. Using SURF.", detectorType.c_str());
		fd = createDetector("SURF"); //recursive call with correct parameter
	}
	ROS_ERROR_COND(fd == 0, "No detector could be created");
	return fd;
}

DescriptorExtractor* OpenNIListener::createDescriptorExtractor( const string& descriptorType ) {
	DescriptorExtractor* extractor = 0;
	if( !descriptorType.compare( "SIFT" ) ) {
		extractor = new SiftDescriptorExtractor();/*( double magnification=SIFT::DescriptorParams::GET_DEFAULT_MAGNIFICATION(), bool isNormalize=true, bool recalculateAngles=true, int nOctaves=SIFT::CommonParams::DEFAULT_NOCTAVES, int nOctaveLayers=SIFT::CommonParams::DEFAULT_NOCTAVE_LAYERS, int firstOctave=SIFT::CommonParams::DEFAULT_FIRST_OCTAVE, int angleMode=SIFT::CommonParams::FIRST_ANGLE )*/
	}
	else if( !descriptorType.compare( "SURF" ) ) {
		extractor = new SurfDescriptorExtractor();/*( int nOctaves=4, int nOctaveLayers=2, bool extended=false )*/
	}
	else {
		ROS_ERROR("No valid descriptor-matcher-type given: %s. Using SURF", descriptorType.c_str());
		extractor = createDescriptorExtractor("SURF");
	}
	return extractor;
}

void OpenNIListener::togglePause(){
	pause_ = !pause_;
	if(pause_) Q_EMIT setGUIStatus("Processing Thread Stopped");
	else Q_EMIT setGUIStatus("Processing Thread Running");
}
void OpenNIListener::getOneFrame(){
	getOneFrame_=true;
}

/// Create a QImage from image. The QImage stores its data in the rgba_buffers_ indexed by idx (reused/overwritten each call)
QImage OpenNIListener::cvMat2QImage(const cv::Mat& image, unsigned int idx){
	ROS_DEBUG_STREAM("Converting Matrix of type " << openCVCode2String(image.type()) << " to RGBA");
	if(rgba_buffers_.size() <= idx){
		rgba_buffers_.resize(idx+1);
	}
	ROS_DEBUG_STREAM("Target Matrix is of type " << openCVCode2String(rgba_buffers_[idx].type()));
	if(image.rows != rgba_buffers_[idx].rows || image.cols != rgba_buffers_[idx].cols){
		ROS_DEBUG("Created new rgba_buffer with index %i", idx);
		rgba_buffers_[idx] = cv::Mat( image.rows, image.cols, CV_8UC4);
		printMatrixInfo(rgba_buffers_[idx]);
	}
	cv::Mat alpha( image.rows, image.cols, CV_8UC1, cv::Scalar(255)); //TODO this could be buffered for performance
	cv::Mat in[] = { image, alpha };
	// rgba[0] -> bgr[2], rgba[1] -> bgr[1],
	// rgba[2] -> bgr[0], rgba[3] -> alpha[0]
	int from_to[] = { 0,0,  0,1,  0,2,  1,3 };
	mixChannels( in , 2, &rgba_buffers_[idx], 1, from_to, 4 );
	printMatrixInfo(rgba_buffers_[idx]);
	//cv::cvtColor(image, rgba_buffers_, CV_GRAY2RGBA);}
	//}
	return QImage((unsigned char *)(rgba_buffers_[idx].data),
			rgba_buffers_[idx].cols, rgba_buffers_[idx].rows,
			rgba_buffers_[idx].step, QImage::Format_RGB32 );
}

/*/ Old helper functions from earlier times. Could be useful lateron
void transformPointCloud (const Eigen::Matrix4f &transform, const sensor_msgs::PointCloud2 &in,
                          sensor_msgs::PointCloud2 &out)
{
  // Get X-Y-Z indices
  int x_idx = pcl::getFieldIndex (in, "x");
  int y_idx = pcl::getFieldIndex (in, "y");
  int z_idx = pcl::getFieldIndex (in, "z");

  if (x_idx == -1 || y_idx == -1 || z_idx == -1) {
    ROS_ERROR ("Input dataset has no X-Y-Z coordinates! Cannot convert to Eigen format.");
    return;
  }

  if (in.fields[x_idx].datatype != sensor_msgs::PointField::FLOAT32 || 
      in.fields[y_idx].datatype != sensor_msgs::PointField::FLOAT32 || 
      in.fields[z_idx].datatype != sensor_msgs::PointField::FLOAT32) {
    ROS_ERROR ("X-Y-Z coordinates not floats. Currently only floats are supported.");
    return;
  }
  // Check if distance is available
  int dist_idx = pcl::getFieldIndex (in, "distance");
  // Copy the other data
  if (&in != &out)
  {
    out.header = in.header;
    out.height = in.height;
    out.width  = in.width;
    out.fields = in.fields;
    out.is_bigendian = in.is_bigendian;
    out.point_step   = in.point_step;
    out.row_step     = in.row_step;
    out.is_dense     = in.is_dense;
    out.data.resize (in.data.size ());
    // Copy everything as it's faster than copying individual elements
    memcpy (&out.data[0], &in.data[0], in.data.size ());
  }

  Eigen::Array4i xyz_offset (in.fields[x_idx].offset, in.fields[y_idx].offset, in.fields[z_idx].offset, 0);

  for (size_t i = 0; i < in.width * in.height; ++i) {
    Eigen::Vector4f pt (*(float*)&in.data[xyz_offset[0]], *(float*)&in.data[xyz_offset[1]], *(float*)&in.data[xyz_offset[2]], 1);
    Eigen::Vector4f pt_out;

    bool max_range_point = false;
    int distance_ptr_offset = i*in.point_step + in.fields[dist_idx].offset;
    float* distance_ptr = (dist_idx < 0 ? NULL : (float*)(&in.data[distance_ptr_offset]));
    if (!std::isfinite (pt[0]) || !std::isfinite (pt[1]) || !std::isfinite (pt[2])) {
      if (distance_ptr==NULL || !std::isfinite(*distance_ptr)) { // Invalid point 
        pt_out = pt;
      } else { // max range point
        pt[0] = *distance_ptr;  // Replace x with the x value saved in distance
        pt_out = transform * pt;
        max_range_point = true;
        //std::cout << pt[0]<<","<<pt[1]<<","<<pt[2]<<" => "<<pt_out[0]<<","<<pt_out[1]<<","<<pt_out[2]<<"\n";
      }
    } else {
      pt_out = transform * pt;
    }

    if (max_range_point) {
      // Save x value in distance again
 *(float*)(&out.data[distance_ptr_offset]) = pt_out[0];
      pt_out[0] = std::numeric_limits<float>::quiet_NaN();
      //std::cout << __PRETTY_FUNCTION__<<": "<<i << "is a max range point.\n";
    }

    memcpy (&out.data[xyz_offset[0]], &pt_out[0], sizeof (float));
    memcpy (&out.data[xyz_offset[1]], &pt_out[1], sizeof (float));
    memcpy (&out.data[xyz_offset[2]], &pt_out[2], sizeof (float));

    xyz_offset += in.point_step;
  }

  // Check if the viewpoint information is present
  int vp_idx = pcl::getFieldIndex (in, "vp_x");
  if (vp_idx != -1) {
    // Transform the viewpoint info too
    for (size_t i = 0; i < out.width * out.height; ++i) {
      float *pstep = (float*)&out.data[i * out.point_step + out.fields[vp_idx].offset];
      // Assume vp_x, vp_y, vp_z are consecutive
      Eigen::Vector4f vp_in (pstep[0], pstep[1], pstep[2], 1);
      Eigen::Vector4f vp_out = transform * vp_in;

      pstep[0] = vp_out[0];
      pstep[1] = vp_out[1];
      pstep[2] = vp_out[2];
    }
  }
}
 */

void depthToCV8UC1(const cv::Mat& float_img, cv::Mat& mono8_img){
	//Process images
	if(mono8_img.rows != float_img.rows || mono8_img.cols != float_img.cols){
		mono8_img = cv::Mat(float_img.size(), CV_8UC1);}
	//The following doesn't work due to NaNs
	//double minVal, maxVal;
	//minMaxLoc(float_img, &minVal, &maxVal);
	//ROS_DEBUG("Minimum/Maximum Depth in current image: %f/%f", minVal, maxVal);
	//mono8_img = cv::Scalar(0);
	//cv::line( mono8_img, cv::Point2i(10,10),cv::Point2i(200,100), cv::Scalar(255), 3, 8);
	cv::convertScaleAbs(float_img, mono8_img, 100, 0.0);
}

//Little debugging helper functions
std::string openCVCode2String(unsigned int code){
	switch(code){
	case 0 : return std::string("CV_8UC1" );
	case 8 : return std::string("CV_8UC2" );
	case 16: return std::string("CV_8UC3" );
	case 24: return std::string("CV_8UC4" );
	case 2 : return std::string("CV_16UC1");
	case 10: return std::string("CV_16UC2");
	case 18: return std::string("CV_16UC3");
	case 26: return std::string("CV_16UC4");
	case 5 : return std::string("CV_32FC1");
	case 13: return std::string("CV_32FC2");
	case 21: return std::string("CV_32FC3");
	case 29: return std::string("CV_32FC4");
	}
	return std::string("Unknown");
}

void printMatrixInfo(cv::Mat& image){
	ROS_DEBUG_STREAM("Matrix Type:" << openCVCode2String(image.type()) <<  " rows: " <<  image.rows  <<  " cols: " <<  image.cols);
}
