#ifndef FILE_GEOMETRY_UPDATER_H
#define FILE_GEOMETRY_UPDATER_H

#include <memory>
#include <iostream>
#include <unordered_map>

#include <Eigen/Eigen>
#include <opencv2/core.hpp>

#include <mesher.h>
#include <mesh_stitcher.h>
#include <utils/gpu_norm_seg.h>

using namespace std;
using namespace Eigen;

class MeshReconstruction;
class ActiveSet;
class TextureUpdater;

namespace gfx {

class GpuTex2D;

} // namespace gfx

class GeometryUpdater {
public:
	void setup(MeshReconstruction *reconstruction) {
		mesh_reconstruction = reconstruction;
		stitching.setup(reconstruction);
		meshing.setup(reconstruction);
	}

	void extend(
			MeshReconstruction* reconstruction,
			InformationRenderer *information_renderer,
			TextureUpdater* texture_updater,
			LowDetailRenderer* low_detail_renderer,
			GpuStorage* gpu_storage,
			shared_ptr<ActiveSet> active_set_of_formerly_visible_patches,
			vector<shared_ptr<ActiveSet>> all_active_sets, //TODO: get up to date active sets by providing an
			// interface to a proper container
			shared_ptr<gfx::GpuTex2D> d_std_tex,
			cv::Mat &d_std_mat, Matrix4f depth_pose_in,
			shared_ptr<gfx::GpuTex2D> rgb_tex,
			Matrix4f color_pose_in);

	//TODO: this!!!!
	void update(GpuStorage* gpu_storage,
				shared_ptr<gfx::GpuTex2D> d_std_tex,
	            Matrix4f depth_pose_in,
	            shared_ptr<ActiveSet> &active_set);
	
	MeshReconstruction *mesh_reconstruction;
	Mesher meshing;
	MeshStitcher stitching;

	//TODO: integrate this here!
	GpuNormSeg gpu_pre_seg_;

	GeometryUpdater(	GarbageCollector* garbage_collector,
						int width,int height) :
							gpu_pre_seg_(garbage_collector,width,height) {}

};

#endif // FILE_GEOMETRY_UPDATE_H
