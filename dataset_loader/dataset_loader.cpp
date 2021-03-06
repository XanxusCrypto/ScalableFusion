#include "dataset_loader.h"

#include <thread>
#include <fstream>

#include <eigen3/Eigen/Geometry>

using namespace std;
using namespace Eigen;

TumDataset::TumDataset(string folder, bool realtime, bool use_pose,
                       bool use_high_res, int skip_n_frames, float depth_scale,
                       float trajectory_GT_scale, bool invert_GT_trajectory) 
		: folder_path_(folder),
		  frame_index_(skip_n_frames),
		  has_high_res_(false),
		  has_poses_(use_pose),
		  radiometric_response_(nullptr),
		  read_depth_(false),
		  read_rgb_(false),
		  replay_speed(0),
		  running_(false),
		  scale_depth_(depth_scale),
		  skip_count(20),
		  vignetting_response_(nullptr),
		  white_fix_(cv::Vec3f(1, 1, 1)) {

	// Open frame association file
	ifstream frame_list;
	if(use_high_res) {
		frame_list.open(folder + "/associations_ids.txt");
		if(frame_list.is_open()) {
			has_high_res_ = true;
		} else {
			frame_list.open(folder + "/associations.txt");
			if(!frame_list.is_open()) {
				cout << "Couldn't open dataset (no associations.txt file found)" << endl;
				assert(0);
			}
		}
	}

	// Read rgb-depth-associations
	string line;
	while(getline(frame_list, line)) {
		istringstream line_ss(line);
		string word;

		// Read first timestamp
		getline(line_ss, word, ' ');
		double timestamp = atof(word.c_str());
		timestamps_.push_back(timestamp);

		// Read location of depth frame
		getline(line_ss, word, ' ');
		string depth_file = folder_path_ + "/" + word;
		depth_files_.push_back(depth_file);

		// Skip second timestamp
		getline(line_ss, word, ' ');

		// Read location of rgb frame
		getline(line_ss, word, ' ');
		string rgb_file = folder_path_ + "/" + word;
		rgb_files_.push_back(rgb_file);
	}

	if(!rgb_files_.empty()) {
		running_ = true;
	}	else {
		cout << "Could not read frame associations" << endl;
		assert(0);
	}

	// TODO: White fix? What is this?
	cv::FileStorage fs_white_fix_;
	has_high_res_ ?
			fs_white_fix_.open(folder + "/white_fix_ids.yml", cv::FileStorage::READ) :
			fs_white_fix_.open(folder + "/white_fix_.yml", cv::FileStorage::READ);

	if(fs_white_fix_.isOpened()) {
		fs_white_fix_["r_gain"] >> white_fix_[2];
		fs_white_fix_["g_gain"] >> white_fix_[1];
		fs_white_fix_["b_gain"] >> white_fix_[0];
	}

	// Read groundtruth
	ifstream trajectory_file(folder + "/groundtruth.txt");
	if(trajectory_file.is_open()) {
		string line;
		while(getline(trajectory_file, line)) {
			if(line[0] != '#') {
				TrajectoryPoint_ p;
				float x, y, z;
				float qx, qy, qz, qw;
				sscanf(line.c_str(), "%lf %f %f %f %f %f %f %f", 
				       &p.timestamp, &x, &y, &z, &qx, &qy, &qz, &qw);

				x *= trajectory_GT_scale;
				y *= trajectory_GT_scale;
				z *= trajectory_GT_scale;

				//read line to parameters and convert
				Affine3f transform(Translation3f(x, y, z));
				Matrix4f t = transform.matrix();

				// Create homogenous rotation matrix
				Quaternionf quaternion(qw, qx, qy, qz);
				Matrix4f r = Matrix4f::Identity();
				r.block<3, 3>(0, 0) = quaternion.toRotationMatrix();

				p.position = t * r;

				if(invert_GT_trajectory) {
					Matrix4f mat = p.position;
					p.position.block<3, 3>(0, 0) = mat.block<3, 3>(0, 0).inverse();
					p.position.block<3, 1>(0, 3) = mat.block<3, 1>(0, 3);
				}

				trajectory_.push_back(p);
			}
		}
	}

	if(folder.find("tumalike") != string::npos) {
		if(folder.find("1") != string::npos ||
		   folder.find("3") != string::npos ||
		   folder.find("5") != string::npos ||
		   folder.find("6") != string::npos ||
		   folder.find("7") != string::npos ||
		   folder.find("8") != string::npos ||
		   folder.find("9") != string::npos) {

			rgb_intrinsics_   = Vector4f(565, 575, 315, 220);
			depth_intrinsics_ = Vector4f(563.937, 587.847, 328.987, 225.661);

			Matrix4f rel_depth_to_color = Matrix4f::Identity();
			rel_depth_to_color(0, 2) = -0.026f; //i think the color camera is 2.6cm left of the depth camera
			Matrix3f rot = (AngleAxisf(-0.05 * 0.00, Vector3f::UnitX()) *
			                AngleAxisf( 0.00 * M_PI, Vector3f::UnitY()) *
			                AngleAxisf( 0.00 * M_PI, Vector3f::UnitZ())
			                ).normalized().toRotationMatrix();
			Matrix4f rot4 = Matrix4f::Identity();
			rot4.block<3, 3>(0, 0) = rot;
			depth_2_rgb_ = rot4 * rel_depth_to_color;

			// Basically whats in the elastic fusion initialization
			rgb_intrinsics_   = Vector4f(528, 528, 320, 240); // TODO: why is this wrong
			depth_intrinsics_ = Vector4f(528, 528, 320, 240);
			depth_2_rgb_      = Matrix4f::Identity(); // TODO: no.
		} else {
			rgb_intrinsics_   = Vector4f(537.562, 537.278, 313.730, 243.601);
			depth_intrinsics_ = Vector4f(563.937, 587.847, 328.987, 225.661);
			depth_2_rgb_      = Matrix4f::Identity(); // TODO: no.
		}
	} else {
		// TUM intrinsics:
		rgb_intrinsics_   = Vector4f(535.4, 539.2, 320.1, 247.6);
		depth_intrinsics_ = rgb_intrinsics_;
		depth_2_rgb_      = Matrix4f::Identity();
	}

	// Read exposure file if it exists
	ifstream exposure_file;
	has_high_res_ ?
			exposure_file.open(folder + "/rgb_ids_exposure.txt") :
			exposure_file.open(folder + "/rgb_exposure.txt");

	if(exposure_file.is_open()) {
		string line;
		while(getline(exposure_file, line)) {
			exposure_times_.push_back(atof(line.c_str()));
		}

		cout << "these exposure times are all wrong and need to be assigned to the correct frame" << endl;

		cv::Size size;
		cv::Mat M;
		cv::Mat D;
		cv::Mat M_new;
		if(has_high_res_) {
			cv::FileStorage intrinsics_id_storage(folder + "/../calib_result_ids.yml", 
			                                      cv::FileStorage::READ);
			if(!intrinsics_id_storage.isOpened()) {
				assert(0);
			}

			intrinsics_id_storage["camera_matrix"]           >> M;
			intrinsics_id_storage["distortion_coefficients"] >> D;
			intrinsics_id_storage["image_width"]             >> size.width;
			intrinsics_id_storage["image_height"]            >> size.height;

			M_new = cv::getOptimalNewCameraMatrix(M, D, size, 1, size);
			M_new = M; // TODO: excuse me wtf
			rgb_intrinsics_ = Vector4f(M_new.at<double>(0, 0), 
			                           M_new.at<double>(1, 1),
			                           M_new.at<double>(0, 2), 
			                           M_new.at<double>(1, 2));
			cv::initUndistortRectifyMap(M, D, cv::Mat(), M_new, size, CV_16SC2, 
			                            rgb_undistort_1_, rgb_undistort_2_);

			float focalScale = 1.0f;
			rgb_intrinsics_[0] *= focalScale;
			rgb_intrinsics_[1] *= focalScale;

		} else {
			// Do standard xtion stuff
			rgb_intrinsics_ = Vector4f(530, 530, 320, 240);
		}

		// Default values
		depth_intrinsics_ = Vector4f(568, 568, 320, 240); // the structure sensor
		//depth_intrinsics_ = Vector4f(570, 570, 320, 240); // xtion

		cv::Mat R, T, Rf, Tf;

		if(has_high_res_) {
			Matrix4f rot4 = Matrix4f::Identity();

			// TRACK 16 - 19 should work with these settings:
			// Tweaking of the calibration because the camera rack is not rigid
			Matrix3f rot = (AngleAxisf(0.010 * M_PI, Vector3f::UnitX()) *
			                AngleAxisf(0.002 * M_PI, Vector3f::UnitY()) *
			                AngleAxisf(0.000 * M_PI, Vector3f::UnitZ())
			                ).normalized().toRotationMatrix();
			Matrix4f rot41 = Matrix4f::Identity();
			rot41.block<3, 3>(0, 0) = rot;

			cv::FileStorage fs(folder + "/../extrinsics.yml", cv::FileStorage::READ);

			fs["R"] >> R;
			fs["T"] >> T;
			R.convertTo(Rf, CV_32FC1);
			T.convertTo(Tf, CV_32FC1);
			Matrix3f eR(reinterpret_cast<float*>(Rf.data));
			Vector3f eT(reinterpret_cast<float*>(Tf.data));

			rot4.block<3, 3>(0, 0) = eR;
			rot4.block<3, 1>(0, 3) = eT;

			depth_2_rgb_ =  rot41 * rot4;

		} else {
			depth_2_rgb_ = Matrix4f::Identity();
			depth_2_rgb_(0, 3) = 0.026f; // Standard xtion baseline
		}

		if(has_high_res_) {
			radiometric_response_ = new radical::RadiometricResponse(
				folder + "/../rgb_ids.crf");
			vignetting_response_  = new radical::VignettingResponse(
				folder + "/../rgb_ids.vgn");
		} else {
			radiometric_response_ = new radical::RadiometricResponse(
				folder + "/../rgb.crf");
			vignetting_response_  = new radical::VignettingResponse(
				folder + "/../rgb.vgn");
		}
	}
}

TumDataset::~TumDataset() {
	if(radiometric_response_ != nullptr) {
		delete radiometric_response_;
		delete vignetting_response_;
	}
}

void TumDataset::readNewSetOfImages() {
	if(frame_index_ != 0)
		frame_index_ += skip_count;

	if(frame_index_ < rgb_files_.size()) {
		current_depth_     = cv::imread(depth_files_[frame_index_], 
		                                cv::IMREAD_UNCHANGED) * scale_depth_;
		current_rgb_       = cv::imread(rgb_files_[frame_index_]);
		current_timestamp_ = timestamps_[frame_index_];

		if(!exposure_times_.empty()) {
			rgb_exposure_time_ = exposure_times_[frame_index_];
		}

		if(!depth_undistort_1_.empty()) {
			// Undistort the images
			cv::Mat current_depth_undistorted;
			cv::remap(current_depth_, current_depth_undistorted,
			          depth_undistort_1_, depth_undistort_2_, cv::INTER_NEAREST);
		}

		if(radiometric_response_ != nullptr && vignetting_response_ != nullptr) {
			cv::Mat irradiance, radiance;
			radiometric_response_->inverseMap(current_rgb_, irradiance);
			vignetting_response_->remove(irradiance, radiance);
			radiance *= 0.9;
			radiance *= 1.2;
			for (int i = 0; i < radiance.size().area(); i++) {
				cv::Vec3f v = radiance.at<cv::Vec3f>(i);
				v[0] *= white_fix_[0];
				v[1] *= white_fix_[1];
				v[2] *= white_fix_[2];
				radiance.at<cv::Vec3f>(i) = v;
			}
			radiometric_response_->directMap(radiance, current_rgb_);
		}

		if(!rgb_undistort_1_.empty()) {
			cv::Mat current_rgb_undistorted;
			cv::remap(current_rgb_, current_rgb_undistorted, 
			          rgb_undistort_1_, rgb_undistort_2_, cv::INTER_LINEAR);
			current_rgb_ = current_rgb_undistorted; 
			if(scale_depth_ != 1) {
				assert(0); // TODO: this scalefactor thingy really needs cleanup
			}
		}
		//as in the last datasets i collected
		//TODO: unify this over all datasets i use!

		//no scaling for the tumalike datasets

		//the highres datasets need scaling though:
		//current_depth_ = current_depth_*0.5f;// * 5; //scale by 5 in the high resolution dataset //between bla and 16

		frame_index_++;

	} else {
		running_ = false;
	}

	// If we are done before our time we wait....
	chrono::system_clock::time_point now = chrono::system_clock::now();
	if(replay_speed != 0) {
		chrono::system_clock::duration frame_time =
			chrono::microseconds((int)(33333.0f * (1.0f / replay_speed)));
		chrono::system_clock::duration duration = now - last_frame_readout_;
		if((frame_time - duration) > chrono::duration<int>(0))
			this_thread::sleep_for(frame_time - duration);
	}

	last_frame_readout_ = chrono::system_clock::now();
}


bool TumDataset::isRunning() {
	return running_;
}

cv::Mat TumDataset::getDepthFrame() {
	read_depth_ = true;
	if(read_depth_ && read_rgb_) {
		read_depth_ = false;
		read_rgb_ = false;
	}
	return current_depth_;
}

cv::Mat TumDataset::getRgbFrame() {
	read_rgb_ = true;
	if(read_depth_ && read_rgb_) {
		read_depth_ = false;
		read_rgb_ = false;
	}
	return current_rgb_;
}

Vector4f TumDataset::getDepthIntrinsics() {
	return depth_intrinsics_;
}

Vector4f TumDataset::getRgbIntrinsics() {
	return rgb_intrinsics_;
}

Matrix4f TumDataset::getDepth2RgbRegistration() {
	return depth_2_rgb_;
}

bool TumDataset::hasGroundTruth() {
	return has_poses_;
}

Matrix4f TumDataset::getDepthPose() {
	Matrix4f pose;
	double delta_time_min = 1000;
	for(auto p : trajectory_) {
		if(fabs(current_timestamp_ - p.timestamp) < delta_time_min) {
			delta_time_min = fabs(current_timestamp_ - p.timestamp);
			pose = p.position;
		}
	}
	return pose;
}

Matrix4f TumDataset::getRgbPose() {
	Matrix4f pose;
	double delta_time_min = 1000;
	for(auto p : trajectory_) {
		if(fabs(current_timestamp_ - p.timestamp) < delta_time_min) {
			delta_time_min = fabs(current_timestamp_ - p.timestamp);
			pose = p.position;
		}
	}
	return pose;
}

bool TumDataset::hasHighRes() {
	return has_high_res_;
}

float TumDataset::getRgbExposure() {
	return rgb_exposure_time_;
}
