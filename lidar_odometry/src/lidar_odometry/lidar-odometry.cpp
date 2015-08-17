#include "lidar-odometry.hpp"

LidarOdom::LidarOdom(boost::shared_ptr<lcm::LCM> &lcm_):
  lcm_(lcm_)
{

  laserType_ = FRSM_HOKUYO_UTM;
  //parameters for a hokuyo with the helicopters mirror's attached
  validBeamAngles_[0] = -2.1;
  validBeamAngles_[1] = 2.1;
  beamSkip_ = 3;
  spatialDecimationThresh_ = .2;
  maxRange_ = 29.7;


  publishRelative_ = FALSE;
  publishPose_ = TRUE;
  doDrawing_ = TRUE;
  lastDrawTime_ =0;

  //hardcoded scan matcher params
  double metersPerPixel = .02; //translational resolution for the brute force search
  double thetaResolution = .01; //angular step size for the brute force search
  frsm_incremental_matching_modes_t  matchingMode= FRSM_COORD_ONLY; //use gradient descent to improve estimate after brute force search
  int useMultires = 3; // low resolution will have resolution metersPerPixel * 2^useMultiRes

  double initialSearchRangeXY = .15; //nominal range that will be searched over
  double initialSearchRangeTheta = .1;

  //SHOULD be set greater than the initialSearchRange
  double maxSearchRangeXY = .3; //if a good match isn't found I'll expand and try again up to this size...
  double maxSearchRangeTheta = .2; //if a good match isn't found I'll expand and try again up to this size...

  int maxNumScans = 30; //keep around this many scans in the history
  double addScanHitThresh = .80; //add a new scan to the map when the number of "hits" drops below this

  bool stationaryMotionModel = false; //use constant velocity model
  double motionModelPriorWeight =0; //don't use the prior for anything other than centering the window

  int useThreads = 1;

  //create the actual scan matcher object
  sm_ = new ScanMatcher(metersPerPixel, thetaResolution, useMultires,
            useThreads,true);

  if (sm_->isUsingIPP())
      fprintf(stderr, "Using IPP\n");
  else
      fprintf(stderr, "NOT using IPP\n");


  ScanTransform startPose;
  memset(&startPose, 0, sizeof(startPose));
  startPose.theta = 0; //set the scan matcher to start at 0 heading... cuz pi/2 would be rediculous
  sm_->initSuccessiveMatchingParams(maxNumScans, initialSearchRangeXY,
            maxSearchRangeXY, initialSearchRangeTheta, maxSearchRangeTheta,
            matchingMode, addScanHitThresh,
            stationaryMotionModel,motionModelPriorWeight,&startPose);

  if (doDrawing_) {
    lcmgl_state_ = bot_lcmgl_init(lcm_->getUnderlyingLCM(), "FRSM_state");
    lcmgl_scan_ = bot_lcmgl_init(lcm_->getUnderlyingLCM(), "FRSM_scan");
  }
  else {
    lcmgl_state_ = NULL;
    lcmgl_scan_ = NULL;
  }


  cout <<"LidarOdom Constructed\n";

}


LidarOdom::~LidarOdom()
{
}

void LidarOdom::draw(frsmPoint * points, unsigned numPoints, const ScanTransform * T)
{
  sm_->draw_state_lcmgl(lcmgl_state_);
  sm_->draw_scan_lcmgl(lcmgl_scan_, points, numPoints, T);
  bot_lcmgl_switch_buffer(lcmgl_scan_);
  bot_lcmgl_switch_buffer(lcmgl_state_);
}



void sm_roll_pitch_yaw_to_quat (const double rpy[3], double q[4])
{
    double roll = rpy[0], pitch = rpy[1], yaw = rpy[2];

    double halfroll = roll / 2;
    double halfpitch = pitch / 2;
    double halfyaw = yaw / 2;

    double sin_r2 = sin (halfroll);
    double sin_p2 = sin (halfpitch);
    double sin_y2 = sin (halfyaw);

    double cos_r2 = cos (halfroll);
    double cos_p2 = cos (halfpitch);
    double cos_y2 = cos (halfyaw);

    q[0] = cos_r2 * cos_p2 * cos_y2 + sin_r2 * sin_p2 * sin_y2;
    q[1] = sin_r2 * cos_p2 * cos_y2 - cos_r2 * sin_p2 * sin_y2;
    q[2] = cos_r2 * sin_p2 * cos_y2 + sin_r2 * cos_p2 * sin_y2;
    q[3] = cos_r2 * cos_p2 * sin_y2 - sin_r2 * sin_p2 * cos_y2;
}


Eigen::Isometry3d getScanTransformAsIsometry3d(ScanTransform tf){
  Eigen::Isometry3d tf_out;
  tf_out.setIdentity();
  tf_out.translation()  << tf.x, tf.y, 0;

  double rpy[3] = { 0, 0, tf.theta };
  double quat[4];
  sm_roll_pitch_yaw_to_quat(rpy, quat);
  Eigen::Quaterniond q(quat[0], quat[1],quat[2],quat[3]);
  tf_out.rotate(q);

  return tf_out;
}


void LidarOdom::doOdometry(float* ranges, int nranges, float rad0, float radstep, int64_t utime){

    //Project ranges into points, and decimate points so we don't have too many
    frsmPoint * points = (frsmPoint *) calloc(nranges, sizeof(frsmPoint));
    int numValidPoints = frsm_projectRangesAndDecimate(beamSkip_,
            spatialDecimationThresh_, ranges, nranges, rad0,
            radstep, points, maxRange_, validBeamAngles_[0],
            validBeamAngles_[1]);
    if (numValidPoints < 30) {
        fprintf(stderr, "WARNING! NOT ENOUGH VALID POINTS! numValid=%d\n",
                numValidPoints);
        free(points);
        return;
    }

    //Actually do the matching
    ScanTransform r = sm_->matchSuccessive(points, numValidPoints,
            laserType_, utime, NULL); //don't have a better estimate than prev, so just set prior to NULL
                                      //utime is ONLY used to tag the scans that get added to the map, doesn't actually matter
    Eigen::Isometry3d r_Iso = getScanTransformAsIsometry3d(r);
    prevOdom_ = currOdom_;
    currOdom_ = r_Iso;
    prevUtime_ = currUtime_;
    currUtime_ = utime;

    //Print current position periodically
    static double lastPrintTime = 0;
    if (frsm_get_time() - lastPrintTime > 2.0) {
        lastPrintTime = frsm_get_time();
        fprintf(stderr,
                "x=%+7.3f y=%+7.3f t=%+7.3f\t score=%f hits=%.2f sx=%.2f sxy=%.2f sy=%.2f st=%.2f, numValid=%d\n",
                r.x, r.y, r.theta, r.score, (double) r.hits / (double) numValidPoints, r.sigma[0], r.sigma[1],
                r.sigma[4], r.sigma[8], numValidPoints);
    }

    //Do drawing periodically
    // This was disabled when moving to the frsm library. it would be easily re-added on a fork
    //Do drawing periodically!
    if (doDrawing_ && frsm_get_time() - lastDrawTime_ > .2) {
        lastDrawTime_ = frsm_get_time();
        //frsm_tictoc("drawing");
        draw(points, numValidPoints, &r);
        //frsm_tictoc("drawing");
    }

    //cleanup
    free(points);
}


/////// CODE BELOW IS TO ACCEPT A SET OF X,Y POINTS AND TREAT THEM AS LIDAR RETURNS
/////// ORIGINALLY USED FOR HORIZONTAL DATA COMING FROM A VELODYNE
int projectPointsToPoints(std::vector<float> x, std::vector<float> y, int numPoints,
    frsmPoint * points, double maxRange = 1e10, 
    double * aveValidRange = NULL, double * stddevValidRange = NULL)
{
  int count = 0;
  double aveRange = 0;
  double aveRangeSq = 0;

  for (int i = 0; i < numPoints; i++) {
    double range = sqrt( x[i]*x[i] + y[i]*y[i] );
    if (range > .1 && range < maxRange ) {
      points[count].x = x[i];
      points[count].y = y[i];
      count++;
      aveRange += range;
      aveRangeSq += frsm_sq(range);
    }
  }

  aveRange /= (double) count;
  aveRangeSq /= (double) count;

  if (aveValidRange != NULL)
    *aveValidRange = aveRange;
  if (stddevValidRange != NULL)
    *stddevValidRange = sqrt(aveRangeSq - frsm_sq(aveRange));

  return count;
}


int projectPointsAndDecimate(int beamskip, float spatialDecimationThresh, std::vector<float> x, std::vector<float> y, int npoints,
    frsmPoint * points, double maxRange = 1e10)
{
  int lastAdd = -1000;
  double aveRange;
  double stdDevRange;

  int numValidPoints = projectPointsToPoints(x, y, npoints,
    points, maxRange, &aveRange, &stdDevRange);

  frsmPoint origin = { 0, 0 };
  frsmPoint lastAddPoint = { 0, 0 };
  int numDecPoints = 0;
  for (int i = 0; i < numValidPoints; i++) {
    //add every beamSkip beam, unless points are more than spatialDecimationThresh, or more than 1.8 stdDevs more than ave range
    if ((i - lastAdd) > beamskip || frsm_dist(&points[i], &lastAddPoint) > spatialDecimationThresh || frsm_dist(&points[i],
        &origin) > (aveRange + 1.8 * stdDevRange)) {
      lastAdd = i;
      lastAddPoint = points[i];
      points[numDecPoints] = points[i]; // ok since i >= numDecPoints
      numDecPoints++;
    }
  }
  return numDecPoints;

}


void LidarOdom::doOdometry(std::vector<float> x, std::vector<float> y, int npoints, int64_t utime){

    double maxRange = 39.0;
    int beamSkip = 3;

    //Project ranges into points, and decimate points so we don't have too many
    frsmPoint * points = (frsmPoint *) calloc(npoints, sizeof(frsmPoint));
    int numValidPoints = projectPointsAndDecimate(beamSkip,
            spatialDecimationThresh_, x, y, npoints, points, maxRange);
    if (numValidPoints < 30) {
        fprintf(stderr, "WARNING! NOT ENOUGH VALID POINTS! numValid=%d\n",
                numValidPoints);
        free(points);
        return;
    }

    //Actually do the matching
    ScanTransform r = sm_->matchSuccessive(points, numValidPoints,
            laserType_, utime, NULL); //don't have a better estimate than prev, so just set prior to NULL
                                      //utime is ONLY used to tag the scans that get added to the map, doesn't actually matter
    Eigen::Isometry3d r_Iso = getScanTransformAsIsometry3d(r);
    prevOdom_ = currOdom_;
    currOdom_ = r_Iso;
    prevUtime_ = currUtime_;
    currUtime_ = utime;

    //Print current position periodically
    static double lastPrintTime = 0;
    if (frsm_get_time() - lastPrintTime > 2.0) {
        lastPrintTime = frsm_get_time();
        fprintf(stderr,
                "x=%+7.3f y=%+7.3f t=%+7.3f\t score=%f hits=%.2f sx=%.2f sxy=%.2f sy=%.2f st=%.2f, numValid=%d\n",
                r.x, r.y, r.theta, r.score, (double) r.hits / (double) numValidPoints, r.sigma[0], r.sigma[1],
                r.sigma[4], r.sigma[8], numValidPoints);
    }

    //Do drawing periodically
    // This was disabled when moving to the frsm library. it would be easily re-added on a fork
    //Do drawing periodically!
    if (doDrawing_ && frsm_get_time() - lastDrawTime_ > .2) {
        lastDrawTime_ = frsm_get_time();
        //frsm_tictoc("drawing");
        draw(points, numValidPoints, &r);
        //frsm_tictoc("drawing");
    }

    //cleanup
    free(points);
}