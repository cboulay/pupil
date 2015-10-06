
#include <opencv2/core/core.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <iostream>

#include "singleeyefitter/common/types.h"
#include "singleeyefitter/common/colors.h"

#include "singleeyefitter/fun.h"
#include "singleeyefitter/utils.h"
#include "singleeyefitter/cvx.h"
#include "singleeyefitter/Ellipse.h"  // use ellipse eyefitter
#include "singleeyefitter/distance.h"
#include "singleeyefitter/mathHelper.h"
#include "singleeyefitter/detectorUtils.h"
#include "singleeyefitter/EllipseDistanceApproxCalculator.h"
#include "singleeyefitter/EllipseEvaluation.h"


template<typename Scalar>
struct Result{
  typedef singleeyefitter::Ellipse2D<Scalar> Ellipse;
  double confidence =  0.0 ;
  Ellipse ellipse;
  double timeStamp = 0.0;
};

// use a struct for all properties and pass it to detect method every time we call it.
// Thus we don't need to keep track if GUI is updated and cython handles conversion from Dict to struct
struct DetectProperties{
  int intensity_range;
  int blur_size;
  float canny_treshold;
  float canny_ration;
  int canny_aperture;
  int pupil_size_max;
  int pupil_size_min;
  float strong_perimeter_ratio_range_min;
  float strong_perimeter_ratio_range_max;
  float strong_area_ratio_range_min;
  float strong_area_ratio_range_max;
  int contour_size_min;
  float ellipse_roundness_ratio;
  float initial_ellipse_fit_treshhold;
  float final_perimeter_ratio_range_min;
  float final_perimeter_ratio_range_max;

};


template< typename Scalar >
class Detector2D{

private:
  typedef singleeyefitter::Ellipse2D<Scalar> Ellipse;


public:

  Detector2D();
  Result<Scalar> detect(DetectProperties& props, cv::Mat& image, cv::Mat& color_image, cv::Mat& debug_image, cv::Rect& usr_roi ,  cv::Rect& pupil_roi, bool visualize, bool use_debug_image);
  std::vector<cv::Point> ellipse_true_support(Ellipse& ellipse, Scalar ellipse_circumference, std::vector<cv::Point>& raw_edges);


private:

  bool mUse_strong_prior;
  int mPupil_Size;
  Ellipse mPrior_ellipse;



};

void printPoints( std::vector<cv::Point> points){
  std::for_each(points.begin(), points.end(), [](cv::Point& p ){ std::cout << p << std::endl;} );
}

template <typename Scalar>
Detector2D<Scalar>::Detector2D(): mUse_strong_prior(false), mPupil_Size(100){};

template <typename Scalar>
std::vector<cv::Point> Detector2D<Scalar>::ellipse_true_support( Ellipse& ellipse, Scalar ellipse_circumference, std::vector<cv::Point>& raw_edges){

  Scalar major_radius = ellipse.major_radius;
  Scalar minor_radius = ellipse.minor_radius;

  //std::cout << ellipse_circumference << std::endl;
  std::vector<Scalar> distances;
  std::vector<cv::Point> support_pixels;
  EllipseDistCalculator<Scalar> ellipseDistance(ellipse);

  for(auto it = raw_edges.begin(); it != raw_edges.end(); it++){

      const cv::Point p = *it;
      Scalar distance = ellipseDistance( (Scalar)p.x, (Scalar)p.y );  // change this one, to approxx ?
      if(distance <=  1.3 ){
        support_pixels.push_back(p);
      }
  }

  return std::move(support_pixels);
}
template<typename Scalar>
Result<Scalar> Detector2D<Scalar>::detect( DetectProperties& props, cv::Mat& image, cv::Mat& color_image, cv::Mat& debug_image, cv::Rect& usr_roi , cv::Rect& pupil_roi,bool visualize, bool use_debug_image ){

  Result<Scalar> result;

  //remove this later
  debug_image = cv::Scalar(0); //clear debug image

  cv::Rect roi = usr_roi & pupil_roi;  // intersect rectangles
  if( roi.area() < 1.0 )
    roi = usr_roi;

  const int image_width = image.size().width;
  const int image_height = image.size().height;

  const cv::Mat pupil_image = cv::Mat(image, roi);  // image with usr_roi
  const int w = pupil_image.size().width/2;
  const float coarse_pupil_width = w/2.0f;
  const int padding = int(coarse_pupil_width/4.0f);

  const int offset = props.intensity_range;
  const int spectral_offset = 5;

  cv::Mat histogram;
  int histSize;
  histSize = 256; //from 0 to 255
  /// Set the ranges
  float range[] = { 0, 256 } ; //the upper boundary is exclusive
  const float* histRange = { range };

  cv::calcHist( &pupil_image, 1 , 0, cv::Mat(), histogram , 1 , &histSize, &histRange, true, false );


  int lowest_spike_index = 255;
  int highest_spike_index = 0;
  float max_intensity = 0;

  singleeyefitter::detector::calculate_spike_indices_and_max_intenesity( histogram, 40, lowest_spike_index, highest_spike_index, max_intensity);

  if( visualize ){

      const int scale_x  = 100;
      const int scale_y = 1 ;
      // display the histogram and the spikes
      for(int i = 0; i < histogram.rows; i++){
        const float norm_i  = histogram.ptr<float>(i)[0]/max_intensity ; // normalized intensity
        cv::line( color_image, {image_width, i*scale_y}, { image_width - int(norm_i * scale_x), i * scale_y}, mBlue_color );
      }
      cv::line(color_image, {image_width, lowest_spike_index* scale_y}, { int(image_width - 0.5f * scale_x), lowest_spike_index * scale_y }, mRed_color);
      cv::line(color_image, {image_width, (lowest_spike_index+offset)* scale_y}, { int(image_width - 0.5f * scale_x), (lowest_spike_index + offset)* scale_y }, mYellow_color);
      cv::line(color_image, {image_width, (highest_spike_index)* scale_y}, { int(image_width - 0.5f * scale_x), highest_spike_index* scale_y }, mRed_color);
      cv::line(color_image, {image_width, (highest_spike_index - spectral_offset)* scale_y}, { int(image_width - 0.5f * scale_x), (highest_spike_index - spectral_offset)* scale_y }, mWhite_color);

  }

   //create dark and spectral glint masks
  cv::Mat binary_img;
  cv::Mat kernel;
  cv::inRange( pupil_image, cv::Scalar(0) , cv::Scalar(lowest_spike_index + props.intensity_range), binary_img );  // binary threshold
  kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, {7,7} );
  cv::dilate( binary_img, binary_img, kernel, {-1,-1}, 2 );

  cv::Mat spec_mask;
  cv::inRange( pupil_image, cv::Scalar(0) , cv::Scalar(highest_spike_index - spectral_offset), spec_mask );  // binary threshold
  cv::erode( spec_mask, spec_mask, kernel);

  kernel = cv::getStructuringElement(cv::MORPH_ELLIPSE, {9,9} );
  //open operation to remove eye lashes
  cv::morphologyEx( pupil_image, pupil_image, cv::MORPH_OPEN, kernel);

  if( props.blur_size > 1 )
    cv::medianBlur(pupil_image, pupil_image, props.blur_size );

  cv::Mat edges;
  cv::Canny( pupil_image, edges, props.canny_treshold, props.canny_treshold * props.canny_ration, props.canny_aperture);

  //remove edges in areas not dark enough and where the glint is (spectral refelction from IR leds)
  cv::min(edges, spec_mask, edges);
  cv::min(edges, binary_img, edges);


  if( visualize ){
    // get sub matrix
    cv::Mat overlay = cv::Mat(color_image, roi );
    cv::Mat g_channel( overlay.rows, overlay.cols, CV_8UC1 );
    cv::Mat b_channel( overlay.rows, overlay.cols, CV_8UC1 );
    cv::Mat r_channel( overlay.rows, overlay.cols, CV_8UC1 );
    cv::Mat out[] = {b_channel, g_channel,r_channel};
    cv:split(overlay, out);

    cv::max(g_channel, edges,g_channel);
    cv::max(b_channel, binary_img,b_channel);
    cv::min(b_channel, spec_mask,b_channel);

    cv::merge(out, 3, overlay);

    //draw a frame around the automatic pupil ROI in overlay.
    auto rect = cv::Rect(0, 0, overlay.size().width, overlay.size().height);
    cvx::draw_dotted_rect( overlay, rect, mWhite_color);
    //draw a frame around the area we require the pupil center to be.
    rect = cv::Rect(padding, padding, pupil_roi.width-padding, pupil_roi.height-padding);
    cvx::draw_dotted_rect( overlay, rect, mWhite_color);

    //draw size ellipses
    cv::Point center(100, image_height -100);
    cv::circle( color_image, center, props.pupil_size_min/2.0, mRed_color );
    // real pupil size of this frame is calculated further down, so this size is from the last frame
    cv::circle( color_image, center, mPupil_Size/2.0, mGreen_color );
    cv::circle( color_image, center, props.pupil_size_max/2.0, mRed_color );

    auto text_string = std::to_string(mPupil_Size);
    cv::Size text_size = cv::getTextSize( text_string, cv::FONT_HERSHEY_SIMPLEX, 0.4 , 1, 0);
    cv::Point text_pos = { center.x - text_size.width/2 , center.y + text_size.height/2};
    cv::putText( color_image, text_string, text_pos, cv::FONT_HERSHEY_SIMPLEX, 0.4, mRoyalBlue_color );


  }

  //get raw edge pix for later
  std::vector<cv::Point> raw_edges;
  cv::findNonZero(edges, raw_edges);

  ///////////////////////////////
  /// Strong Prior Part Begin ///
  ///////////////////////////////

  //if we had a good ellipse before ,let see if it is still a good first guess:
  // if( mUse_strong_prior ){

  //   mUse_strong_prior = false;

  //   //recalculate center in coords system of ROI views
  //   Ellipse ellipse = mPrior_ellipse;
  //   ellipse.center[0] -= ( pupil_roi.x + usr_roi.x  );
  //   ellipse.center[1] -= ( pupil_roi.y + usr_roi.y  );

  //   if( !raw_edges.empty() ){

  //     std::vector<cv::Point> support_pixels;
  //     double ellipse_circumference = ellipse.circumference();
  //     support_pixels = ellipse_true_support(ellipse, ellipse_circumference, raw_edges);

  //     double support_ration = support_pixels.size() / ellipse_circumference;

  //     if(support_ration >= props.strong_perimeter_ratio_range_min){

  //       cv::RotatedRect refit_ellipse = cv::fitEllipse(support_pixels);

  //       if(use_debug_image){
  //           cv::ellipse(debug_image, toRotatedRect(ellipse), mRoyalBlue_color, 4);
  //           cv::ellipse(debug_image, refit_ellipse, mRed_color, 1);
  //       }

  //       ellipse = toEllipse<double>(refit_ellipse);
  //       ellipse.center[0] += ( pupil_roi.x + usr_roi.x  );
  //       ellipse.center[1] += ( pupil_roi.y + usr_roi.y  );
  //       mPrior_ellipse = ellipse;

  //       double goodness = std::min(0.1, support_ration);

  //       result.confidence = goodness;
  //       result.ellipse = ellipse;

  //       mPupil_Size = ellipse.major_radius;

  //       return result;

  //     }
  //   }
  // }

  ///////////////////////////////
  ///  Strong Prior Part End  ///
  ///////////////////////////////

  //from edges to contours
  Contours_2D contours ;
  cv::findContours(edges, contours, CV_RETR_LIST, CV_CHAIN_APPROX_NONE );

  //first we want to filter out the bad stuff, to short ones
  const auto min_contour_size_pred = [&props]( const Contour_2D& contour){
    return contour.size() > props.contour_size_min;
  };
  contours = singleeyefitter::fun::filter( min_contour_size_pred , contours);

  //now we learn things about each contour through looking at the curvature.
  //For this we need to simplyfy the contour so that pt to pt angles become more meaningfull

  Contours_2D approx_contours;
  std::for_each(contours.begin(), contours.end(), [&](const Contour_2D& contour){
    std::vector<cv::Point> approx_c;
    cv::approxPolyDP( contour, approx_c, 1.5, false);
    approx_contours.push_back(std::move(approx_c));
  });

  // split contours looking at curvature and angle
  Scalar split_angle = 80;
  int min_contour_size = 4;  //removing stubs makes combinatorial search feasable
  //split_contours = singleeyefitter::detector::split_contours(approx_contours, split_angle );
  Contours_2D split_contours = singleeyefitter::detector::split_contours_optimized(approx_contours, split_angle , min_contour_size );

  if( split_contours.empty()){
    result.confidence = 0.0;
    return result;
  }
  //removing stubs makes combinatorial search feasable
  //  MOVED TO split_contours_optimized
  //split_contours = singleeyefitter::fun::filter( [](std::vector<cv::Point>& v){ return v.size() <= 3;} , split_contours);

  if(use_debug_image ){
    // debug segments
    int colorIndex = 0;
    for(auto& segment : split_contours){
      const cv::Scalar_<int> colors[] = {mRed_color, mBlue_color, mRoyalBlue_color, mYellow_color, mWhite_color, mGreen_color};
      cv::polylines(debug_image, segment, false, colors[colorIndex], 1, 4);
      colorIndex++;
      colorIndex %= 6;
    }
  }

  std::sort(split_contours.begin(), split_contours.end(), [](Contour_2D& a, Contour_2D& b){ return a.size() > b.size(); });

  //finding poential candidates for ellipse seeds that describe the pupil.
  std::vector<int> strong_seed_contours;
  std::vector<int> weak_seed_contours;


  const cv::Rect ellipse_center_varianz = cv::Rect(padding,padding, pupil_image.size().width - 2.0 * padding, pupil_image.size().height - 2.0 *padding);
  const EllipseEvaluation is_Ellipse( ellipse_center_varianz,props.ellipse_roundness_ratio,props.pupil_size_min, props.pupil_size_max  );

  for(int i=0; i < split_contours.size(); i++){

    auto contour = split_contours.at(i);

    if( contour.size() >= 5 ){ // because fitEllipse needs at least 5 points

      cv::RotatedRect ellipse = cv::fitEllipse(contour);
      //is this ellipse a plausible candidate for a pupil?
      if( is_Ellipse(ellipse ) ){
        auto e = toEllipse<Scalar>(ellipse);
        Scalar point_distances = 0.0;
        EllipseDistCalculator<Scalar> ellipseDistance(e);

        //std::cout << "Ellipse: "  << ellipse.center  << " " << ellipse.size << " "<< ellipse.angle << std::endl;
        //std::cout << "Points: ";
        for(int j=0; j < contour.size(); j++){
            auto& point = contour.at(j);
            //std::cout << point << ", ";
            point_distances += std::pow( std::abs( ellipseDistance( (Scalar)point.x, (Scalar)point.y )), 2 );
           // std::cout << "d=" << distance << ", " <<std::endl;
        }
       // std::cout << std::endl;
        Scalar fit_variance = point_distances / contour.size();
        //std::cout  << "contour index " << i <<std::endl;
        //std::cout  << "fit var1: " << fit_variance <<std::endl;
        if( fit_variance < props.initial_ellipse_fit_treshhold ){
          // how much ellipse is supported by this contour?

          auto ellipse_contour_support_ratio = []( Ellipse& ellipse, std::vector<cv::Point>& contour ){

                Scalar ellipse_circumference = ellipse.circumference();
                Scalar ellipse_area = ellipse.area();
                std::vector<cv::Point> hull;
                cv::convexHull(contour, hull);
                Scalar actual_area = cv::contourArea(hull);
                Scalar actual_length  = cv::arcLength(contour, false);
                Scalar area_ratio = actual_area / ellipse_area;
                Scalar perimeter_ratio = actual_length / ellipse_circumference; //we assume here that the contour lies close to the ellipse boundary
                return std::pair<Scalar,Scalar>(area_ratio,perimeter_ratio);
          };

          auto ratio = ellipse_contour_support_ratio(e, contour);
          Scalar area_ratio = ratio.first;
          Scalar perimeter_ratio = ratio.second;
          // same as in original
          //std::cout << area_ratio << ", " << perimeter_ratio << std::endl;
          if( props.strong_perimeter_ratio_range_min <= perimeter_ratio &&
              perimeter_ratio <= props.strong_perimeter_ratio_range_max &&
              props.strong_area_ratio_range_min <= area_ratio &&
              area_ratio <= props.strong_area_ratio_range_max ){
            //std::cout << "add seed: " << i << std::endl;
            strong_seed_contours.push_back(i);
            if(use_debug_image){
              cv::polylines( debug_image, contour, false, mRoyalBlue_color, 4);
              cv::ellipse( debug_image, ellipse, mBlue_color);
            }

          }else{
            weak_seed_contours.push_back(i);
            if(use_debug_image){
              cv::polylines( debug_image, contour, false, mBlue_color, 2);
              cv::ellipse( debug_image, ellipse, mBlue_color);
            }
          }
        }
      }
    }
  }

  std::vector<int>& seed_indices = strong_seed_contours;

  if( seed_indices.empty() && !weak_seed_contours.empty() ){
      seed_indices = weak_seed_contours;
  }

  // still empty ? --> exits
  if( seed_indices.empty() ){
      result.confidence = 0.0;
      std::cout << "EARLY EXIT!!!!!" << std::endl;
      return result;
  }


  auto ellipse_evaluation = [&]( std::vector<cv::Point>& contour) -> bool {

      auto ellipse = cv::fitEllipse(contour);
      Scalar point_distances = 0.0;
      EllipseDistCalculator<Scalar> ellipseDistance( toEllipse<Scalar>(ellipse) );
      for(int i=0; i < contour.size(); i++){
          auto point = contour.at(i);
          point_distances += std::pow(std::abs( ellipseDistance( (Scalar)point.x, (Scalar)point.y )), 2);
      }
      Scalar fit_variance = point_distances / float(contour.size());
      //std::cout << "fit var2: " << fit_variance << std::endl;
      return fit_variance <= props.initial_ellipse_fit_treshhold;

  };

  auto pruning_quick_combine = [&]( std::vector<std::vector<cv::Point>>& contours,  std::set<int>& seed_indices, int max_evals = 1e20, int max_depth = 5  ){

    typedef std::set<int> Path;

    std::vector<Path> unknown(seed_indices.size());
      // init with paths of size 1 == seed indices
    int n = 0;
    std::generate( unknown.begin(), unknown.end(), [&](){ return Path{n++}; }); // fill with increasing values, starting from 0

    std::vector<int> mapping; // contains all indices, starting with seed_indices
    mapping.reserve(contours.size());
    mapping.insert(mapping.begin(), seed_indices.begin(), seed_indices.end());
   // std::cout << "mapping size: " << mapping.size() << std::endl;

    // add indices which are not used to the end of mapping
    for( int i=0; i < contours.size(); i++){
      if( seed_indices.find(i) == seed_indices.end() ){ mapping.push_back(i); }
    }

    // std::cout << "mapping[";
    // for(int index : mapping ){ std::cout  << index << ", ";};
    // std::cout << "]";
    // std::cout << std::endl;

    // contains all the indices for the contours, which altogther fit best
    std::vector<Path> results;
    // contains bad paths
    std::vector<Path> prune;

    int eval_count = 0;
    while( !unknown.empty() && eval_count <= max_evals ){

       //  for(auto& u : unknown){
       //    std::cout << "unknown[";
       //    for(int index : u ){ std::cout  << index << ", ";};
       //     std::cout << "]";
       //  }
       // std::cout << std::endl;

      eval_count++;
      //take a path and combine it with others to see if the fit gets better
      Path current_path = unknown.back();
      unknown.pop_back();
      if( current_path.size() <= max_depth ){

          bool includes_bad_paths = false;
          for( Path& bad_path: prune){
            // check if bad_path is a subset of current_path
            // for std::include both containers need to be ordered. std::set guarantees this
            includes_bad_paths |= std::includes(current_path.begin(), current_path.end(), bad_path.begin(), bad_path.end());
          }

          if( !includes_bad_paths ){
              int size = 0;
              for( int j : current_path ){ size += contours.at(mapping.at(j)).size(); };
              std::vector<cv::Point> test_contour;
              test_contour.reserve(size);
              //std::cout << "reserve contour size:2 " << size << std::endl;
              std::set<int> test_contour_indices;
              //concatenate contours to one contour
              for( int k : current_path ){
                std::vector<cv::Point>& c = contours.at(mapping.at(k));
              //std::cout << "singe contour size:2 " << c.size() << std::endl;

               test_contour.insert( test_contour.end(), c.begin(), c.end() );
               test_contour_indices.insert(mapping.at(k));
               //std::cout << "test_contour_indices[";

              }

             // std::cout << "evaluate ellipse " << std::endl;
              //std::cout << "amount contours: " << current_path.size() << std::endl;
              //std::cout << "contour size:2 " << test_contour.size() << std::endl;
              //we have not tested this and a subset of this was sucessfull before
              if( ellipse_evaluation( test_contour ) ){

                //yes this was good, keep as solution
                results.push_back( test_contour_indices );
                //std::cout << "add result" << std::endl;
                //lets explore more by creating paths to each remaining node
                for(int l= (*current_path.rbegin())+1 ; l < mapping.size(); l++  ){
                    unknown.push_back( current_path );
                    unknown.back().insert(l); // add a new path
                    //std::cout << "push new" << std::endl;

                }

              }else{
                prune.push_back( current_path);
              }
          }
      }
    }
    return results;
  };

  std::set<int> seed_indices_set = std::set<int>(seed_indices.begin(),seed_indices.end());

  //for(int index : seed_indices ){ std::cout << "seed index: " << index << std::endl;};
  //std::cout << "split size: " << split_contours.size() << std::endl;
  std::vector<std::set<int>> solutions = pruning_quick_combine( split_contours, seed_indices_set, 1000, 5);
  //std::cout << "solutions: " << solutions.size() << std::endl;

  //find largest sets which contains all previous ones
  auto filter_subset = [](std::vector<std::set<int>>& sets){
    std::vector<std::set<int>> filtered_set;
    int i = 0;
    for(auto& current_set : sets){

        //check if this current_set is a subset of set
        bool isSubset = false;
        for( int j = 0; j < sets.size(); j++){
          if(j == i ) continue;// don't compare to itself
          auto& set = sets.at(j);
          // for std::include both containers need to be ordered. std::set guarantees this
          isSubset |= std::includes(set.begin(), set.end(), current_set.begin(), current_set.end());

        }

        if(!isSubset){
          //std::cout << "is not subset " << std::endl;
           filtered_set.push_back(current_set);
        }
        i++;
    }
    return filtered_set;
  };
  //std::cout << "solutions before: " << solutions.size() << std::endl;

  // for(auto& s : solutions){
  //   std::cout << "[";
  //   for(int index : s ){ std::cout  << index << ", ";};
  //   std::cout << "]";

  // }
  // std::cout << std::endl;

  solutions = filter_subset(solutions);
//  std::cout << "solutions after: " << solutions.size() << std::endl;


//---------------------------------------------------////

  int index_best_Solution = -1;
  int enum_index = 0;
  for( auto& s : solutions){

    std::vector<cv::Point> test_contour;
    //concatenate contours to one contour
    for( int i : s ){
     std::vector<cv::Point>& c = split_contours.at(i);
     test_contour.insert( test_contour.end(), c.begin(), c.end() );
    }
    auto cv_ellipse = cv::fitEllipse( test_contour );

    if(use_debug_image ){
        cv::ellipse(debug_image, cv_ellipse , mRed_color);
    }
    Ellipse ellipse = toEllipse<Scalar>(cv_ellipse);
    Scalar ellipse_circumference = ellipse.circumference();
    std::vector<cv::Point>  support_pixels = ellipse_true_support(ellipse, ellipse_circumference, raw_edges);
    Scalar support_ratio = support_pixels.size() / ellipse_circumference;
    //TODO: refine the selection of final candidate

    if( support_ratio >= props.final_perimeter_ratio_range_min && is_Ellipse(cv_ellipse) ){

      index_best_Solution = enum_index;
        if( support_ratio >= props.strong_perimeter_ratio_range_min ){
            ellipse.center[0] += ( pupil_roi.x + usr_roi.x  );
            ellipse.center[1] += ( pupil_roi.y + usr_roi.y  );
            mPrior_ellipse = ellipse;

            if(use_debug_image){
              cv::ellipse(debug_image, cv_ellipse , mGreen_color);
            }
        }
    }
    enum_index++;

  }

  // select ellipse

  if( index_best_Solution == -1 ){
      // no good final ellipse found
      result.confidence = 0.0;
      //history
      return result;
  }

  auto& best_solution = solutions.at( index_best_Solution );

  std::vector<std::vector<cv::Point>> best_contours;
  std::vector<cv::Point>best_contour;
    //concatenate contours to one contour
  for( int i : best_solution ){
   std::vector<cv::Point>& c = split_contours.at(i);
   best_contours.push_back( c );
   best_contour.insert( best_contour.end(), c.begin(), c.end() );
  }
  auto cv_ellipse = cv::fitEllipse(best_contour );

  // final calculation of goodness of fit
  auto ellipse = toEllipse<Scalar>(cv_ellipse);
  Scalar ellipse_circumference = ellipse.circumference();
  std::vector<cv::Point>  support_pixels = ellipse_true_support(ellipse, ellipse_circumference, raw_edges);
  Scalar support_ratio = support_pixels.size() / ellipse_circumference;

  Scalar goodness = std::min( Scalar(1.0), support_ratio);

  //final fitting and return of result

  auto final_fitting = [&](std::vector<std::vector<cv::Point>>& contours, cv::Mat& edges ) -> std::vector<cv::Point> {
    //use the real edge pixels to fit, not the aproximated contours
    cv::Mat support_mask(edges.rows, edges.cols, edges.type(), {0,0,0});
    cv::polylines( support_mask, contours, false, {255,255,255}, 2 );

    //draw into the suport mask with thickness 2
    cv::Mat new_edges;
    std::vector<cv::Point> new_contours;
    cv::min(edges, support_mask, new_edges);
    cv::findNonZero( new_edges, new_contours);

      if(use_debug_image){
          cv::Mat overlay = color_image.colRange(usr_roi.x + pupil_roi.x, usr_roi.x + pupil_roi.x + pupil_roi.width).rowRange(usr_roi.y + pupil_roi.y, usr_roi.y + pupil_roi.y + pupil_roi.height);
          cv::Mat g_channel( overlay.rows, overlay.cols, CV_8UC1 );
          cv::Mat b_channel( overlay.rows, overlay.cols, CV_8UC1 );
          cv::Mat r_channel( overlay.rows, overlay.cols, CV_8UC1 );
          cv::Mat out[] = {b_channel, g_channel,r_channel};
          cv:split(overlay, out);

          cv::threshold(new_edges, new_edges, 0, 255,cv::THRESH_BINARY);
          cv::max(r_channel, new_edges,r_channel);
          cv::merge(out, 3, overlay);
      }

      return new_contours;

  };


  std::vector<cv::Point> final_edges =  final_fitting(best_contours, edges );
  auto cv_final_Ellipse = cv::fitEllipse( final_edges);

  Scalar size_difference  = std::abs( 1.0 - cv_ellipse.size.height / cv_final_Ellipse.size.height );

  if(  is_Ellipse( cv_final_Ellipse ) && size_difference < 0.3 ){

      if( use_debug_image ){
          cv::ellipse( debug_image, cv_final_Ellipse, mGreen_color );
      }
  }

  //cv::imshow("debug_image", debug_image);

  mPupil_Size =  cv_final_Ellipse.size.height;
  result.confidence = goodness;
  result.ellipse = toEllipse<Scalar>(cv_final_Ellipse);

  return result;

}

