/*********************************************************************
* Software License Agreement (BSD License)
* 
*  Copyright (c) 2008, EJ Kreinar, Case Western Reserve University
*  All rights reserved.
* 
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
* 
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of the Willow Garage nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
* 
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*********************************************************************/

// Based loosely on the robot_pose_ekf program by Wim Meeussen
//  and Willow Garage

#include <robot_precision_ekf/robot_precision_ekf_node.h>

#include <iostream>

using namespace MatrixWrapper;
using namespace std;
using namespace ros;
using namespace tf;


static const double EPS = 1e-5;

// constructor
RobotPrecisionEKFNode::RobotPrecisionEKFNode()
{
  ros::NodeHandle nh_private("~");
  ros::NodeHandle nh;
  
  tfb_ = new tf::TransformBroadcaster();
  tf_ = new tf::TransformListener();

  // *****************
  // GET PARAMETERS
  // *****************

  // General Parameters
  nh_private.param("global_frame_id", global_frame_id_, std::string("map"));
  nh_private.param("odom_frame_id", odom_frame_id_, std::string("odom"));
  nh_private.param("base_frame_id", base_frame_id_, std::string("base_link"));
  nh_private.param("sensor_timeout", timeout_, 1.0);
  
  // Filter parameters
  std::string tmp_filter_type;
  nh_private.param("filter_type", tmp_filter_type, std::string("ekf_5state"));
  nh_private.param("odom_used", odom_used_, true);
  nh_private.param("imu_used",  imu_used_, true);
  nh_private.param("gps_used",   gps_used_, true);
  double freq;
  nh_private.param("freq", freq, 10.0);
  double tmp_tol;
  nh_private.param("transform_tolerance", tmp_tol, 0.1);
  
  // Noise parameters
  nh_private.param("sigma_sys_x",  sigma_sys_x_, 0.01);
  nh_private.param("sigma_sys_y",  sigma_sys_y_, 0.01);
  nh_private.param("sigma_sys_tht",  sigma_sys_tht_, 0.05);
  nh_private.param("sigma_sys_vel",  sigma_sys_vel_, 0.5);
  nh_private.param("sigma_sys_omg",  sigma_sys_omg_, 0.5);
  nh_private.param("sigma_sys_vR", sigma_sys_vR_, 0.05);
  nh_private.param("sigma_sys_vL", sigma_sys_vL_, 0.05);
  nh_private.param("sigma_sys_imubias",  sigma_sys_imubias_, 0.001);
  nh_private.param("sigma_meas_gps_x",  sigma_meas_gps_x_, 0.05);
  nh_private.param("sigma_meas_gps_y",  sigma_meas_gps_y_, 0.05);
  nh_private.param("sigma_meas_odom_alpha",  sigma_meas_odom_alpha_, 0.01);
  nh_private.param("sigma_meas_odom_epsilon",  sigma_meas_odom_eps_, 0.0001);
  nh_private.param("sigma_meas_imu_omg",  sigma_meas_imu_omg_, 0.05);
  
  // Node parameters
  nh_private.param("debug",   debug_, false);

  ROS_INFO("Setting filter type to: %s", tmp_filter_type.c_str());
  if(tmp_filter_type == "ekf_5state")
  {
    filter_type_ = RobotPrecisionEKF::EKF_5STATE;
  }
  else if(tmp_filter_type == "ekf_3state")
  {
    filter_type_ = RobotPrecisionEKF::EKF_3STATE;  
  }
  else if(tmp_filter_type == "ekf_7state_verr")
  {
    filter_type_ = RobotPrecisionEKF::EKF_7STATE_VERR;  
  }
  else
  {
    ROS_WARN("Unknown filter type \"%s\"; defaulting to ekf_5state",
             tmp_filter_type.c_str());
    filter_type_ = RobotPrecisionEKF::EKF_5STATE;
  }
  
  
  // ********************************
  // INITIALIZE EKF and MEASUREMENTS
  // ********************************
   
  MatrixWrapper::ColumnVector sysNoise(8);
  sysNoise(1) = pow(sigma_sys_x_,2); // variance = sigma^2
  sysNoise(2) = pow(sigma_sys_y_,2);
  sysNoise(3) = pow(sigma_sys_tht_,2);
  sysNoise(4) = pow(sigma_sys_vel_,2);
  sysNoise(5) = pow(sigma_sys_omg_,2);
  sysNoise(6) = pow(sigma_sys_vR_,2);
  sysNoise(7) = pow(sigma_sys_vL_,2);
  sysNoise(8) = pow(sigma_sys_imubias_,2);
  sys_covariance_ = sysNoise;
  
  // Initialize Filter with desired configuration, dt, and noise
  ekf_filter_ = new RobotPrecisionEKF(filter_type_, 1.0/max(freq,1.0), sysNoise);
  
  // Add odometry measurement
  if (odom_used_){
    if (!ekf_filter_->initMeasOdom(sigma_meas_odom_alpha_, sigma_meas_odom_eps_))
      ROS_WARN("Tried to initialize Odometry measurement but failed");
  }
  
  // Add GPS measurement
  if (gps_used_){
    ColumnVector gpsNoise(2);
    gpsNoise(1) = pow(sigma_meas_gps_x_,2);
    gpsNoise(2) = pow(sigma_meas_gps_y_,2);
    gps_covariance_ = gpsNoise;
    if (!ekf_filter_->initMeasGPS(gpsNoise))
      ROS_WARN("Tried to initialize GPS measurement but failed");
  }
  
  // Add IMU measurement
  if (imu_used_) {
    double imuNoise = pow(sigma_meas_imu_omg_,2);
    if (!ekf_filter_->initMeasIMU(imuNoise))
      ROS_WARN("Tried to initialize IMU measurement but failed");
  }
  
  systemUpdate();
  

  // ********************************
  // NODE-SPECIFIC INITIALIZATIONS
  // ********************************
  
  // Set timer to desired dt
  timer_ = nh_private.createTimer(ros::Duration(1.0/max(freq,1.0)), &RobotPrecisionEKFNode::spin, this);
  transform_tolerance_.fromSec(tmp_tol);

  // advertise our estimation
  pose_pub_ = nh_private.advertise<geometry_msgs::PoseWithCovarianceStamped>("ekf_pose", 2);
  // TODO: Publish the estimated velocity if we want...???

  // initialize
  filter_stamp_ = Time::now();

  // subscribe to odom messages
  if (odom_used_){
    ROS_INFO("Odom sensor will be used on topic 'odom'");
    odom_sub_ = nh.subscribe("odom", 10, &RobotPrecisionEKFNode::odomCallback, this);
  }
  else ROS_INFO("Odom sensor will NOT be used");

  // subscribe to imu messages
  if (imu_used_){
    ROS_INFO("Imu sensor will be used on topic 'imu/data'");
    imu_sub_ = nh.subscribe("imu/data", 10,  &RobotPrecisionEKFNode::imuCallback, this);
  }
  else ROS_INFO("Imu sensor will NOT be used");

  // subscribe to vo messages
  if (gps_used_){
    ROS_INFO("Gps sensor will be used on topic 'gps_pose'");
    gps_sub_ = nh.subscribe("gps_pose", 10, &RobotPrecisionEKFNode::gpsCallback, this);
  }
  else ROS_INFO("VO sensor will NOT be used");

  // publish state service
  time_new_ = ros::Time::now().toSec();
  time_old_ = time_new_;
  time_start_ = 0.0;
  time_init_ = false;

  if (debug_){
    debug_pub_ = nh_private.advertise<robot_precision_ekf::EKFDebug>("ekf_debug", 2);
    // open files for debugging
    // TODO: Use files for debugging/ automated testing
    state_file_.open("/tmp/state_file.txt");
    cov_file_.open("/tmp/cov_file.txt");
    if (odom_used_)
      odom_file_.open("/tmp/odom_file.txt");
    if (imu_used_)
      imu_file_.open("/tmp/imu_file.txt");
    if (gps_used_)
      gps_file_.open("/tmp/gps_file.txt");
  }
};


// destructor
RobotPrecisionEKFNode::~RobotPrecisionEKFNode(){

  if (debug_){
    // close files for debugging
    state_file_.close();
    cov_file_.close();
    odom_file_.close();
    imu_file_.close();
    gps_file_.close();
  }
};

void RobotPrecisionEKFNode::setStartTime(double t)
{
  if (time_init_)
    return;
  else
  {
    time_start_ = t;
    time_init_ = true;
  }
}


// callback function for odom data
void RobotPrecisionEKFNode::odomCallback(const OdomConstPtr& odom)
{
  //odom_callback_counter_++;
  odom_stamp_ = odom->header.stamp;
  setStartTime(odom_stamp_.toSec());
  odom_time_  = Time::now();
  
  double v, w, vR, vL;
  v = odom->twist.twist.linear.x;
  w = odom->twist.twist.angular.z;
  vR = v + ODOM_TRACK/2*w;
  vL = v - ODOM_TRACK/2*w;
  
  ekf_filter_->measurementUpdateOdom(vR, vL);
  
  /*
  cout << endl << endl;
  cout << "Encoders:" << endl;
  cout << "v: " << v << " w: " << w << endl
       << "vR: " << vR << " vL: " << vL << endl;
  cout << "Encoder Update: " << endl;
  cout << " Posterior Mean = " << endl << ekf_filter_->getMean() << endl
       << " Covariance = " << endl << ekf_filter_->getCovariance() << "" << endl;
   */    
  if (debug_)
  {
    ekf_debug_.enc_vel = v;
    ekf_debug_.enc_omg = w;
    odom_file_ << odom_stamp_.toSec()-time_start_ << "," << v << "," << w << endl;
  }
};


// callback function for imu data
void RobotPrecisionEKFNode::imuCallback(const ImuConstPtr& imu)
{
  //imu_callback_counter_++;
  imu_stamp_ = imu->header.stamp;
  setStartTime(imu_stamp_.toSec());
  imu_time_  = Time::now();
  
  double imu_omg = imu->angular_velocity.z;
  
  ekf_filter_->measurementUpdateIMU(imu_omg);
  
  /*
  cout << endl << endl;
  cout << "IMU:" << endl;
  cout << "omega: " << imu_omg << endl;
  cout << "IMU Update: " << endl;
  cout << " Posterior Mean = " << endl << ekf_filter_->getMean() << endl
       << " Covariance = " << endl << ekf_filter_->getCovariance() << "" << endl;
  */
  if (debug_)
  {
    ekf_debug_.imu_omg = imu_omg;
    imu_file_ << imu_stamp_.toSec()-time_start_ << "," << imu_omg << endl;
  }
};

// callback function for GPS data
void RobotPrecisionEKFNode::gpsCallback(const GpsConstPtr& gps)
{
  //gps_callback_counter_++;
  gps_stamp_ = gps->header.stamp;
  setStartTime(gps_stamp_.toSec());
  gps_time_  = Time::now();
  cout << endl << "Gps Received: X: " << gps->pose.position.x << "," << gps->pose.position.y << endl;
  
  // Perform the system update every time the GPS is received!
  // It should be received every 10 hz, and may be more reliable than the 
  // internal ROS time. Plus, we dont want the system update and measurement 
  // updates to have any phase difference
  time_new_ = gps_stamp_.toSec();//ros::Time::now().toSec();
  double time_diff = time_new_-time_old_;
  ekf_filter_->setNewTimestep(time_diff);
  time_old_ = time_new_;
  
  this->systemUpdate();
  ekf_filter_->measurementUpdateGPS(gps->pose.position.x,gps->pose.position.y);
  
  ROS_INFO("\nSpin function at time %f, Elapsed: %f", ros::Time::now().toSec(), time_diff);
  cout << endl << endl;
  cout << "GPS Update: " << endl;
  cout << " Posterior Mean = " << endl << ekf_filter_->getMean() << endl
       << " Covariance = " << endl << ekf_filter_->getCovariance() << "" << endl;
  
  if (debug_)
  {
    ekf_debug_.gps_x = gps->pose.position.x;
    ekf_debug_.gps_y = gps->pose.position.y;
    gps_file_ << time_new_-time_start_ <<","<<gps->pose.position.x << "," << gps->pose.position.y << endl;
  }
  
  // Once the GPS message arrives, publish the updated state!
  publish();
};

void RobotPrecisionEKFNode::systemUpdate()
{
  // Current Design:
  //   - Everything Relies on the GPS measurement being received:
  //   - Upon receipt of GPS message, the system is updated:
  //       - dt modified
  //       - system prediction update
  //       - measurement update
  //   - Finally, the state and other debugging information is published
  
  // TODO: Experiment in the future with different timing schemes
  // 1. DIDNT WORK: System update based on a ROS timer, measurement updates in all callbacks
  //     I believe when the system was updated at 10Hz and the GPS measurement got out of phase
  //     dependent on some weird things in the node. Very very strange and finnicky magic happened
  // 2. HAVENT TRIED: System update based on a ROS timer, System AND measurement updates in all callbacks
  //     This will require a total change in the filter timestep at each update. BUT, this will free 
  //     the filter from depending on the GPS measurement. Ie, the GPS can NOT happen at all and
  //     the system will still update using other measurements received
  // 3. HAVENT TRIED: System update based on a ROS timer, verify during each measurement and sys update
  //     to make sure everything is synced and no measurements are out of phase (ie, two GPS updates 
  //     should not occur back-to-back without having a system update). 
  
  ekf_filter_->systemUpdate();
  
  cout << endl;
  cout << "System Update: " << endl;
  cout << " Posterior Mean = " << endl << ekf_filter_->getMean() << endl
       << " Covariance = " << endl << ekf_filter_->getCovariance() << "" << endl;
       
  // The system will spin whether or not any measurements are received
}

// filter loop
void RobotPrecisionEKFNode::spin(const ros::TimerEvent& e)
{
  // Dont do anything right now
  //systemUpdate();
};


void RobotPrecisionEKFNode::publish()
{
  MatrixWrapper::ColumnVector mean = ekf_filter_->getMean();
  MatrixWrapper::SymmetricMatrix cov = ekf_filter_->getCovariance();
  
  geometry_msgs::PoseWithCovarianceStamped p;
  // Fill in the header
  p.header.frame_id = global_frame_id_;
  p.header.stamp = gps_stamp_;
  // Copy in the pose
  p.pose.pose.position.x = mean(1);
  p.pose.pose.position.y = mean(2);
  tf::quaternionTFToMsg(tf::createQuaternionFromYaw(mean(3)), p.pose.pose.orientation);
  // Copy in the covariance
  for(int i=0; i<2; i++)
  {
    for(int j=0; j<2; j++)
    {
      p.pose.covariance[6*i+j] = cov(i+1,j+1);
    }
  }
  p.pose.covariance[6*5+5] = cov(3,3);
  pose_pub_.publish(p);
  
  // subtracting base to odom from map to base and send map to odom instead
  tf::Stamped<tf::Pose> odom_to_map;
  try
  {
    tf::Transform tmp_tf(tf::createQuaternionFromYaw(mean(3)),
                         tf::Vector3(mean(1),
                                     mean(2),
                                     0.0));
    tf::Stamped<tf::Pose> tmp_tf_stamped (tmp_tf.inverse(),
                                          gps_stamp_,
                                          base_frame_id_);
    // TODO: (ejk) Use a message filter instead of waiting for the odom->base_link transform
    // Also, note that the rosrun tf view_frames program wasnt very happy with this tf... :(
    this->tf_->waitForTransform(odom_frame_id_, base_frame_id_, gps_stamp_, ros::Duration(0.1));
    this->tf_->transformPose(odom_frame_id_,
                             tmp_tf_stamped,
                             odom_to_map);                         
    
    latest_tf_ = tf::Transform(tf::Quaternion(odom_to_map.getRotation()),
                               tf::Point(odom_to_map.getOrigin()));

    // We want to send a transform that is good up until a
    // tolerance time so that odom can be used
    ros::Time transform_expiration = (gps_stamp_ +
                                      transform_tolerance_);
    tf::StampedTransform tmp_tf_new(latest_tf_.inverse(),
                                    transform_expiration,
                                    global_frame_id_, odom_frame_id_);
    this->tfb_->sendTransform(tmp_tf_new);
  }
  catch(tf::TransformException e)
  {
    ROS_WARN("Failed to subtract base to odom transform (%s). Skipping transform", e.what());
  }
  
  // Send the debugging output...
  if (debug_)
  {
    int numstates = ekf_filter_->getNumStates();
    cout << "Times: " << time_new_ << " " << time_start_ <<endl;
    state_file_ << time_new_-time_start_ << ",";
    cov_file_   << time_new_-time_start_ << ",";
    cout        << time_new_-time_start_ << ",";
    for (int i=1; i<=(numstates-1); i++) {
      cout        << mean(i) << ",";
      cout        << 3*sqrt(cov(i,i)) << ",";
      state_file_ << mean(i) << ",";
      cov_file_   << 3*sqrt(cov(i,i)) << ",";
    }
    state_file_ << mean(numstates) << endl;
    cov_file_   << cov(numstates,numstates) << endl;
    
    // Send state and diagonal error bars
    switch (filter_type_)
    {
      case (RobotPrecisionEKF::EKF_5STATE):
        ekf_debug_.ekf_vel = mean(4);
        ekf_debug_.ekf_omg = mean(5);
        ekf_debug_.ekf_err_vel = 3*sqrt(cov(4,4));
        ekf_debug_.ekf_err_omg = 3*sqrt(cov(5,5));
      case (RobotPrecisionEKF::EKF_3STATE):
        ekf_debug_.ekf_x = mean(1);
        ekf_debug_.ekf_y = mean(2);
        ekf_debug_.ekf_tht = mean(3);
        ekf_debug_.ekf_err_x = 3*sqrt(cov(1,1));
        ekf_debug_.ekf_err_y = 3*sqrt(cov(2,2));
        ekf_debug_.ekf_err_tht = 3*sqrt(cov(3,3));
      default:
        debug_pub_.publish(ekf_debug_);
      break;
    }
  }
  
}


// ----------
// -- MAIN --
// ----------
int main(int argc, char **argv)
{
  // Initialize ROS
  ros::init(argc, argv, "robot_precision_ekf");

  // create filter class
  RobotPrecisionEKFNode ekf_filter_node;

  ros::spin();
  
  return 0;
}
