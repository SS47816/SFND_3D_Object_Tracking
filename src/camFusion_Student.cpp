
#include <iostream>
#include <algorithm>
#include <numeric>
#include <set>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <pcl/segmentation/extract_clusters.h>

#include "camFusion.hpp"
#include "dataStructures.h"

using namespace std;


// Create groups of Lidar points whose projection into the camera falls into the same bounding box
void clusterLidarWithROI(std::vector<BoundingBox> &boundingBoxes, std::vector<LidarPoint> &lidarPoints, float shrinkFactor, cv::Mat &P_rect_xx, cv::Mat &R_rect_xx, cv::Mat &RT)
{
    // loop over all Lidar points and associate them to a 2D bounding box
    cv::Mat X(4, 1, cv::DataType<double>::type);
    cv::Mat Y(3, 1, cv::DataType<double>::type);

    for (auto it1 = lidarPoints.begin(); it1 != lidarPoints.end(); ++it1)
    {
        // assemble vector for matrix-vector-multiplication
        X.at<double>(0, 0) = it1->x;
        X.at<double>(1, 0) = it1->y;
        X.at<double>(2, 0) = it1->z;
        X.at<double>(3, 0) = 1;

        // project Lidar point into camera
        Y = P_rect_xx * R_rect_xx * RT * X;
        cv::Point pt;
        pt.x = Y.at<double>(0, 0) / Y.at<double>(0, 2); // pixel coordinates
        pt.y = Y.at<double>(1, 0) / Y.at<double>(0, 2);

        vector<vector<BoundingBox>::iterator> enclosingBoxes; // pointers to all bounding boxes which enclose the current Lidar point
        for (vector<BoundingBox>::iterator it2 = boundingBoxes.begin(); it2 != boundingBoxes.end(); ++it2)
        {
            // shrink current bounding box slightly to avoid having too many outlier points around the edges
            cv::Rect smallerBox;
            smallerBox.x = (*it2).roi.x + shrinkFactor * (*it2).roi.width / 2.0;
            smallerBox.y = (*it2).roi.y + shrinkFactor * (*it2).roi.height / 2.0;
            smallerBox.width = (*it2).roi.width * (1 - shrinkFactor);
            smallerBox.height = (*it2).roi.height * (1 - shrinkFactor);

            // check wether point is within current bounding box
            if (smallerBox.contains(pt))
            {
                enclosingBoxes.push_back(it2);
            }

        } // eof loop over all bounding boxes

        // check wether point has been enclosed by one or by multiple boxes
        if (enclosingBoxes.size() == 1)
        { 
            // add Lidar point to bounding box
            enclosingBoxes[0]->lidarPoints.push_back(*it1);
        }

    } // eof loop over all Lidar points
}


void show3DObjects(std::vector<BoundingBox> &boundingBoxes, cv::Size worldSize, cv::Size imageSize, bool bWait)
{
    // create topview image
    cv::Mat topviewImg(imageSize, CV_8UC3, cv::Scalar(255, 255, 255));

    for(auto it1=boundingBoxes.begin(); it1!=boundingBoxes.end(); ++it1)
    {
        // create randomized color for current 3D object
        cv::RNG rng(it1->boxID);
        cv::Scalar currColor = cv::Scalar(rng.uniform(0,150), rng.uniform(0, 150), rng.uniform(0, 150));

        // plot Lidar points into top view image
        int top=1e8, left=1e8, bottom=0.0, right=0.0; 
        float xwmin=1e8, ywmin=1e8, ywmax=-1e8;
        for (auto it2 = it1->lidarPoints.begin(); it2 != it1->lidarPoints.end(); ++it2)
        {
            // world coordinates
            float xw = (*it2).x; // world position in m with x facing forward from sensor
            float yw = (*it2).y; // world position in m with y facing left from sensor
            xwmin = xwmin<xw ? xwmin : xw;
            ywmin = ywmin<yw ? ywmin : yw;
            ywmax = ywmax>yw ? ywmax : yw;

            // top-view coordinates
            int y = (-xw * imageSize.height / worldSize.height) + imageSize.height;
            int x = (-yw * imageSize.width / worldSize.width) + imageSize.width / 2;

            // find enclosing rectangle
            top = top<y ? top : y;
            left = left<x ? left : x;
            bottom = bottom>y ? bottom : y;
            right = right>x ? right : x;

            // draw individual point
            cv::circle(topviewImg, cv::Point(x, y), 4, currColor, -1);
        }

        // draw enclosing rectangle
        cv::rectangle(topviewImg, cv::Point(left, top), cv::Point(right, bottom),cv::Scalar(0,0,0), 2);

        // augment object with some key data
        char str1[200], str2[200];
        sprintf(str1, "id=%d, #pts=%d", it1->boxID, (int)it1->lidarPoints.size());
        putText(topviewImg, str1, cv::Point2f(left-250, bottom+50), cv::FONT_ITALIC, 2, currColor);
        sprintf(str2, "xmin=%2.2f m, yw=%2.2f m", xwmin, ywmax-ywmin);
        putText(topviewImg, str2, cv::Point2f(left-250, bottom+125), cv::FONT_ITALIC, 2, currColor);  
    }

    // plot distance markers
    float lineSpacing = 2.0; // gap between distance markers
    int nMarkers = floor(worldSize.height / lineSpacing);
    for (size_t i = 0; i < nMarkers; ++i)
    {
        int y = (-(i * lineSpacing) * imageSize.height / worldSize.height) + imageSize.height;
        cv::line(topviewImg, cv::Point(0, y), cv::Point(imageSize.width, y), cv::Scalar(255, 0, 0));
    }

    // display image
    string windowName = "3D Objects";
    cv::namedWindow(windowName, 2);
    cv::imshow(windowName, topviewImg);

    if(bWait)
    {
        cv::waitKey(0); // wait for key to be pressed
    }
}


// associate a given bounding box with the keypoints it contains
void clusterKptMatchesWithROI(BoundingBox &prev_BB, BoundingBox &curr_BB, std::vector<cv::KeyPoint> &kptsPrev, std::vector<cv::KeyPoint> &kptsCurr, std::vector<cv::DMatch> &kptMatches)
{
    for (auto it = kptMatches.begin(); it != kptMatches.end(); ++it)
    {
        auto& curr_kpt = kptsCurr.at(it->trainIdx);
        if (!curr_BB.roi.contains(curr_kpt.pt))
            continue;
        auto& prev_kpt = kptsPrev.at(it->queryIdx);
        if (!prev_BB.roi.contains(prev_kpt.pt))
            continue;

        curr_BB.kptMatches.push_back(*it);
    }
}


// Compute time-to-collision (TTC) based on keypoint correspondences in successive images
void computeTTCCamera(std::vector<cv::KeyPoint> &kptsPrev, std::vector<cv::KeyPoint> &kptsCurr, 
                      std::vector<cv::DMatch> kptMatches, double frameRate, double &TTC, cv::Mat *visImg)
{
    vector<double> dist_ratios;

    // loop through all the matches
    for (auto it1 = kptMatches.begin(); it1 != kptMatches.end(); ++it1)
    {
        auto& prev_kpt_1 = kptsPrev.at(it1->queryIdx);
        auto& curr_kpt_1 = kptsCurr.at(it1->trainIdx);

        // compoute distances between every points
        for (auto it2 = it1 + 1; it2 != kptMatches.end(); ++it2)
        {
            const double k_min_dist = 100.0;
            
            auto& prev_kpt_2 = kptsPrev.at(it2->queryIdx);
            auto& curr_kpt_2 = kptsCurr.at(it2->trainIdx);

            const double prev_dist = cv::norm(prev_kpt_1.pt - prev_kpt_2.pt);
            const double curr_dist = cv::norm(curr_kpt_1.pt - prev_kpt_2.pt);

            if (prev_dist >= k_min_dist && curr_dist >= k_min_dist)
            {
                dist_ratios.emplace_back(curr_dist / prev_dist);
            }
        }
    }
    
    const double dt = 1.0 / frameRate;
    if (dist_ratios.size() > 0)
    {
        std::sort(dist_ratios.begin(), dist_ratios.end());
        const int median_index = std::floor(dist_ratios.size() / 2);
        // const double centre_cells_ratio = 0.2;
        // const int num_centre_cells = centre_cells_ratio * dist_ratios.size() / 2;
        // double sum = 0.0;
        // for (int i = median_index - num_centre_cells; i <= median_index + num_centre_cells; ++i)
        //     sum += dist_ratios[i];
        
        // const double ratio = sum / (2*num_centre_cells + 1);
        
        const double ratio = dist_ratios[median_index];
        
        const double max_range = 99.99;
        if ((ratio - 1) >= 0.0001)
        {
            TTC = -dt / (1 - ratio);
            TTC = TTC > max_range ? max_range : TTC;
        }
        else
        {
            TTC = max_range;
        }
        
    }
    else
    {
        // std::cout << "No valid camera-based distance-ratio computed!" << std::endl;
        TTC = 99.99;
    }
}


// Filter/Cluster the pointcloud and compute the minimum distance from the object to ego pose in the x direction
double computeMinDistanceLidar(std::vector<LidarPoint> &lidarPoints, const double clusterTolerance, const int minSize, const double minR)
{
    pcl::PointCloud<pcl::PointXYZ>::Ptr input_cloud(new pcl::PointCloud<pcl::PointXYZ>);
    std::vector<pcl::PointCloud<pcl::PointXYZ>::Ptr> clusters;
    
    // filter out the low reflectivity points
    for (const auto& pt : lidarPoints)
    {
        if (pt.r < minR) continue;
        input_cloud->push_back(pcl::PointXYZ(pt.x, pt.y, pt.z));
    }

    // add points to kdTree structure
    pcl::search::KdTree<pcl::PointXYZ>::Ptr kdTree (new pcl::search::KdTree<pcl::PointXYZ>);
    kdTree->setInputCloud(input_cloud);

    // perform a euclidean clustering
    std::vector<pcl::PointIndices> clusters_indices;
    pcl::EuclideanClusterExtraction<pcl::PointXYZ> ec;
    ec.setClusterTolerance(clusterTolerance); 
    ec.setMinClusterSize(minSize);
    ec.setSearchMethod(kdTree);
    ec.setInputCloud(input_cloud);
    ec.extract(clusters_indices);

    // loop through all points to get the min distance in x direction
    double min_dist = std::numeric_limits<double>::max();
    
    for (auto cluster_indices : clusters_indices)
    {
        for (auto index : cluster_indices.indices)
            min_dist = std::min(min_dist, (double)input_cloud->points[index].x);
    }

    return min_dist;
}


void computeTTCLidar(std::vector<LidarPoint> &lidarPointsPrev,
                     std::vector<LidarPoint> &lidarPointsCurr, double frameRate, double &TTC)
{
    const double clusterTolerance = 0.2;
    const int minSize = 5;
    const double minR = 0.1;
    
    const double prev_dist = computeMinDistanceLidar(lidarPointsPrev, clusterTolerance, minSize, minR);
    const double curr_dist = computeMinDistanceLidar(lidarPointsCurr, clusterTolerance, minSize, minR);

    const double max_range = 99.99;
    const double dt = 1.0 / frameRate;
    // TTC = (prev_dist - curr_dist) > 0.0 ? curr_dist / (prev_dist - curr_dist) * dt : max_range;
    
    if ((prev_dist - curr_dist) >= 0.0001)
    {
        TTC = curr_dist / (prev_dist - curr_dist) * dt;
        TTC = TTC > max_range ? max_range : TTC;
    }
    else
    {
        TTC = max_range;
    }
    
}


void matchBoundingBoxes(std::vector<cv::DMatch> &matches, std::map<int, int> &bbBestMatches, DataFrame &prevFrame, DataFrame &currFrame)
{
    std::map<std::pair<int, int>, int> BB_matching_count;
    for (const auto& match : matches)
    {
        const int prev_index = match.queryIdx;
        const int curr_index = match.trainIdx;
        const auto& prev_kpt = prevFrame.keypoints[prev_index].pt;
        const auto& curr_kpt = currFrame.keypoints[curr_index].pt;

        for (const auto& prev_BB : prevFrame.boundingBoxes)
        {
            if (!prev_BB.roi.contains(prev_kpt)) 
                continue;
            
            for (const auto& curr_BB : currFrame.boundingBoxes)
            {
                if (!curr_BB.roi.contains(curr_kpt)) 
                    continue;
                BB_matching_count[std::make_pair(prev_BB.boxID, curr_BB.boxID)]++;
            }
        }
    }

    std::vector<std::pair<std::pair<int, int>, int>> num_matches;
    for (const auto& match_count : BB_matching_count)
    {
        num_matches.emplace_back(match_count);
    }

    std::sort(num_matches.begin(), num_matches.end(), [](const std::pair<std::pair<int, int>, int>& a, const std::pair<std::pair<int, int>, int>& b)
    {
        return a.second > b.second;
    });

    bbBestMatches.clear();
    std::set<int> visited_prev_BB;
    for (const auto& match : num_matches)
    {
        if (visited_prev_BB.count(match.first.first))
            continue;
        visited_prev_BB.insert(match.first.first);
        bbBestMatches.insert(match.first);
    }
}
