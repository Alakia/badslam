// Copyright 2019 ETH Zürich, Silvano Galliani, Thomas Schöps
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice,
//    this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its contributors
//    may be used to endorse or promote products derived from this software
//    without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.


#include "badslam/input_azurekinect.h"

#ifdef HAVE_K4A

namespace vis {

K4AInputThread::~K4AInputThread() {
  exit_ = true;
  if (thread_) {
    thread_->join();
  }
  k4a_device_close(device);
  LOG(INFO) << "Closing K4A input thread";
}

uint32_t K4AInputThread::k4a_convert_fps_to_uint(k4a_fps_t fps) {
  uint32_t fps_int;
  switch (fps) {
  case K4A_FRAMES_PER_SECOND_5:
    fps_int = 5;
    break;
  case K4A_FRAMES_PER_SECOND_15:
    fps_int = 15;
    break;
  case K4A_FRAMES_PER_SECOND_30:
    fps_int = 30;
    break;
  default:
    assert(0);
    fps_int = 0;
    break;
  }
  return fps_int;
}

void K4AInputThread::init_undistortion_map() {
  auto intrinsics = calibration.color_camera_calibration.intrinsics.parameters.param;
  width = calibration.color_camera_calibration.resolution_width;
  height = calibration.color_camera_calibration.resolution_height;
  
  std::vector<double> _camera_matrix = {
      intrinsics.fx / factor,
      0.f,
      intrinsics.cx / factor,
      0.f,
      intrinsics.fy / factor,
      intrinsics.cy / factor,
      0.f,
      0.f,
      1.f};
  
  // Create cv matrices
  camera_matrix = cv::Mat(3, 3, CV_64F, &_camera_matrix[0]);
  //cv::Mat pinhole_camera_matrix = cv::Mat(3, 3, CV_32F, &_pinhole_camera_matrix[0]);
  
  std::vector<double> _dist_coeffs = { intrinsics.k1, intrinsics.k2, intrinsics.p1,
                                       intrinsics.p2, intrinsics.k3, intrinsics.k4,
                                       intrinsics.k5, intrinsics.k6 };
  
  cv::Mat cv_depth_downscaled = cv::Mat::zeros(
      height / factor,
      width / factor, CV_16U);
  
  //cv::resize(cv_depth,
  //    cv_depth_downscaled,
  //    cv_depth_downscaled.size(),
  //    CV_INTER_AREA);
  
  cv::Mat dist_coeffs = cv::Mat(8, 1, CV_64F, &_dist_coeffs[0]);
  new_camera_matrix = cv::getOptimalNewCameraMatrix(
      camera_matrix,
      dist_coeffs,
      cv_depth_downscaled.size(),
      0,
      cv_depth_downscaled.size(),
      0,
      true
  );
  LOG(INFO) << "Camera matrix is " << camera_matrix;
  LOG(INFO) << "New camera matrix is " << new_camera_matrix;
  
  cv::Mat_<double> I = cv::Mat_<double>::eye(3, 3);
  
  map1 = cv::Mat::zeros(cv_depth_downscaled.size(), CV_16SC2);
  map2 = cv::Mat::zeros(cv_depth_downscaled.size(), CV_16UC1);
  initUndistortRectifyMap(camera_matrix, dist_coeffs, I, new_camera_matrix, cv::Size(width / factor, height / factor),
      map1.type(), map1, map2);
  //LOG(INFO) << "Map1 size is " << map1.size();
  //LOG(INFO) << "map2 size is " << map1.size();
}

void K4AInputThread::init_memory() {
  if (K4A_RESULT_SUCCEEDED != k4a_image_create(
      K4A_IMAGE_FORMAT_DEPTH16,
      calibration.color_camera_calibration.resolution_width,
      calibration.color_camera_calibration.resolution_height,
      calibration.color_camera_calibration.resolution_width * (int)sizeof(uint16_t),
      &transformed_depth_image)) {
      LOG(ERROR) << "WARNING: Failed to create transformed depth image!";
  }
  
  cv_undistorted_color = cv::Mat::zeros(
      calibration.color_camera_calibration.resolution_height/factor,
      calibration.color_camera_calibration.resolution_width/factor,
      CV_8UC4);
  
  cv_undistorted_depth = cv::Mat::zeros(
      calibration.color_camera_calibration.resolution_height/factor,
      calibration.color_camera_calibration.resolution_width/factor,
      CV_16U);
  
  cv_depth_downscaled = cv::Mat::zeros(
      calibration.color_camera_calibration.resolution_height/factor,
      calibration.color_camera_calibration.resolution_width/factor,
        CV_16U);
  cv_color_downscaled = cv::Mat::zeros(
      calibration.color_camera_calibration.resolution_height/factor,
      calibration.color_camera_calibration.resolution_width/factor,
      CV_8UC4);
}

void print_extrinsic(k4a_calibration_extrinsics_t *extrinsics) {
  printf("R:\n");
  printf(" \
%.10f %.10f %.10f\n \
%.10f %.10f %.10f\n \
%.10f %.10f %.10f\n",
      extrinsics->rotation[0],
      extrinsics->rotation[1],
      extrinsics->rotation[2],
      extrinsics->rotation[3],
      extrinsics->rotation[4],
      extrinsics->rotation[5],
      extrinsics->rotation[6],
      extrinsics->rotation[7],
      extrinsics->rotation[8]);
  printf("t:\n");
  printf("%.10f %.10f %.10f\n",
      extrinsics->translation[0],
      extrinsics->translation[1],
      extrinsics->translation[2]);
}

static void print_calibration(k4a_calibration_camera_t *calibration) {
  printf("intrinsic parameters: \n");
  printf("resolution width: %d\n", calibration->resolution_width);
  printf("resolution height: %d\n", calibration->resolution_height);
  printf("principal point x: %.10f\n", calibration->intrinsics.parameters.param.cx);
  printf("principal point y: %.10f\n", calibration->intrinsics.parameters.param.cy);
  printf("focal length x: %.10f\n", calibration->intrinsics.parameters.param.fx);
  printf("focal length y: %.10f\n", calibration->intrinsics.parameters.param.fy);
  printf("K:\n");
  printf(" \
%.10f 0 %.10f\n \
0 %.10f %.10f\n \
0 0 1\n",
      calibration->intrinsics.parameters.param.fx,
      calibration->intrinsics.parameters.param.cx,
      calibration->intrinsics.parameters.param.fy,
      calibration->intrinsics.parameters.param.cy);
  printf("radial distortion coefficients:\n");
  printf("k1: %.10f\n", calibration->intrinsics.parameters.param.k1);
  printf("k2: %.10f\n", calibration->intrinsics.parameters.param.k2);
  printf("k3: %.10f\n", calibration->intrinsics.parameters.param.k3);
  printf("k4: %.10f\n", calibration->intrinsics.parameters.param.k4);
  printf("k5: %.10f\n", calibration->intrinsics.parameters.param.k5);
  printf("k6: %.10f\n", calibration->intrinsics.parameters.param.k6);
  printf("center of distortion in Z=1 plane, x: %.10f\n", calibration->intrinsics.parameters.param.codx);
  printf("center of distortion in Z=1 plane, y: %.10f\n", calibration->intrinsics.parameters.param.cody);
  printf("tangential distortion coefficient x: %.10f\n", calibration->intrinsics.parameters.param.p1);
  printf("tangential distortion coefficient y: %.10f\n", calibration->intrinsics.parameters.param.p2);
  printf("metric radius: %.10f\n", calibration->intrinsics.parameters.param.metric_radius);
  printf("extrinsic parameters: \n");
  print_extrinsic(&calibration->extrinsics);
  printf("\n");
}

void K4AInputThread::Start(RGBDVideo<Vec3u8, u16>* rgbd_video, float* depth_scaling) {
  rgbd_video_ = rgbd_video;
  LOG(INFO) << "depth scaling " << *depth_scaling;
  //factor = *depth_scaling;
  
  uint32_t device_count = k4a_device_get_installed_count();
  
  if (device_count == 0) {
    LOG(ERROR) << "WARNING: I can't find any K4A device!";
    return;
  }
  
  if (K4A_RESULT_SUCCEEDED != k4a_device_open(K4A_DEVICE_DEFAULT, &device)) {
    LOG(ERROR) << "WARNING: Failed to open K4A device!";
    return;
  }
  
  config.camera_fps = K4A_FRAMES_PER_SECOND_30;
  //config.color_format = K4A_IMAGE_FORMAT_COLOR_BGRA32;
  config.color_format = K4A_IMAGE_FORMAT_COLOR_MJPG;
  //config.color_format = K4A_IMAGE_FORMAT_COLOR_YUY2;
  //config.color_format = K4A_IMAGE_FORMAT_COLOR_NV12;
  config.color_resolution = K4A_COLOR_RESOLUTION_720P;
  config.depth_mode = K4A_DEPTH_MODE_WFOV_2X2BINNED;
  config.synchronized_images_only = true;
  
  if (K4A_RESULT_SUCCEEDED != k4a_device_start_cameras(device, &config)) {
    LOG(ERROR) << "WARNING: Failed to open K4A device!";
  }
  
  if (K4A_RESULT_SUCCEEDED !=
      k4a_device_get_calibration(device, config.depth_mode, config.color_resolution, &calibration)) {
    LOG(ERROR) << "WARNING: Failed to get calibration for K4A device!";
  }
  //k4a_device_set_color_control(
  //    device,
  //    K4A_COLOR_CONTROL_POWERLINE_FREQUENCY,
  //    K4A_COLOR_CONTROL_MODE_MANUAL,
  //    50);
  
  K4A_CHECK(k4a_device_set_color_control(
      device,
      K4A_COLOR_CONTROL_EXPOSURE_TIME_ABSOLUTE,
      K4A_COLOR_CONTROL_MODE_MANUAL,
      7000));
  
  transformation = k4a_transformation_create(&calibration);
  
  print_calibration(&calibration.color_camera_calibration);
  print_calibration(&calibration.depth_camera_calibration);
  
  // Pre allocate memory
  init_memory();
  init_undistortion_map();
  
  //*depth_scaling = 1.0 / depth_scale; // XXX TODO
  
  float color_parameters[4];
  color_parameters[0] = new_camera_matrix.at<double>(0, 0);
  color_parameters[1] = new_camera_matrix.at<double>(1, 1);
  color_parameters[2] = new_camera_matrix.at<double>(0, 2) + 0.5; // XXX is 0.5 needed?
  color_parameters[3] = new_camera_matrix.at<double>(1, 2) + 0.5;
  
  // pinhole camera with params
  // fx, fy, cx, cy.
  rgbd_video->color_camera_mutable()->reset(new PinholeCamera4f(
      width/factor, height/factor, color_parameters));
  //LOG(INFO) << rgbd_video->color_camera()->parameters();
  
  // Set depth camera to be the same as the color camera
  rgbd_video->depth_camera_mutable()->reset(new PinholeCamera4f(
      width/factor, height/factor, color_parameters));
  
  // Start thread
  exit_ = false;
  thread_.reset(new thread(std::bind(&K4AInputThread::ThreadMain, this)));
}

bool K4AInputThread::decode_image_opencv(const k4a_image_t &color_image,
    k4a_image_t *_uncompressed_color_image,
    cv::Mat &decodedImage) {
  size_t nSize = k4a_image_get_size(color_image);
  uchar* buf = k4a_image_get_buffer(color_image);
  cv::Mat rawData(1, nSize, CV_8UC1, (void*)buf);
  
  decodedImage = cv::imdecode(rawData, CV_LOAD_IMAGE_UNCHANGED);
  if (decodedImage.data == NULL) {
    // Error reading raw image data
    printf("Error decoding image\n");
    return false;
  }
  k4a_image_create_from_buffer(
      K4A_IMAGE_FORMAT_COLOR_BGRA32,
      decodedImage.cols,
      decodedImage.rows,
      decodedImage.step,
      decodedImage.data,
      decodedImage.step[0] * decodedImage.rows,
      NULL,
      NULL,
      _uncompressed_color_image);
  //cv::imshow("Decoded image", decodedImage);
  //cv::waitKey(1000);
  return true;
}

void K4AInputThread::GetNextFrame() {
  static bool first = true;
  
  // Wait for the next frame
  unique_lock<mutex> lock(queue_mutex);
  // The first time delete queues to be in sync
  if (first) {
    depth_image_queue.clear();
    color_image_queue.clear();
    first = false;
  }
  while (depth_image_queue.empty()) {
    new_frame_condition_.wait(lock);
  }
  
  shared_ptr<Image<u16>> depth_image = depth_image_queue.front();
  depth_image_queue.erase(depth_image_queue.begin());
  shared_ptr<Image<Vec3u8>> color_image = color_image_queue.front();
  color_image_queue.erase(color_image_queue.begin());
  
  lock.unlock();
  
  // Add the frame to the RGBDVideo object
  rgbd_video_->depth_frames_mutable()->push_back(
      ImageFramePtr<u16, SE3f>(new ImageFrame<u16, SE3f>(depth_image)));
  rgbd_video_->color_frames_mutable()->push_back(
      ImageFramePtr<Vec3u8, SE3f>(new ImageFrame<Vec3u8, SE3f>(color_image)));
}

bool K4AInputThread::transform_depth_to_color(
    const k4a_transformation_t &transformation_handle,
    const k4a_image_t &depth_image,
    const k4a_image_t &color_image,
    k4a_image_t *transformed_image) {
  if (K4A_RESULT_SUCCEEDED !=
      k4a_transformation_depth_image_to_color_camera(
          transformation_handle,
          depth_image,
          *transformed_image)) {
    LOG(ERROR) << "Failed to compute transformed depth image for k4a";
    return false;
  }
  
  return true;
}

bool K4AInputThread::undistort_depth_and_rgb(k4a_calibration_intrinsic_parameters_t &intrinsics,
    const cv::Mat &cv_color,
    const cv::Mat &cv_depth,
    cv::Mat &undistorted_color,
    cv::Mat &undistorted_depth,
    const float _factor) {
  cv::resize(cv_depth,
      cv_depth_downscaled,
      cv_depth_downscaled.size(),
      CV_INTER_AREA);
  
  cv_depth_downscaled = cv_depth_downscaled * 5.f; // scale values to correct range TODO should be done according to parameter
  //cv::imshow("cv depth downscale ", cv_depth_downscaled);
  
  cv::resize(cv_color,
      cv_color_downscaled,
      cv_color_downscaled.size(),
      CV_INTER_AREA);
  //cv::imshow("cv color downscale ", cv_color_downscaled);
  
  remap(cv_depth_downscaled, undistorted_depth, map1, map2, cv::INTER_LINEAR, cv::BORDER_CONSTANT);
  remap(cv_color_downscaled, undistorted_color, map1, map2, cv::INTER_LINEAR, cv::BORDER_CONSTANT);
  return true;
}

void K4AInputThread::ThreadMain() {
  clock_t first_capture_start = clock();
  k4a_wait_result_t result = K4A_WAIT_RESULT_TIMEOUT;
  int32_t timeout_sec_for_first_capture = 60;
  
  // Wait for the first capture in a loop
  while (!exit_ && (clock() - first_capture_start) < (CLOCKS_PER_SEC * timeout_sec_for_first_capture)) {
    result = k4a_device_get_capture(device, &capture, 100);
    if (result == K4A_WAIT_RESULT_SUCCEEDED) {
      k4a_capture_release(capture);
      break;
    } else if (result == K4A_WAIT_RESULT_FAILED) {
      LOG(ERROR) << "Runtime error: k4a_device_get_capture() returned error: ";
    }
  }
  
  while (true) {
    if (exit_) {
      k4a_device_close(device);
      break;
    }
    
    uint32_t camera_fps = k4a_convert_fps_to_uint(config.camera_fps);
    
    clock_t recording_start = clock();
    int32_t timeout_ms = 1000 / camera_fps;
    result = k4a_device_get_capture(device, &capture, timeout_ms);
    if (result == K4A_WAIT_RESULT_TIMEOUT) {
      LOG(WARNING) << "k4a timeout device get capture";
      continue;
    } else if (result != K4A_WAIT_RESULT_SUCCEEDED) {
      std::cerr << "Runtime error: k4a_device_get_capture() returned " << result << std::endl;
      break;
    }
    
    // Access the depth16 image
    k4a_image_t k4a_depth_image = k4a_capture_get_depth_image(capture);
    
    // Access the rgb_image image
    k4a_image_t k4a_rgb_image = k4a_capture_get_color_image(capture);
    
    if (k4a_rgb_image == NULL || k4a_depth_image == NULL) {
      LOG(WARNING) << "Failed to get both depth and rgb images, skipping frame";
      if (k4a_rgb_image) {
        k4a_image_release(k4a_rgb_image);
      }
      if (k4a_depth_image) {
        k4a_image_release(k4a_depth_image);
      }
      k4a_capture_release(capture);
      continue;
    }
    
    cv::Mat cv_uncompressed_color_image;
    // Uncompress color image
    if (config.color_format == K4A_IMAGE_FORMAT_COLOR_MJPG) {
      if (!decode_image_opencv(
          k4a_rgb_image,
          &uncompressed_color_image,
          cv_uncompressed_color_image)) {
        break;
      }
    } else if (config.color_format == K4A_IMAGE_FORMAT_COLOR_BGRA32) {
      cv_uncompressed_color_image = cv::Mat(
          height,
          width,
          CV_8UC4,
          k4a_image_get_buffer(k4a_rgb_image));
    } else if (config.color_format == K4A_IMAGE_FORMAT_COLOR_YUY2) {
      size_t nSize = k4a_image_get_size(k4a_rgb_image);
      uchar* buf = k4a_image_get_buffer(k4a_rgb_image);
      cv::Mat rawData(height + height/2, width, CV_8UC1, (void*)buf);
      LOG(INFO) << "rawdata size is " << nSize;
      LOG(INFO) << "rawdata shape is " << rawData.size();
      cv::Mat output(height, width, CV_8UC3);
      cv::cvtColor(rawData, output, CV_YUV2BGR_YUY2, 0 );
      cv::imshow("cv", output);
      cv::waitKey(10000);
      LOG(INFO) << "Color shape is " << output.size();
      cv::imshow("cv", output);
      cv::waitKey(10000);
    } else if (config.color_format == K4A_IMAGE_FORMAT_COLOR_NV12) {
      size_t nSize = k4a_image_get_size(k4a_rgb_image);
      uchar* buf = k4a_image_get_buffer(k4a_rgb_image);
      cv::Mat rawData(height *1.5, width, CV_8UC1, (void*)buf);
      //cv::Mat cv(height, width, CV_8UC3);
      cv::cvtColor(rawData, cv_uncompressed_color_image, CV_YUV2RGB_NV21, 0 );
    }
    
    // Reproject depth on color
    if (!transform_depth_to_color(
        transformation,
        k4a_depth_image,
        uncompressed_color_image,
        &transformed_depth_image)) {
      break;
    }
    
    // wrap opencv mat over result
    cv::Mat cv_transformed_depth_image(
        k4a_image_get_height_pixels(transformed_depth_image),
        k4a_image_get_width_pixels(transformed_depth_image),
        CV_16UC1,
        k4a_image_get_buffer(transformed_depth_image));
    
    //LOG(INFO) << "K4A wrap opencv mat image";
    //cv::imshow("cv transformed depth", cv_transformed_depth_image);
    //cv::imshow("cv uncompressed color", cv_uncompressed_color_image);
    // Undistort (and downscale) color and depth
    undistort_depth_and_rgb(
        calibration.color_camera_calibration.intrinsics.parameters,
        cv_uncompressed_color_image,
        cv_transformed_depth_image,
        cv_undistorted_color,
        cv_undistorted_depth, factor);
    //LOG(INFO) << "K4A undistort depth and rgb";
    //cv::imshow("Undistorted color", cv_undistorted_color);
    //cv::imshow("Undistorted depth", cv_undistorted_depth);
    cv::cvtColor(cv_undistorted_color, cv_undistorted_color_noalpha, CV_BGRA2RGB);
    //LOG(INFO) << "Width and height are " << width << " " << height;
    //LOG(INFO) << "cv undistorted depth size is " << cv_undistorted_depth.size();
    //LOG(INFO) << "cv undistorted color_noalpha size is " << cv_undistorted_color_noalpha.size();
    //cv::imshow("Undistorted color noapha", cv_undistorted_color_noalpha);
    //cv::waitKey(3000);
    
    // Add the frame to the queue
    unique_lock<mutex> lock(queue_mutex);
    
    shared_ptr<Image<u16>> depth_image(new Image<u16>(
        cv_undistorted_depth.cols, 
        cv_undistorted_depth.rows));
    depth_image->SetTo(
        reinterpret_cast<const u16*>(cv_undistorted_depth.data), 
        cv_undistorted_depth.step[0]);
    depth_image_queue.push_back(depth_image);
    
    shared_ptr<Image<Vec3u8>> color_image(new Image<Vec3u8>(
        cv_undistorted_color_noalpha.cols, 
        cv_undistorted_color_noalpha.rows));
    color_image->SetTo(
        reinterpret_cast<const Vec3u8*>(cv_undistorted_color_noalpha.data), 
        cv_undistorted_color_noalpha.step[0]);
    color_image_queue.push_back(color_image);
    
    lock.unlock();
    new_frame_condition_.notify_all();
    k4a_image_release(k4a_rgb_image);
    k4a_image_release(k4a_depth_image);
    k4a_capture_release(capture);
  }
  
  k4a_device_close(device);
}

}

#endif  // HAVE_K4A
