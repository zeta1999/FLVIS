#include <pluginlib/class_list_macros.h>
#include <nodelet/nodelet.h>
#include <ros/ros.h>
#include <message_filters/subscriber.h>
#include <message_filters/time_synchronizer.h>
#include <message_filters/synchronizer.h>
#include <sensor_msgs/Image.h>

#include <opencv2/opencv.hpp>
#include <cv_bridge/cv_bridge.h>
#include <image_transport/image_transport.h>

#include <g2o/core/base_vertex.h>
#include <g2o/core/base_unary_edge.h>
#include <g2o/core/block_solver.h>
#include <g2o/core/optimization_algorithm_levenberg.h>
#include <g2o/solvers/csparse/linear_solver_csparse.h>
#include <g2o/types/sba/types_six_dof_expmap.h>

#include <utils/tic_toc_ros.h>
#include <include/common.h>
#include <include/depth_camera.h>
#include <include/feature_dem.h>
#include <include/rviz_frame.h>
#include <include/rviz_path.h>
#include <include/camera_frame.h>
#include <include/yamlRead.h>
#include <include/cv_draw.h>
#include <vo_nodelet/KeyFrame.h>
#include <vo_nodelet/CorrectionInf.h>
#include <include/keyframe_msg.h>
#include <include/bundle_adjustment.h>
#include <include/correction_inf_msg.h>
#include <include/octomap_feeder.h>




enum TRACKINGSTATE{UnInit, Working, trackingFail};

namespace vo_nodelet_ns
{



struct ID_POSE {
    int    frame_id;
    SE3    T_c_w;
} ;


class VOTrackingNodeletClass : public nodelet::Nodelet
{
public:
    VOTrackingNodeletClass()  {;}
    ~VOTrackingNodeletClass() {;}

private:

    message_filters::Subscriber<sensor_msgs::Image> image_sub;
    message_filters::Subscriber<sensor_msgs::Image> depth_sub;
    typedef message_filters::sync_policies::ExactTime<sensor_msgs::Image, sensor_msgs::Image> MyExactSyncPolicy;
    message_filters::Synchronizer<MyExactSyncPolicy> * exactSync_;
    ros::Subscriber correction_inf_sub;

    //State Machine
    enum TRACKINGSTATE   vo_tracking_state;

    //Tools
    FeatureDEM         *featureDEM;

    //F2F
    int frameCount;
    Mat cameraMatrix;
    Mat distCoeffs;
    int image_width,image_height;
    CameraFrame::Ptr curr_frame,last_frame;
    SE3 T_c_w_last_keyframe;
    //Pose Records and Correction Information
    deque<ID_POSE> pose_records;
    bool has_feedback;
    CorrectionInfStruct correction_inf;

    //Visualization
    Mat currShowImg;
    RVIZFrame* frame_pub;
    RVIZPath*  path_pub;
    OctomapFeeder* octomap_pub;

    KeyFrameMsg* kf_pub;
    image_transport::ImageTransport *it;
    image_transport::Publisher cv_pub;



    virtual void onInit()
    {
        ros::NodeHandle& nh = getPrivateNodeHandle();
        //cv::startWindowThread(); //Bug report https://github.com/ros-perception/image_pipeline/issues/201

        //Load Parameter
        string configFilePath;
        nh.getParam("/yamlconfigfile",   configFilePath);
        image_width  = getIntVariableFromYaml(configFilePath,"image_width");
        image_height = getIntVariableFromYaml(configFilePath,"image_height");
        cameraMatrix = cameraMatrixFromYamlIntrinsics(configFilePath);
        distCoeffs = distCoeffsFromYaml(configFilePath);
        double fx = cameraMatrix.at<double>(0,0);
        double fy = cameraMatrix.at<double>(1,1);
        double cx = cameraMatrix.at<double>(0,2);
        double cy = cameraMatrix.at<double>(1,2);
        cout << "cameraMatrix:" << endl << cameraMatrix << endl
             << "distCoeffs:" << endl << distCoeffs << endl
             << "image_width: "  << image_width << " image_height: "  << image_height << endl
             << "fx: "  << fx << " fy: "  << fy <<  " cx: "  << cx <<  " cy: "  << cy << endl;

        featureDEM  = new FeatureDEM(image_width,image_height,3);

        curr_frame = std::make_shared<CameraFrame>();
        last_frame = std::make_shared<CameraFrame>();
        curr_frame->height = last_frame->height = image_height;
        curr_frame->width = last_frame->width = image_width;
        curr_frame->d_camera = last_frame->d_camera = DepthCamera(fx,fy,cx,cy,1000.0);

        frameCount = 0;
        vo_tracking_state = UnInit;
        has_feedback = false;

        //Publish
        path_pub  = new RVIZPath(nh,"/vo_path");
        frame_pub = new RVIZFrame(nh,"/vo_camera_pose","/vo_curr_frame");
        kf_pub    = new KeyFrameMsg(nh,"/vo_kf");
        octomap_pub = new OctomapFeeder(nh,"/vo_octo_tracking","vo_local",1);
        octomap_pub->d_camera=curr_frame->d_camera;
        it = new image_transport::ImageTransport(nh);
        cv_pub = it->advertise("/vo_img", 1);

        //Subscribe
        //Sync Subscribe Image and Rectified Depth Image
        image_sub.subscribe(nh, "/vo/image", 1);
        depth_sub.subscribe(nh, "/vo/depth_image", 1);
        exactSync_ = new message_filters::Synchronizer<MyExactSyncPolicy>(MyExactSyncPolicy(3), image_sub, depth_sub);
        exactSync_->registerCallback(boost::bind(&VOTrackingNodeletClass::image_input_callback, this, _1, _2));
        //Correction information
        correction_inf_sub = nh.subscribe<vo_nodelet::CorrectionInf>(
                    "/vo_localmap_feedback",
                    1,
                    boost::bind(&VOTrackingNodeletClass::correction_feedback_callback, this, _1));
        cout << "init Subscribe" << endl;

        cout << "onInit() finished" << endl;
    }

    void correction_feedback_callback(const vo_nodelet::CorrectionInf::ConstPtr& msg)
    {
        //unpacking and update the structure
        CorrectionInfMsg::unpack(msg,
                                 correction_inf.frame_id,
                                 correction_inf.T_c_w,
                                 correction_inf.lm_count,
                                 correction_inf.lm_id,
                                 correction_inf.lm_3d,
                                 correction_inf.lm_outlier_count,
                                 correction_inf.lm_outlier_id);
        has_feedback=true;
//        cout << "Tracking: correction frame id:" << correction_inf.frame_id << endl;
//        cout<<  "Tracking: correction Twc: " << correction_inf.T_c_w.inverse().so3().log().transpose() << " | "
//             << correction_inf.T_c_w.inverse().translation().transpose() << endl;
//        cout << "Tracking: correction landmark count:"  << correction_inf.lm_count << endl;
//        for(int i=0; i< correction_inf.lm_count; i++)
//        {
//            cout << "lm_id"  << correction_inf.lm_id.at(i) <<"  "<< correction_inf.lm_3d.at(i).transpose() << endl;
//        }
//        cout << "correction landmark outlier_count:" << correction_inf.lm_outlier_count << endl;
//        for(int i=0; i< correction_inf.lm_outlier_count; i++)
//        {
//            cout << "lm_outlier_id"  << correction_inf.lm_outlier_id.at(i) << endl;
//        }
    }

    void image_input_callback(const sensor_msgs::ImageConstPtr & imgPtr, const sensor_msgs::ImageConstPtr & depthImgPtr)
    {
        tic_toc_ros tt_cb;

        frameCount++;
        if(frameCount<30) return;

        //15hz Frame Rate
        //if((frameCount%2)==0) return;
        //cout << endl << "Frame No: " << frameCount << endl;
        curr_frame->frame_id = frameCount;
        //Greyscale Img
        curr_frame->clear();
        cv_bridge::CvImagePtr cvbridge_image  = cv_bridge::toCvCopy(imgPtr, imgPtr->encoding);
        curr_frame->img=cvbridge_image->image;
        //equalizeHist(curr_frame->img, curr_frame->img);
        //Depth Img
        cv_bridge::CvImagePtr cvbridge_depth_image  = cv_bridge::toCvCopy(depthImgPtr, depthImgPtr->encoding);
        curr_frame->d_img=cvbridge_depth_image->image;

        cvtColor(curr_frame->img,currShowImg,CV_GRAY2BGR);
        ros::Time currStamp = imgPtr->header.stamp;

        switch(vo_tracking_state)
        {
        case UnInit:
        {
            Mat3x3 R;
            // 0  0  1
            //-1  0  0
            // 0 -1  0
            R << 0, 0, 1, -1, 0, 0, 0,-1, 0;
            Mat3x3 R_roll_m30deg;
            R_roll_m30deg << 1.0,  0.0,  0.0,
                    0.0,  1,  0,
                    0.0, 0.0,  1;
            Vec3   t=Vec3(0,0,1);
            SE3    T_w_c(R*R_roll_m30deg,t);
            curr_frame->T_c_w=T_w_c.inverse();//Tcw = (Twc)^-1

            vector<Vec2> pts2d;
            vector<Vec3> pts3d_c;
            vector<Mat>  descriptors;
            vector<bool> maskHas3DInf;

            featureDEM->detect(curr_frame->img,pts2d,descriptors);
            cout << "Detect " << pts2d.size() << " Features"<< endl;
            for(size_t i=0; i<pts2d.size(); i++)
            {
                curr_frame->landmarks.push_back(LandMarkInFrame(descriptors.at(i),
                                                                pts2d.at(i),
                                                                Vec3(0,0,0),
                                                                false,
                                                                curr_frame->T_c_w));
            }
            curr_frame->depthInnovation();


            drawKeyPts(currShowImg, vVec2_2_vcvP2f(pts2d));
            frame_pub->pubFramePtsPoseT_c_w(curr_frame->getValid3dPts(),curr_frame->T_c_w,currStamp);
            path_pub->pubPathT_c_w(curr_frame->T_c_w,currStamp);

            kf_pub->pub(*curr_frame,currStamp);
            T_c_w_last_keyframe = curr_frame->T_c_w;

            vo_tracking_state = Working;
            cout << "vo_tracking_state = Working" << endl;
            break;
        }


        case Working:
        {
            /*Recover from LocalMap Feedback msg
             *
            */
            if(has_feedback)
            {
                //find pose;
                int corr_id = correction_inf.frame_id;
                int old_pose_idx = 0;
                for(int i=(pose_records.size()-1); i>=0; i--)
                {
                    if(pose_records.at(i).frame_id==corr_id)
                    {
                        old_pose_idx=i;
                        break;
                    }
                }
                SE3 old_T_c_w = pose_records.at(old_pose_idx).T_c_w;
                SE3 old_T_c_w_inv = old_T_c_w.inverse();
                SE3 update_T_c_w = correction_inf.T_c_w;
                //update pose records
                for(int i=old_pose_idx; i<pose_records.size(); i++)
                {
                    SE3 T_diff= pose_records.at(i).T_c_w * old_T_c_w_inv;
                    pose_records.at(i).T_c_w = T_diff * update_T_c_w;
                }
                //update last_frame pose and landmake p3d throuth transformation
                for(auto lm:last_frame->landmarks)
                {
                    //cout << lm.lm_id << "befor FB "<< lm.lm_3d_w.transpose() << endl;
                }
                SE3 T_diff= last_frame->T_c_w * old_T_c_w_inv;
                last_frame->T_c_w = T_diff * update_T_c_w;
                last_frame->correctLMP3DWByLMP3DCandT();
                //update last_frame landmake lm_3d_w and mask outlier
                last_frame->forceCorrectLM3DW(correction_inf.lm_count,correction_inf.lm_id,correction_inf.lm_3d);
                last_frame->forceMarkOutlier(correction_inf.lm_outlier_count,correction_inf.lm_outlier_id);
                for(auto lm:last_frame->landmarks)
                {
                    //cout << lm.lm_id << "after FB " << lm.lm_3d_w.transpose() << endl;
                }
                //maek last_frame outlier
                has_feedback = false;
            }

            /* F2F Workflow
                     STEP1: Track Match and Update to curr_frame
                     STEP2: 2D3D-PNP+F2FBA
                     STEP3: Redetect
                     STEP4: Update Landmarks(IIR)
            */
            //STEP1:
            vector<Vec2> lm2d_from;
            vector<Vec2> lm2d_to;
            vector<Vec2> outlier_tracking;
            last_frame->trackMatchAndEraseOutlier(curr_frame->img,
                                                  lm2d_from,
                                                  lm2d_to,
                                                  outlier_tracking);
            for(size_t i=0; i<last_frame->landmarks.size(); i++)
            {
                curr_frame->landmarks.push_back(last_frame->landmarks.at(i));
                curr_frame->landmarks.at(i).lm_2d=lm2d_to.at(i);
            }

            //STEP2:
            vector<Point2f> p2d;
            vector<Point3f> p3d;
            curr_frame->getValid2d3dPair_cvPf(p2d,p3d);
            cv::Mat r_ = cv::Mat::zeros(3, 1, CV_64FC1);
            cv::Mat t_ = cv::Mat::zeros(3, 1, CV_64FC1);
            SE3_to_rvec_tvec(last_frame->T_c_w, r_ , t_ );

            Mat inliers;

            solvePnPRansac(p3d,p2d,cameraMatrix,distCoeffs,r_,t_,false,100,8.0,0.99,inliers,SOLVEPNP_P3P);


            curr_frame->T_c_w = SE3_from_rvec_tvec(r_,t_);
            //Remove Outliers ||reprojection error|| > MAD of all reprojection error
            vector<Vec2> outlier_reproject;
            double mean_error;
            curr_frame->CalReprjInlierOutlier(mean_error,outlier_reproject,2.0);
            //bundleAdjustment::BAInFrame(*curr_frame);
            //cout << "Reprojection Error " << mean_error << endl;

            //Refill the keyPoints
            vector<Vec2> newKeyPts;
            vector<Mat>  newDescriptor;
            int newPtsCount;
            featureDEM->redetect(curr_frame->img,
                                 curr_frame->get2dPtsVec(),
                                 newKeyPts,newDescriptor,newPtsCount);

            //add landmarks without 3D information
            Vec3 pt3d_w(0,0,0);
            for(size_t i=0; i<newKeyPts.size(); i++)
            {
                curr_frame->landmarks.push_back(LandMarkInFrame(newDescriptor.at(i),
                                                                newKeyPts.at(i),
                                                                Vec3(0,0,0),
                                                                false,
                                                                curr_frame->T_c_w));
            }
            //innovate 3D information
            curr_frame->depthInnovation();

            //record the pose
            ID_POSE tmp;
            tmp.frame_id = curr_frame->frame_id;
            tmp.T_c_w = curr_frame->T_c_w;
            pose_records.push_back(tmp);
            if(pose_records.size() >= 100)
            {
                pose_records.pop_front();
            }


            //visualize and publish
            vector<Vec2> outlier;
            outlier.insert(outlier.end(), outlier_tracking.begin(), outlier_tracking.end());
            outlier.insert(outlier.end(), outlier_reproject.begin(), outlier_reproject.end());
            drawOutlier(currShowImg,outlier);
            drawFlow(currShowImg,lm2d_from,lm2d_to);
            drawRegion16(currShowImg);
            sensor_msgs::ImagePtr img_msg = cv_bridge::CvImage(std_msgs::Header(), "bgr8", currShowImg).toImageMsg();
            cv_pub.publish(img_msg);

            frame_pub->pubFramePtsPoseT_c_w(curr_frame->getValid3dPts(),curr_frame->T_c_w);
            path_pub->pubPathT_c_w(curr_frame->T_c_w,currStamp);
            octomap_pub->pub(curr_frame->T_c_w,curr_frame->d_img,currStamp);
//            tf_pub->pubTFT_c_w(curr_frame->T_c_w,currStamp);

            SE3 T_diff_key_curr = T_c_w_last_keyframe*(curr_frame->T_c_w.inverse());
            Vec3 t=T_diff_key_curr.translation();
            Vec3 r=T_diff_key_curr.so3().log();
            double t_norm = fabs(t[0]) + fabs(t[1]) + fabs(t[2]);
            double r_norm = fabs(r[0]) + fabs(r[1]) + fabs(r[2]);

            if(t_norm>=0.15 || r_norm>=2.0)
            {
                kf_pub->pub(*curr_frame,currStamp);
                T_c_w_last_keyframe = curr_frame->T_c_w;
//                cout << "Tracking: F " << frameCount << " as KF, t_norm: " << t_norm << " r_norm: "<< r_norm
//                     << " Twc: " << curr_frame->T_c_w.inverse().so3().log().transpose() << " | "
//                     << curr_frame->T_c_w.inverse().translation().transpose() << endl;
            }
            //            if(frameCount%4==0)
            //            {
            //                kf_pub->pub(*curr_frame,currStamp);
            //            }
            break;
        }

        case trackingFail:
        {
            vo_tracking_state = UnInit;
            break;
        }

        default:
            cout<<"error"<<endl;
        }//end of state machine



        waitKey(1);
        last_frame.swap(curr_frame);

        tt_cb.toc();
    }//image_input_callback(const sensor_msgs::ImageConstPtr & imgPtr, const sensor_msgs::ImageConstPtr & depthImgPtr)






};//class VOTrackingNodeletClass
}//namespace vo_nodelet_ns

PLUGINLIB_EXPORT_CLASS(vo_nodelet_ns::VOTrackingNodeletClass, nodelet::Nodelet)


