/**
* Copyright (C) 2017 Chalmers Revere
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
* USA.
*/

#include <iostream>

#include "slam.hpp"
#include "WGS84toCartesian.hpp"

Slam::Slam(std::map<std::string, std::string> commandlineArguments,cluon::OD4Session &a_od4) :
  od4(a_od4)
, m_optimizer()
, m_lastTimeStamp()
, m_coneCollector()
, m_lastObjectId()
, m_coneMutex()
, m_sensorMutex()
, m_mapMutex()
, m_optimizerMutex()
, m_yawMutex()
, m_groundSpeedMutex()
, m_stateMachineMutex()
, m_odometryData()
, m_gpsReference()
, m_map()
, m_keyframeTimeStamp(cluon::time::now())
, m_newFrame()
, m_sendPose()
, m_sendMutex()
{
  setupOptimizer();
  setUp(commandlineArguments);
  m_coneCollector = Eigen::MatrixXd::Zero(4,100);
  m_lastObjectId = 0;
  m_odometryData << 0,0,0;
  m_sendPose << 0,0,0;
  m_newFrame = true;
}

void Slam::setupOptimizer(){

  typedef g2o::BlockSolver<g2o::BlockSolverTraits<-1, -1> > slamBlockSolver;
  typedef g2o::LinearSolverEigen<slamBlockSolver::PoseMatrixType> slamLinearSolver;
  
  auto linearSolver = g2o::make_unique<slamLinearSolver>();
  linearSolver->setBlockOrdering(false);
  
  g2o::OptimizationAlgorithmGaussNewton* algorithmType = new g2o::OptimizationAlgorithmGaussNewton(g2o::make_unique<slamBlockSolver>(std::move(linearSolver)));
  m_optimizer.setAlgorithm(algorithmType); //Set optimizing method to Gauss Newton
  m_optimizer.setVerbose(true);
}

void Slam::nextSplitPose(cluon::data::Envelope data){
  std::lock_guard<std::mutex> lockSensor(m_sensorMutex);
  if(data.dataType() == opendlv::proxy::GeodeticWgs84Reading::ID()){
    auto position = cluon::extractMessage<opendlv::proxy::GeodeticWgs84Reading>(std::move(data));

    double longitude = position.longitude();
    double latitude = position.latitude();

    //toCartesian(const std::array<double, 2> &WGS84Reference, const std::array<double, 2> &WGS84Position)

    std::array<double,2> WGS84ReadingTemp;

    WGS84ReadingTemp[0] = latitude;
    WGS84ReadingTemp[1] = longitude;

    std::array<double,2> WGS84Reading = wgs84::toCartesian(m_gpsReference, WGS84ReadingTemp); 
    //opendlv::data::environment::WGS84Coordinate gpsCurrent = opendlv::data::environment::WGS84Coordinate(latitude, longitude);
    //opendlv::data::environment::Point3 gpsTransform = m_gpsReference.transform(gpsCurrent);

    m_odometryData(0) =  WGS84Reading[0];
    m_odometryData(1) =  WGS84Reading[1];
  }
  else if(data.dataType() == opendlv::proxy::GeodeticHeadingReading::ID()){
    auto message = cluon::extractMessage<opendlv::proxy::GeodeticHeadingReading>(std::move(data));
    double heading = message.northHeading();
    heading = heading-PI;
    heading = (heading > PI)?(heading-2*PI):(heading);
    heading = (heading < -PI)?(heading+2*PI):(heading);
    m_odometryData(2) = heading;
    //std::cout << "head: " << heading << std::endl;
  }
}

void Slam::nextPose(cluon::data::Envelope data){
    //#########################Recieve Odometry##################################
  
  std::lock_guard<std::mutex> lockSensor(m_sensorMutex);
  m_geolocationReceivedTime = data.sampleTimeStamp();
  auto odometry = cluon::extractMessage<opendlv::logic::sensation::Geolocation>(std::move(data));

  double longitude = odometry.longitude();
  double latitude = odometry.latitude();

  //toCartesian(const std::array<double, 2> &WGS84Reference, const std::array<double, 2> &WGS84Position)
  if(m_gpsCoords){

    std::array<double,2> WGS84ReadingTemp;

    WGS84ReadingTemp[0] = latitude;
    WGS84ReadingTemp[1] = longitude;

    std::array<double,2> WGS84Reading = wgs84::toCartesian(m_gpsReference, WGS84ReadingTemp); 
    //opendlv::data::environment::WGS84Coordinate gpsCurrent = opendlv::data::environment::WGS84Coordinate(latitude, longitude);
    //opendlv::data::environment::Point3 gpsTransform = m_gpsReference.transform(gpsCurrent);

    m_odometryData << WGS84Reading[0],
                      WGS84Reading[1],
                      odometry.heading();
  }
  else{
    m_odometryData << longitude,
                      latitude,
                      odometry.heading();
  }  //std::cout << "head: " << odometry.heading() << std::endl;                   
}

void Slam::nextYawRate(cluon::data::Envelope data){

  std::lock_guard<std::mutex> lockYaw(m_yawMutex);
  auto yawRate = cluon::extractMessage<opendlv::proxy::AngularVelocityReading>(std::move(data));
  m_yawRate = yawRate.angularVelocityZ();
   m_yawReceivedTime = data.sampleTimeStamp();
   //std::cout << "Yaw in message: " << m_yawRate << std::endl;
}
void Slam::nextGroundSpeed(cluon::data::Envelope data){

  std::lock_guard<std::mutex> lockGroundSpeed(m_groundSpeedMutex);
  auto groundSpeed = cluon::extractMessage<opendlv::proxy::GroundSpeedReading>(std::move(data));
  m_groundSpeed = groundSpeed.groundSpeed();
   m_groundSpeedReceivedTime = data.sampleTimeStamp();
   //std::cout << "Yaw in message: " << m_yawRate << std::endl;
}

void Slam::recieveCombinedMessage(cluon::data::TimeStamp currentFrameTime,std::map<int,ConePackage> currentFrame){
  m_lastTimeStamp = currentFrameTime;
  if(isKeyframe()){
    Eigen::MatrixXd cones = Eigen::MatrixXd::Zero(4,currentFrame.size());
    std::map<int,ConePackage>::iterator it;
    int coneIndex = 0;
    it =currentFrame.begin();
    while(it != currentFrame.end()){
      auto direction = std::get<0>(it->second);
      auto distance = std::get<1>(it->second);
      auto type = std::get<2>(it->second);
      cones(0,coneIndex) = direction.azimuthAngle();
      cones(1,coneIndex) = direction.zenithAngle();
      cones(2,coneIndex) = distance.distance();
      cones(3,coneIndex) = type.type();
      coneIndex++;
      it++;
    }
    performSLAM(cones);
  }
}

bool Slam::isKeyframe(){
  cluon::data::TimeStamp startTime = cluon::time::now();
  double timeElapsed = fabs(static_cast<double>(cluon::time::deltaInMicroseconds(m_keyframeTimeStamp,startTime)))/1000;
  std::cout << "Time ellapsed is: " << timeElapsed << std::endl;
  if(timeElapsed>m_timeBetweenKeyframes){//Keyframe candidate is based on time difference from last keyframe
    m_keyframeTimeStamp = startTime;
    return true;
  }
  return false;
}


void Slam::performSLAM(Eigen::MatrixXd cones){
  
  std::lock_guard<std::mutex> lockStateMachine(m_stateMachineMutex);
  if(!m_readyStateMachine || !m_readyState)
  {
    return;
  }
    Eigen::Vector3d pose;
  {
    std::lock_guard<std::mutex> lockSensor(m_sensorMutex);
    pose = m_odometryData;
    m_poses.push_back(pose);
  }
  if(!m_loopClosingComplete){
    {
      std::lock_guard<std::mutex> lockMap(m_mapMutex);
      createConnections(cones,pose);
    }

    uint32_t currentEndCone = m_coneList.size() - 1; 
    uint32_t coneDiff = currentEndCone - m_coneRef;

    if(coneDiff >= 10 && !m_loopClosingComplete){
      std::lock_guard<std::mutex> lockMap(m_mapMutex);
      optimizeEssentialGraph(currentEndCone-coneDiff, currentEndCone);
      m_coneRef = currentEndCone; 
    }
  }

    m_poseId++;

    //Check if there is enough loopclosing candidates
    if(!m_loopClosingComplete){
      if(m_currentConeDiff > m_lapSize){
        std::lock_guard<std::mutex> lockMap(m_mapMutex);
        fullBA();
        m_loopClosingComplete = true;
        m_filterMap = true;
      }
    }
    //Map preprocessing
    if(m_filterMap){
      std::lock_guard<std::mutex> lockMap(m_mapMutex);
      std::lock_guard<std::mutex> lockSensor(m_sensorMutex);
      filterMap();
      updateMap(0,m_coneList.size(),true);
      m_filterMap = false;
      m_currentConeIndex = 0;
    }

    //Localizer
    if(m_loopClosingComplete){
        localizer(cones, pose); //False or True for pose optimization
        sendPose();
        sendCones();
    }

}

int Slam::updateCurrentCone(Eigen::Vector3d pose,uint32_t currentConeIndex, uint32_t remainingIter){
  currentConeIndex=(currentConeIndex<m_map.size())?(currentConeIndex):(currentConeIndex-m_map.size());
  Cone currentCone = m_map[currentConeIndex];
  remainingIter = remainingIter-1;
  auto distance = currentCone.getDistance(pose);
  auto direction = currentCone.getDirection(pose);
  if(remainingIter == 0){
    return currentConeIndex-1;
  }
  if(distance.distance() < 10.0f && fabs(direction.azimuthAngle())>80.0f){
    currentConeIndex = updateCurrentCone(pose,currentConeIndex+1,remainingIter);
  }
  return currentConeIndex;
}

void Slam::localizer(Eigen::MatrixXd cones, Eigen::Vector3d pose){
  g2o::SparseOptimizer localGraph;
  typedef g2o::BlockSolver<g2o::BlockSolverTraits<-1, -1> > slamBlockSolver;
  typedef g2o::LinearSolverEigen<slamBlockSolver::PoseMatrixType> slamLinearSolver;
  
  auto linearSolver = g2o::make_unique<slamLinearSolver>();
  linearSolver->setBlockOrdering(false);
  
  g2o::OptimizationAlgorithmGaussNewton* algorithmType = new g2o::OptimizationAlgorithmGaussNewton(g2o::make_unique<slamBlockSolver>(std::move(linearSolver)));
  localGraph.setAlgorithm(algorithmType); //Set optimizing method to Gauss Newton
  localGraph.setVerbose(true);

  std::lock_guard<std::mutex> lockMap(m_mapMutex);

  //Optimize Heading

  double optimizedHeading = optimizeHeading(cones,pose);
  pose(2) = optimizedHeading;


  //Find match in conelist
  std::vector<int> matchedConeIndex;
  std::vector<Eigen::Vector3d> localObs;
  //double shortestDistance = 10000;
  for(uint32_t i = 0; i < cones.cols(); i++){
    bool foundMatch = false;
    uint32_t j = 0;
    while(!foundMatch && j < m_map.size()){
      Eigen::Vector3d globalCone = coneToGlobal(pose, cones.col(i));
      Cone globalConeObject = Cone(globalCone(0), globalCone(1),0,2000);
      double distance = distanceBetweenConesOpt(m_map[j],globalConeObject);

      if(distance < 1.5){ //m_newConeThreshold
        matchedConeIndex.push_back(j);

        Eigen::Vector3d localCone = Spherical2Cartesian(cones(0,i), cones(1,i),cones(2,i));
        localObs.push_back(localCone);
        foundMatch = true;
/*
        if(distance < shortestDistance){

          shortestDistance = distance;
          //m_currentConeIndex = m_map[j].getId();

        }
        */
      }
      j++;
    }
  }
  m_currentConeIndex = updateCurrentCone(pose,m_currentConeIndex,m_map.size());
  bool performedLocalization = false;
  if(matchedConeIndex.size() > 2 ){  
    //Create graph
    //Add pose vertex
    g2o::VertexSE2* poseVertex = new g2o::VertexSE2;
    poseVertex->setId(1000);
    poseVertex->setEstimate(pose);
    localGraph.addVertex(poseVertex);

    //Add cone vertex
    Eigen::Vector2d coneMeanXY;
    for(uint32_t i = 0; i < matchedConeIndex.size(); i++){

      g2o::EdgeSE2PointXY* coneMeasurement = new g2o::EdgeSE2PointXY;
      coneMeanXY << m_map[matchedConeIndex[i]].getOptX(),m_map[matchedConeIndex[i]].getOptY();
      g2o::VertexPointXY* coneVertex = new g2o::VertexPointXY;
      coneVertex->setId(i);
      coneVertex->setEstimate(coneMeanXY);
      coneVertex->setFixed(true);      
      localGraph.addVertex(coneVertex);

      //Add edge between pose and cone i
      Eigen::Vector2d xyMeasurement;
      coneMeasurement->vertices()[0] = localGraph.vertex(1000);
      coneMeasurement->vertices()[1] = localGraph.vertex(i);
      xyMeasurement << localObs[i](0),localObs[i](1);
      coneMeasurement->setMeasurement(xyMeasurement);

      Eigen::Matrix2d informationMatrix;
      informationMatrix << 1/0.1,0,
                              0,1/0.1;
      coneMeasurement->setInformation(informationMatrix);
      localGraph.addEdge(coneMeasurement);
      performedLocalization = true;
    }
  }
    if(performedLocalization){
      localGraph.initializeOptimization();
      localGraph.optimize(10); 
      g2o::VertexSE2* updatedPoseVertex = static_cast<g2o::VertexSE2*>(localGraph.vertex(1000));
      g2o::SE2 updatedPoseSE2 = updatedPoseVertex->estimate();
      Eigen::Vector3d updatedPose = updatedPoseSE2.toVector();

      std::lock_guard<std::mutex> lockSend(m_sendMutex);
      m_sendPose << updatedPose(0),updatedPose(1),updatedPose(2);
    }else{
      std::lock_guard<std::mutex> lockSend(m_sendMutex);
      m_sendPose << pose(0),pose(1),pose(2);
    }
  
  
}

void Slam::createConnections(Eigen::MatrixXd cones, Eigen::Vector3d pose){
  int currentConeIndex = m_currentConeIndex;
  //Check current cone list and find same cone, if true add pose connection and cone observation
  bool firstCone = false;
  if(m_coneList.size() == 0){
    Eigen::Vector3d localCone = Spherical2Cartesian(cones(0,0), cones(1,0),cones(2,0));
    Eigen::Vector3d globalCone = coneToGlobal(pose, cones.col(0));
    Cone cone = Cone(globalCone(0),globalCone(1),(int)globalCone(2),m_coneList.size()); //Temp id, think of system later
    cone.addObservation(localCone,globalCone,m_poseId,m_currentConeIndex);

    m_coneList.push_back(cone);
    firstCone = true;
  }

  double minDistance = 100;
  for(uint32_t i = 0; i<cones.cols(); i++){//Iterate through local cone objects
    double distanceToCar = cones(2,i);
    Eigen::Vector3d localCone = Spherical2Cartesian(cones(0,i), cones(1,i),cones(2,i));
    Eigen::Vector3d globalCone = coneToGlobal(pose, cones.col(i)); //Make local cone into global coordinate frame
    uint32_t j = 0;
    bool coneFound = false;
    while(!coneFound && j<m_coneList.size() && !m_loopClosing && !firstCone){
      if(fabs(m_coneList[j].getType() - cones(3,i))<0.0001 && !std::isnan(globalCone(0)) && !std::isnan(globalCone(1))){ //Check is same classification

        Cone globalConeObject = Cone(globalCone(0), globalCone(1),0,2000);
        double distance = distanceBetweenCones(m_coneList[j],globalConeObject);

        if(distance<m_newConeThreshold){ //NewConeThreshold is the accepted distance for a new cone candidate
          coneFound = true;
          m_coneList[j].addObservation(localCone, globalCone, m_poseId,m_currentConeIndex);
          
          if(distanceToCar<minDistance && distanceToCar<m_coneMappingThreshold){//Update current cone to know where in the map we are
            currentConeIndex = j;
            minDistance = distanceToCar;
          }
        }
      }
      j++;
    }
    if(distanceToCar < m_coneMappingThreshold && !coneFound && !m_loopClosing && !firstCone){
      //std::cout << "Trying to add cone" << std::endl;
      if(!std::isnan(globalCone(0)) && !std::isnan(globalCone(1))){
        Cone cone = Cone(globalCone(0),globalCone(1),(int)globalCone(2),m_coneList.size()); //Temp id, think of system later
        cone.addObservation(localCone, globalCone,m_poseId,m_currentConeIndex);
        m_coneList.push_back(cone);  
      }
    }
  }
  m_currentConeDiff = m_currentConeIndex - currentConeIndex;
  m_currentConeIndex = currentConeIndex;
}

void Slam::createFullGraph(){
  //Add all poses to graph
  addPosesToGraph();
  //For each cone in conelist add poses
  addConesToGraph();
}

void Slam::optimizeEssentialGraph(uint32_t graphIndexStart, uint32_t graphIndexEnd){

  //Initialize graph
  g2o::SparseOptimizer essentialGraph;
  typedef g2o::BlockSolver<g2o::BlockSolverTraits<-1, -1> > slamBlockSolver;
  typedef g2o::LinearSolverEigen<slamBlockSolver::PoseMatrixType> slamLinearSolver;
  
  auto linearSolver = g2o::make_unique<slamLinearSolver>();
  linearSolver->setBlockOrdering(false);
  
  g2o::OptimizationAlgorithmGaussNewton* algorithmType = new g2o::OptimizationAlgorithmGaussNewton(g2o::make_unique<slamBlockSolver>(std::move(linearSolver)));
  essentialGraph.setAlgorithm(algorithmType); //Set optimizing method to Gauss Newton
  essentialGraph.setVerbose(true);

  std::vector<int> posesToGraph;
  //Find cones of conespan and extract poses
  for(uint32_t i = graphIndexStart; i < graphIndexEnd+1; i++){

    std::vector<int> currentConnectedPoses = m_coneList[i].getConnectedPoses();

    for(uint32_t j = 0; j < currentConnectedPoses.size(); j++){

      posesToGraph.push_back(currentConnectedPoses[j]);
    }

  }
  if(posesToGraph.size() > 0){

    uint32_t max = *std::max_element(posesToGraph.begin(), posesToGraph.end());
    uint32_t min = *std::min_element(posesToGraph.begin(), posesToGraph.end());

    //add poses to graph based on min and max
    for(uint32_t k = min; k < max+1; k++){

      //Add vertex
      g2o::VertexSE2* poseVertex = new g2o::VertexSE2;
      poseVertex->setId(k);
      poseVertex->setEstimate(m_poses[k-1000]);

      essentialGraph.addVertex(poseVertex);

      //Add edge
      if(k > min){
        g2o::EdgeSE2* odometryEdge = new g2o::EdgeSE2;
        odometryEdge->vertices()[0] = essentialGraph.vertex(k-1);
        odometryEdge->vertices()[1] = essentialGraph.vertex(k);
        g2o::VertexSE2* prevVertex = static_cast<g2o::VertexSE2*>(essentialGraph.vertex(k-1));
        g2o::SE2 prevPose = prevVertex->estimate();
        g2o::SE2 currentPose = g2o::SE2(m_poses[k-1000](0), m_poses[k-1000](1), m_poses[k-1000](2));
        g2o::SE2 measurement = prevPose.inverse()*currentPose;
        odometryEdge->setMeasurement(measurement);
        odometryEdge->setInformation(Eigen::Matrix3d::Identity()*1/0.5); //Actual covariance should be configured
        essentialGraph.addEdge(odometryEdge);
      }
    }

    //Connect cones to poses
    for(uint32_t i = graphIndexStart; i < graphIndexEnd; i++){

        Eigen::Vector2d coneMeanXY;
        if(!m_coneList[i].isOptimized()){
          m_coneList[i].calculateMean();
          coneMeanXY << m_coneList[i].getMeanX(),m_coneList[i].getMeanY();
        }else{
          coneMeanXY << m_coneList[i].getOptX(),m_coneList[i].getOptY();
        }
      
        g2o::VertexPointXY* coneVertex = new g2o::VertexPointXY;
        coneVertex->setId(m_coneList[i].getId());
        coneVertex->setEstimate(coneMeanXY);
        essentialGraph.addVertex(coneVertex);

        //Connect cones to POses
        g2o::EdgeSE2PointXY* coneMeasurement = new g2o::EdgeSE2PointXY;
        std::vector<int> connectedPoses = m_coneList[i].getConnectedPoses();

      for(uint32_t j = 0; j < connectedPoses.size(); j++){
          Eigen::Vector2d xyMeasurement;
          xyMeasurement = getConeToPoseMeasurement(i,j);
          //std::cout << "x: " << xyMeasurement(0) << " y: " << xyMeasurement(1) << std::endl; 
          coneMeasurement->vertices()[0] = essentialGraph.vertex(connectedPoses[j]);
          coneMeasurement->vertices()[1] = essentialGraph.vertex(m_coneList[i].getId());
          coneMeasurement->setMeasurement(xyMeasurement);

          Eigen::Vector2d covXY = m_coneList[i].getCovariance();
          Eigen::Matrix2d informationMatrix;
          informationMatrix << 1/covXY(0),0,
                              0,1/covXY(1);
          coneMeasurement->setInformation(informationMatrix); //Placeholder value
          //std::cout << "cX: " << covXY(0) << " cY: " << covXY(1) << std::endl;
          essentialGraph.addEdge(coneMeasurement);

        }
      }


    g2o::VertexSE2* firstRobotPose = dynamic_cast<g2o::VertexSE2*>(essentialGraph.vertex(min));
    firstRobotPose->setFixed(true);

    /*g2o::VertexPointXY* firstCone = dynamic_cast<g2o::VertexPointXY*>(essentialGraph.vertex(graphIndexStart));
    firstCone->setFixed(true);*/
    //std::cout << "Optimizing" << std::endl;
    essentialGraph.initializeOptimization();
    essentialGraph.optimize(10); //Add config for amount of iterations??
    //std::cout << "Optimizing done." << std::endl;

    updateFromEssential(min, max, graphIndexStart,graphIndexEnd, essentialGraph);
    updateMap(graphIndexStart,graphIndexEnd,false);
  }
}

void Slam::updateFromEssential(uint32_t poseStart, uint32_t poseEnd,uint32_t coneStart,uint32_t coneEnd, g2o::SparseOptimizer &essentialGraph){

  //Update pose vector

  for(uint32_t i = poseStart; i < poseEnd; i++){
    g2o::VertexSE2* updatedPoseVertex = static_cast<g2o::VertexSE2*>(essentialGraph.vertex(i));
    g2o::SE2 updatedPoseSE2 = updatedPoseVertex->estimate();
    Eigen::Vector3d updatedPose = updatedPoseSE2.toVector();
    m_poses[i-1000] << updatedPose(0),updatedPose(1),updatedPose(2);
  }
  //Set optimized cone positions

   Eigen::Vector2d updatedConeXY;
   g2o::VertexPointXY* updatedConeVertex;

   for(uint32_t i = coneStart; i < coneEnd; i++){
    updatedConeVertex = static_cast<g2o::VertexPointXY*>(essentialGraph.vertex(i));
    updatedConeXY = updatedConeVertex->estimate();
    m_coneList[i].setOptX(updatedConeXY(0));
    m_coneList[i].setOptY(updatedConeXY(1));
    m_coneList[i].setOptimized();    
   }
}

Eigen::Vector3d Slam::updatePoseFromGraph(){

  g2o::VertexSE2* updatedPoseVertex = static_cast<g2o::VertexSE2*>(m_optimizer.vertex(m_poseId-1));
  g2o::SE2 updatedPoseSE2 = updatedPoseVertex->estimate();
  Eigen::Vector3d updatedPose = updatedPoseSE2.toVector();
  return updatedPose;
}

void Slam::addPosesToGraph(){

  for(uint32_t i = 0; i < m_poses.size(); i++){
    g2o::VertexSE2* poseVertex = new g2o::VertexSE2;
    poseVertex->setId(i+1000);
    poseVertex->setEstimate(m_poses[i]);

    m_optimizer.addVertex(poseVertex);

    addOdometryMeasurement(m_poses[i],i);

    std::vector<int> poseVector;
    m_connectivityGraph.push_back(poseVector);
  }
}


void Slam::addOdometryMeasurement(Eigen::Vector3d pose, uint32_t i){
  if(i > 0){
    g2o::EdgeSE2* odometryEdge = new g2o::EdgeSE2;

    odometryEdge->vertices()[0] = m_optimizer.vertex(i+999);
    odometryEdge->vertices()[1] = m_optimizer.vertex(i+1000);
    g2o::VertexSE2* prevVertex = static_cast<g2o::VertexSE2*>(m_optimizer.vertex(i+999));
    g2o::SE2 prevPose = prevVertex->estimate();
    g2o::SE2 currentPose = g2o::SE2(pose(0), pose(1), pose(2));
    g2o::SE2 measurement = prevPose.inverse()*currentPose;
    odometryEdge->setMeasurement(measurement);
    odometryEdge->setInformation(Eigen::Matrix3d::Identity()*1/0.5); //Actual covariance should be configured
    m_optimizer.addEdge(odometryEdge);
  }
}

void Slam::fullBA(){

  createFullGraph();

  g2o::VertexSE2* firstRobotPose = dynamic_cast<g2o::VertexSE2*>(m_optimizer.vertex(1000));
  firstRobotPose->setFixed(true);

  /*g2o::VertexSE2* secondRobotPose = dynamic_cast<g2o::VertexSE2*>(m_optimizer.vertex(1001));
  secondRobotPose->setFixed(true);

  g2o::VertexPointXY* firstCone = dynamic_cast<g2o::VertexPointXY*>(m_optimizer.vertex(0));
  firstCone->setFixed(true);

  g2o::VertexPointXY* secondCone = dynamic_cast<g2o::VertexPointXY*>(m_optimizer.vertex(1));
  secondCone->setFixed(true);*/


  //m_optimizer.setVerbose(true);

  //std::cout << "Optimizing" << std::endl;
  m_optimizer.initializeOptimization();
  m_optimizer.optimize(10); //Add config for amount of iterations??
  //std::cout << "Optimizing done." << std::endl;


  Eigen::Vector2d updatedConeXY;
  g2o::VertexPointXY* updatedConeVertex;

  for(uint32_t j = 0; j < m_coneList.size(); j++){//Iterate and replace old map landmarks with new updated ones
    updatedConeVertex = static_cast<g2o::VertexPointXY*>(m_optimizer.vertex(j));
    updatedConeXY = updatedConeVertex->estimate();
    m_coneList[j].setOptX(updatedConeXY(0));
    m_coneList[j].setOptY(updatedConeXY(1));
  }
  
  for(uint32_t i = 0; i < m_poses.size(); i++){
    g2o::VertexSE2* updatedPoseVertex = static_cast<g2o::VertexSE2*>(m_optimizer.vertex(i+1000));
    g2o::SE2 updatedPoseSE2 = updatedPoseVertex->estimate();
    Eigen::Vector3d updatedPose = updatedPoseSE2.toVector();
    m_poses[i] << updatedPose(0),updatedPose(1),updatedPose(2);
  }
  //updateMap(0,m_coneList.size(),true);

}



Eigen::Vector3d Slam::coneToGlobal(Eigen::Vector3d pose, Eigen::MatrixXd cones){
  Eigen::Vector3d cone = Spherical2Cartesian(cones(0), cones(1), cones(2));
  //convert from local to global coordsystem
  double newX = cone(0)*cos(pose(2))-cone(1)*sin(pose(2));
  double newY = cone(0)*sin(pose(2))+cone(1)*cos(pose(2));
  cone(0) = newX+pose(0);
  cone(1) = newY+pose(1);
  cone(2) = cones(3);
  return cone;
}

Eigen::Vector2d Slam::transformConeToCoG(double angle, double distance){
  const double lidarDistToCoG = 1.5;
  double sign = angle/std::fabs(angle);
  angle = PI - std::fabs(angle*DEG2RAD); 
  double distanceNew = std::sqrt(lidarDistToCoG*lidarDistToCoG + distance*distance - 2*lidarDistToCoG*distance*std::cos(angle));
  double angleNew = std::asin((std::sin(angle)*distance)/distanceNew )*RAD2DEG; 
  Eigen::Vector2d transformed;
  transformed << angleNew*sign,distanceNew;

  return transformed;
}

void Slam::addConesToGraph(){

  for(uint32_t i = 0; i < m_coneList.size(); i++){
    Eigen::Vector2d coneMeanXY;
    if(!m_coneList[i].isOptimized()){
      m_coneList[i].calculateMean();
      coneMeanXY << m_coneList[i].getMeanX(),m_coneList[i].getMeanY();
    }else{
      coneMeanXY << m_coneList[i].getOptX(),m_coneList[i].getOptY();
    }
    g2o::VertexPointXY* coneVertex = new g2o::VertexPointXY;
    coneVertex->setId(m_coneList[i].getId());
    coneVertex->setEstimate(coneMeanXY);
    m_optimizer.addVertex(coneVertex);
    addConeMeasurements(i);
    }
}

void Slam::addConeMeasurements(int i){
  g2o::EdgeSE2PointXY* coneMeasurement = new g2o::EdgeSE2PointXY;

  std::vector<int> connectedPoses = m_coneList[i].getConnectedPoses();

  for(uint32_t j = 0; j < connectedPoses.size(); j++){
  Eigen::Vector2d xyMeasurement;
  xyMeasurement = getConeToPoseMeasurement(i,j);

  coneMeasurement->vertices()[0] = m_optimizer.vertex(connectedPoses[j]);
  coneMeasurement->vertices()[1] = m_optimizer.vertex(m_coneList[i].getId());
  coneMeasurement->setMeasurement(xyMeasurement);

  Eigen::Vector2d covXY = m_coneList[i].getCovariance();
  Eigen::Matrix2d informationMatrix;
  informationMatrix << 1/covXY(0),0,
                       0,1/covXY(1);
  coneMeasurement->setInformation(informationMatrix); //Placeholder value

  m_optimizer.addEdge(coneMeasurement);
  }
  //m_connectivityGraph[m_poseId-1001].push_back(cone.getId());
}

Eigen::Vector2d Slam::getConeToPoseMeasurement(int i,int j){
  Eigen::Vector2d cone;
  cone = m_coneList[i].getLocalConeObservation(j); 
  Eigen::Vector2d measurement;
  measurement << cone(0), cone(1);  
  return measurement;
}

Eigen::Vector2d Slam::getLocalConeToPoseMeasurement(Eigen::Vector3d pose, Eigen::Vector2d cone){
  
  Eigen::Vector2d measurement;
  measurement << cone(0)-pose(0), cone(1)-pose(1);  
  return measurement;
}

Eigen::Vector3d Slam::Spherical2Cartesian(double azimuth, double zenimuth, double distance)
{
  //double xyDistance = distance * cos(azimuth * static_cast<double>(DEG2RAD));
  //azimuth = (azimuth > PI)?(azimuth-2*PI):(azimuth);
  //azimuth = (azimuth < -PI)?(azimuth+2*PI):(azimuth);
  Eigen::Vector2d transformedCone = transformConeToCoG(azimuth,distance);
  azimuth = transformedCone(0);
  distance = transformedCone(1);
  double xData = distance * cos(zenimuth * static_cast<double>(DEG2RAD))*cos(azimuth * static_cast<double>(DEG2RAD));
  double yData = distance * cos(zenimuth * static_cast<double>(DEG2RAD))*sin(azimuth * static_cast<double>(DEG2RAD));
  double zData = distance * sin(zenimuth * static_cast<double>(DEG2RAD));
  Eigen::MatrixXd recievedPoint = Eigen::Vector3d::Zero();
  recievedPoint << xData,
                   yData,
                   zData;
  return recievedPoint;
}

double Slam::optimizeHeading2(Eigen::MatrixXd cones,Eigen::Vector3d pose){

std::cout << "Optimizing Heading ..." << std::endl;
//Select interval in map around current cone index to find best suitable cone set 
double bestHeading = 0;
double oneDeg = 0.01745*2;
double angle = -PI;
double initialHeading = pose(2);
  double lastBestErrorDistance = 1000000;
uint32_t coneFrameSize = cones.cols();
bool doOpt = false; 
if(coneFrameSize > 4){
  coneFrameSize = 4;
  doOpt = true;
}else if(coneFrameSize == 3){
  doOpt = true;
}

if(doOpt){
  int32_t startConeSet = m_currentConeIndex;
  startConeSet = (startConeSet < 0)?(m_map.size() - startConeSet):(startConeSet);
  for(uint32_t i = 0; i < 4; i++){
    angle = -PI;
    //For each i iterate through coneSet with local coneframe size
    startConeSet = startConeSet + i;
      
      if(coneFrameSize == 4){       
        for(double k = angle; k < PI; k = k + oneDeg){
          pose(2) = k;
          Eigen::Vector3d globalCone1 = coneToGlobal(pose, cones.col(0));
          Eigen::Vector3d globalCone2 = coneToGlobal(pose, cones.col(1));
          Eigen::Vector3d globalCone3 = coneToGlobal(pose, cones.col(2));
          Eigen::Vector3d globalCone4 = coneToGlobal(pose, cones.col(3));

          //Check reprojection error of all combinations with this angle
          double errorDistance = 0;
          for(uint32_t j = 0; j < m_headingPerms4.size(); j++){
            uint32_t i1 = startConeSet + m_headingPerms4[j][0]-1;
            if(i1 > m_map.size()){
              i1 = i1 - m_map.size()-1;
            }
            uint32_t i2 = startConeSet + m_headingPerms4[j][1]-1;
            if(i2 > m_map.size()){
              i2 = i2 - m_map.size()-1;
            }
            uint32_t i3 = startConeSet + m_headingPerms4[j][2]-1;
            if(i3 > m_map.size()){
              i3 = i3 - m_map.size()-1;
            }
            uint32_t i4 = startConeSet + m_headingPerms4[j][3]-1;
            if(i4 > m_map.size()){
              i4 = i4 - m_map.size()-1;
            }

            //std::cout << "index: " << i1 << " " << i2 << " " << i3 << " " << i4 << std::endl;
            errorDistance += std::sqrt( (globalCone1(0)-m_map[i1].getOptX())*(globalCone1(0)-m_map[i1].getOptX()) + (globalCone1(1)-m_map[i1].getOptY())*(globalCone1(1)-m_map[i1].getOptY()) );
            errorDistance += std::sqrt( (globalCone2(0)-m_map[i2].getOptX())*(globalCone2(0)-m_map[i2].getOptX()) + (globalCone2(1)-m_map[i2].getOptY())*(globalCone2(1)-m_map[i2].getOptY()) );
            errorDistance += std::sqrt( (globalCone3(0)-m_map[i3].getOptX())*(globalCone3(0)-m_map[i3].getOptX()) + (globalCone3(1)-m_map[i3].getOptY())*(globalCone3(1)-m_map[i3].getOptY()) );
            errorDistance += std::sqrt( (globalCone4(0)-m_map[i4].getOptX())*(globalCone4(0)-m_map[i4].getOptX()) + (globalCone4(1)-m_map[i4].getOptY())*(globalCone4(1)-m_map[i4].getOptY()) );
           
            if(errorDistance < lastBestErrorDistance){

              lastBestErrorDistance = errorDistance;
              bestHeading = k;
              //std::cout << "error distance: " << errorDistance << "best heading: " << k << std::endl;
            }
          }
        }
      }

      //-----------------------------------
    if(coneFrameSize == 3){  
      for(double k = angle; k < PI; k = k + oneDeg){
          pose(2) = k;
          Eigen::Vector3d globalCone1 = coneToGlobal(pose, cones.col(0));
          Eigen::Vector3d globalCone2 = coneToGlobal(pose, cones.col(1));
          Eigen::Vector3d globalCone3 = coneToGlobal(pose, cones.col(2));

          //Check reprojection error of all combinations with this angle
          double errorDistance = 0;
          for(uint32_t j = 0; j < m_headingPerms3.size(); j++){
            uint32_t i1 = startConeSet + m_headingPerms3[j][0]-1;
            if(i1 > m_map.size()){
              i1 = i1 - m_map.size()-1;
            }
            uint32_t i2 = startConeSet + m_headingPerms3[j][1]-1;
            if(i2 > m_map.size()){
              i2 = i2 - m_map.size()-1;
            }
            uint32_t i3 = startConeSet + m_headingPerms3[j][2]-1;
            if(i3 > m_map.size()){
              i3 = i3 - m_map.size()-1;
            }
    

            //std::cout << "index: " << i1 << " " << i2 << " " << i3 << " " << i4 << std::endl;
            errorDistance += std::sqrt( (globalCone1(0)-m_map[i1].getOptX())*(globalCone1(0)-m_map[i1].getOptX()) + (globalCone1(1)-m_map[i1].getOptY())*(globalCone1(1)-m_map[i1].getOptY()) );
            errorDistance += std::sqrt( (globalCone2(0)-m_map[i2].getOptX())*(globalCone2(0)-m_map[i2].getOptX()) + (globalCone2(1)-m_map[i2].getOptY())*(globalCone2(1)-m_map[i2].getOptY()) );
            errorDistance += std::sqrt( (globalCone3(0)-m_map[i3].getOptX())*(globalCone3(0)-m_map[i3].getOptX()) + (globalCone3(1)-m_map[i3].getOptY())*(globalCone3(1)-m_map[i3].getOptY()) );
           
            if(errorDistance < lastBestErrorDistance){

              lastBestErrorDistance = errorDistance;
              bestHeading = k;
              //std::cout << "error distance: " << errorDistance << "best heading: " << k << std::endl;
            }
          }
        }
      }  

  }
}
  //reproject the n amount of locally observed cones in the global map with new heading

  //match all cones with all possible combinations and calculate error

  //Save heading which give the smallest reprojection error
  if(doOpt && lastBestErrorDistance < 3){

    std::cout << "optimized heading: " << bestHeading << " errorDistance: " << lastBestErrorDistance << std::endl;
    return bestHeading;
  }else{

    return initialHeading;
  } 
}

double Slam::optimizeHeading(Eigen::MatrixXd cones,Eigen::Vector3d pose){

//Find surrounding cone indexes of 20 meters
if(cones.cols() > 2){
  double initPose = pose(2);
  std::vector<uint32_t> inMapIndex;
  for(uint32_t i = 0; i < m_map.size(); i++){
    opendlv::logic::perception::ObjectDistance objDistance = m_map[i].getDistance(pose);
    if(objDistance.distance() < 30){
      inMapIndex.push_back(i);
    }
  }
  double angle = pose(2)-PI/4;
  
  double angleMax = pose(2)+PI/4;
  double degrees = 2;
  double angleStep = 0.01745*degrees;

  double bestHeading = 0;
  double bestSumError = 100000;
  std::vector<double> coneErrors;
    for(double k = angle; k < angleMax; k = k + angleStep){
      pose(2) = k;
      double sumOfAllErrors = 0;
      int lastConeFitter = 0;
      for(uint32_t i = 0; i < cones.cols(); i++){
        Eigen::Vector3d globalCone = coneToGlobal(pose, cones.col(i));
        double minimumError = 100000;
        
        for(uint32_t j = 0; j < inMapIndex.size(); j++){
        double errorDistance = std::sqrt( (globalCone(0)-m_map[inMapIndex[j]].getOptX())*(globalCone(0)-m_map[inMapIndex[j]].getOptX()) + (globalCone(1)-m_map[inMapIndex[j]].getOptY())*(globalCone(1)-m_map[inMapIndex[j]].getOptY()) );
          if(errorDistance < minimumError){
            minimumError = errorDistance;
          }
        }//Map
        sumOfAllErrors += minimumError;
        coneErrors.push_back(minimumError);        
      }//Local Frame
        bool betterSum = false;
        if(sumOfAllErrors < bestSumError){
          betterSum = true;
          
        }  
          int conesThatFits = 0;
          for(uint32_t l = 0; l < coneErrors.size(); l++){
            if(coneErrors[l] < 1.5){

              conesThatFits++;
            }
          }
          std::cout << "Fitted Cones: " << conesThatFits << std::endl;
          if(conesThatFits > 2 && conesThatFits >= lastConeFitter && betterSum){
            std::cout << "new Best Heading: " << k << std::endl;
            std::cout << "best Error: " << sumOfAllErrors << std::endl;
            bestHeading = k;
            lastConeFitter = conesThatFits;
            bestSumError = sumOfAllErrors;
          }
        
          coneErrors.clear();
  }  
  if(bestSumError < 3){

    return bestHeading;
  }else{
    return initPose;
  }
}

  return pose(2);

}

void Slam::sendCones()
{
  Eigen::Vector3d pose;
  {
    std::lock_guard<std::mutex> lockSend(m_sendMutex); 
    pose = m_sendPose;
  }//mapmutex too
  std::lock_guard<std::mutex> lockMap(m_mapMutex);
  //std::chrono::system_clock::time_point tp = std::chrono::system_clock::now();
  cluon::data::TimeStamp sampleTime = m_lastTimeStamp;
  for(uint32_t i = 0; i< m_conesPerPacket;i++){ //Iterate through the cones ahead of time the path planning recieves
    int index = (m_currentConeIndex+i<m_map.size())?(m_currentConeIndex+i):(m_currentConeIndex+i-m_map.size()); //Check if more cones is sent than there exists
    opendlv::logic::perception::ObjectDirection directionMsg = m_map[index].getDirection(pose); //Extract cone direction
    directionMsg.objectId(m_conesPerPacket-1-i);
    od4.send(directionMsg,sampleTime,m_senderStamp);
    opendlv::logic::perception::ObjectDistance distanceMsg = m_map[index].getDistance(pose); //Extract cone distance
    distanceMsg.objectId(m_conesPerPacket-1-i);
    od4.send(distanceMsg,sampleTime,m_senderStamp);
    opendlv::logic::perception::ObjectType typeMsg;
    typeMsg.type(m_map[index].getType()); //Extract cone type
    typeMsg.objectId(m_conesPerPacket-1-i);
    od4.send(typeMsg,sampleTime,m_senderStamp);
  }
}

void Slam::sendPose(){
  opendlv::logic::sensation::Geolocation poseMessage;
  std::lock_guard<std::mutex> lockSend(m_sendMutex); 
  poseMessage.longitude(m_sendPose(0));
  poseMessage.latitude(m_sendPose(1));
  poseMessage.heading(static_cast<float>(m_sendPose(2)));
  cluon::data::TimeStamp sampleTime = m_geolocationReceivedTime;
  od4.send(poseMessage, sampleTime ,m_senderStamp);
}

double Slam::distanceBetweenCones(Cone c1, Cone c2){
  c1.calculateMean();
  c2.calculateMean();
  double distance = std::sqrt( (c1.getMeanX()-c2.getMeanX())*(c1.getMeanX()-c2.getMeanX()) + (c1.getMeanY()-c2.getMeanY())*(c1.getMeanY()-c2.getMeanY()) );
  return distance;
}
double Slam::distanceBetweenConesOpt(Cone c1, Cone c2){
  double distance = std::sqrt( (c1.getOptX()-c2.getMeanX())*(c1.getOptX()-c2.getMeanX()) + (c1.getOptY()-c2.getMeanY())*(c1.getOptY()-c2.getMeanY()) );
  return distance;
}
/*
void Slam::guessConeColor(){
  std::vector<Cone> colorMap;
  uint32_t currentConeIndex = 0;
  for(uint32_t i = 0; i<m_poses.size(); i++){
    Eigen::Vector3d pose = m_poses[i];
    currentConeIndex = updateCurrentCone(pose,currentConeIndex,5);
    Cone currentCone = m_map[currentConeIndex];
    auto distance = currentCone.getDistance(pose);
    auto direction = currentCone.getDirection(pose);
    if(distance.distance() < 10.0f && fabs(direction.azimuthAngle())>80.0f){
      if(direction.azimuthAngle()>0){
        currentCone.setType(2);
      }
      else{
        currentCone.setType(1);
      }
      colorMap.push_back(currentCone);
      }
    }
    m_map = colorMap;
}
*/
void Slam::updateMap(uint32_t start, uint32_t end, bool updateToGlobal){
  for(uint32_t i = start; i < end; i++){

    if(updateToGlobal && m_coneList[i].isValid()){
      m_map.push_back(m_coneList[i]);
    }else{
      m_essentialMap.push_back(m_coneList[i]);
    }
  }
}

void Slam::filterMap(){

  //Filter on mean and optimized value
  for(uint32_t i = 0; i < m_coneList.size(); i++){
    double distance = distanceBetweenConesOpt(m_coneList[i],m_coneList[i]);
    if(distance > m_newConeThreshold){
      m_coneList[i].setValidState(false);

    }
  }

  for(uint32_t i = 0; i < m_coneList.size(); i++){
    for(uint32_t j = 0; j < m_coneList.size(); j++){
      if(i != j){
        double distance = std::sqrt( (m_coneList[i].getOptX() - m_coneList[j].getOptX() )*(m_coneList[i].getOptX() - m_coneList[j].getOptX()) + (m_coneList[i].getOptY() - m_coneList[j].getOptY())*(m_coneList[i].getOptY() - m_coneList[j].getOptY()) );

        if(distance < m_newConeThreshold && m_coneList[i].isValid() && m_coneList[j].isValid()){
          m_coneList[j].setValidState(false);
        }
      } 
    }
  }  


  //Check closest pose didstance
  for(uint32_t i = 0; i < m_coneList.size(); i++){
    double closestPoseDistance = 10000;
    uint32_t closestPoseId = 0;
    for(uint32_t j = 0; j < m_poses.size(); j++){
      double distance = std::sqrt( (m_poses[j](0)-m_coneList[i].getOptX())*(m_poses[j](0)-m_coneList[i].getOptX()) + (m_poses[j](1)-m_coneList[i].getOptY())*(m_poses[j](1)-m_coneList[i].getOptY()) );
      if(distance < closestPoseDistance){
        closestPoseDistance = distance;
        closestPoseId = j;
      }

    }
    if(closestPoseDistance > 4 || m_coneList[i].getObservations()<2){
      m_coneList[i].setValidState(false);
    }
    else{
      auto direction = m_coneList[i].getDirection(m_poses[closestPoseId]);
      if(direction.azimuthAngle()>0){
        m_coneList[i].setType(1);
      }
      else{
        m_coneList[i].setType(2);
      }
      
    }

  }

  //Check colours
/*
  for(uint32_t i = 0; i < m_coneList.size(); i++){
    double distance = 10000;
    double azimuth = 1000;
    if(m_coneList[i].getType() == 0){
      for(uint32_t j = 0; j < m_coneList[i].getObservations(); j++ ){
        
        Eigen::Vector2d localObs = m_coneList[i].getLocalConeObservation(j);
        double localDistance = std::sqrt( localObs(0)*localObs(0) + localObs(1)*localObs(1) );
        if(localDistance < distance){
          distance = localDistance;
          azimuth = std::atan2(localObs(1),localObs(0));
        }
      }


      if(azimuth > 0.1 ){

        m_coneList[i].setType(1);

        //std::cout << "New type yellow" << std::endl;
      }else if(azimuth < -0.1){
        m_coneList[i].setType(2);
        //std::cout << "New type blue" << std::endl;
      }
    }*/


  //}
}

void Slam::setUp(std::map<std::string, std::string> configuration)
{

  m_timeDiffMilliseconds = static_cast<uint32_t>(std::stoi(configuration["gatheringTimeMs"]));
  m_newConeThreshold = static_cast<double>(std::stod(configuration["sameConeThreshold"]));
  m_gpsReference[0] = static_cast<double>(std::stod(configuration["refLatitude"]));
  m_gpsReference[1] = static_cast<double>(std::stod(configuration["refLongitude"]));
  m_timeBetweenKeyframes = static_cast<double>(std::stod(configuration["timeBetweenKeyframes"]));
  m_coneMappingThreshold = static_cast<double>(std::stod(configuration["coneMappingThreshold"]));
  m_conesPerPacket = static_cast<int>(std::stoi(configuration["conesPerPacket"]));
  m_lapSize = std::stoi(configuration["lapSize"]);
  //std::cout << "Cones per packet" << m_conesPerPacket << std::endl;
  m_senderStamp = static_cast<int>(std::stoi(configuration["id"]));

  //Create permutaions for optimize heading
  m_headingPerms4 = getPermutations(4);

  for(uint32_t i = 0; i < m_headingPerms4.size(); i++){

    std::cout << m_headingPerms4[i][0] << " " << m_headingPerms4[i][1] << " " << m_headingPerms4[i][2] << " " << m_headingPerms4[i][3] << std::endl;

  }

  m_headingPerms3 = getPermutations(3);
  for(uint32_t i = 0; i < m_headingPerms3.size(); i++){

    std::cout << m_headingPerms3[i][0] << " " << m_headingPerms3[i][1] << " " << m_headingPerms3[i][2] << std::endl;

  }
}

std::vector<std::vector<int>> Slam::getPermutations(int n){

    std::vector<std::vector<int>> perms;
  if(n==4){
        int array[4]={1,2,3,4};
        int addCounter = 0;
      for (int cnt4=4;cnt4!=0;--cnt4){
          for (int cnt3=3;cnt3!=0;--cnt3){
             for (int cnt2=2;cnt2!=0;--cnt2){
                 std::vector<int> currentPerm;
                  currentPerm.push_back(array[0]);
                  currentPerm.push_back(array[1]);
                  currentPerm.push_back(array[2]);
                  currentPerm.push_back(array[3]);
                  perms.push_back(currentPerm);
                  addCounter++;
                 int swap2=array[2];
                 array[2]=array[3];
                 array[3]=swap2;
             }
             int swap3=array[1];
             array[1]=array[2];
             array[2]=array[3];
             array[3]=swap3;
          }
          int swap4=array[0];
          array[0]=array[1];
          array[1]=array[2];
          array[2]=array[3];
          array[3]=swap4;
      }
  }
    if(n==3){

        int array1[3]={1,2,3};
        int array2[3]={2,1,3};
        int array3[3]={3,1,2};
        int array4[3]={1,3,2};
        int array5[3]={2,3,1};
        int array6[3]={3,2,1};
      
      std::vector<int> currentPerm;
      currentPerm.push_back(array1[0]);
      currentPerm.push_back(array1[1]);
      currentPerm.push_back(array1[2]);  
      perms.push_back(currentPerm);
      currentPerm.clear();
      currentPerm.push_back(array2[0]);
      currentPerm.push_back(array2[1]);
      currentPerm.push_back(array2[2]);  
      perms.push_back(currentPerm);
      currentPerm.clear();
      currentPerm.push_back(array3[0]);
      currentPerm.push_back(array3[1]);
      currentPerm.push_back(array3[2]);  
      perms.push_back(currentPerm);
      currentPerm.clear();
      currentPerm.push_back(array4[0]);
      currentPerm.push_back(array4[1]);
      currentPerm.push_back(array4[2]);  
      perms.push_back(currentPerm);
      currentPerm.clear();
      currentPerm.push_back(array5[0]);
      currentPerm.push_back(array5[1]);
      currentPerm.push_back(array5[2]);  
      perms.push_back(currentPerm);
      currentPerm.clear();
      currentPerm.push_back(array6[0]);
      currentPerm.push_back(array6[1]);
      currentPerm.push_back(array6[2]);  
      perms.push_back(currentPerm);
      currentPerm.clear();
    
  }
      return perms;
}
void Slam::initializeModule(){
  //local Gps Vars
  double lastOdoX = 100000;
  double lastOdoY = 100000;
  int validGpsMeasurements = 0;
  bool gpsReadyState = false;

  //Local IMU vars
  bool imuReadyState = false;
  float lastVel = 100000;
  float lastHead = 100000;
  int validVelMeasurements = 0;
  int validHeadMeasurements = 0;
  while(!m_readyState){
    bool sleep = true;
    auto start = std::chrono::system_clock::now();

    while(sleep)
    {
      auto now = std::chrono::system_clock::now();
      auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - start);

      if(elapsed.count() > 50*1000){
        //std::cout << "Timed out" << std::endl;
        sleep = false;
      }
    }
    //GPS

    std::lock_guard<std::mutex> lockSensor(m_sensorMutex);
    if(!gpsReadyState){

      if( std::fabs(m_odometryData(0) - lastOdoX) > 0.001 && std::fabs(m_odometryData(1) - lastOdoY) > 0.001){
        if(m_odometryData(0) < 200 && m_odometryData(1) < 200){
          lastOdoX = m_odometryData(0);
          lastOdoY = m_odometryData(1);
          validGpsMeasurements++;
        }
      }else{}

      if(validGpsMeasurements > 5){
        gpsReadyState = true;
        std::cout << "GPS Ready .." << std::endl;
      }
    }//GPS end  
    //IMU
    if(!imuReadyState){

      std::lock_guard<std::mutex> lockGroundSpeed(m_groundSpeedMutex);
      if(std::fabs(m_groundSpeed - lastVel) > 0.001){ 
        lastVel = m_groundSpeed;  
        validVelMeasurements++;
      }
      if(std::fabs(m_odometryData(2) - lastHead) > 0.001){
        lastHead = static_cast<float>(m_odometryData(2));  
        validHeadMeasurements++;
      }
      if(validVelMeasurements > 30 && validHeadMeasurements > 30){
        imuReadyState = true;
        std::cout << "IMU Ready .." << std::endl;
      }
    }

    if(gpsReadyState && imuReadyState){
      m_readyState = true;
      std::cout << "Slam ready check done !" << std::endl;  
    }
  }//While
  
  

}
void Slam::setStateMachineStatus(cluon::data::Envelope data){
  std::lock_guard<std::mutex> lockStateMachine(m_stateMachineMutex);
  auto machineStatus = cluon::extractMessage<opendlv::proxy::SwitchStateReading>(std::move(data));
  int state = machineStatus.state();
  if(state == 2){
    m_readyStateMachine = true;
  }
  
}
bool Slam::getModuleState(){

  return m_readyState;

}
std::vector<Eigen::Vector3d> Slam::drawPoses(){
  std::lock_guard<std::mutex> lockSensor(m_sensorMutex);
  return m_poses;
}

std::vector<Cone> Slam::drawCones(){
  std::lock_guard<std::mutex> lock(m_mapMutex);
  return m_map;
}
std::vector<Cone> Slam::drawRawCones(){
  std::lock_guard<std::mutex> lock(m_mapMutex);
  return m_coneList;
}
std::vector<Cone> Slam::drawLocalOptimizedCones(){
  std::lock_guard<std::mutex> lock(m_mapMutex);
  return m_essentialMap;
}
Eigen::Vector3d Slam::drawCurrentPose(){
  if(m_loopClosingComplete){
    std::lock_guard<std::mutex> lock(m_sendMutex);
    return m_sendPose;
  }
  else{
    std::lock_guard<std::mutex> lock(m_sensorMutex);
    return m_odometryData;
  }
}
Eigen::Vector3d Slam::drawCurrentUKFPose(){

    std::lock_guard<std::mutex> lock(m_sensorMutex);
    return m_odometryData;
  
}
std::vector<std::vector<int>> Slam::drawGraph(){
  std::lock_guard<std::mutex> lock1(m_mapMutex);
  std::lock_guard<std::mutex> lock2(m_sensorMutex);
  return m_connectivityGraph;
 
}
void Slam::writeToPoseAndMapFile()
{
  std::string filepathMap;
  filepathMap = "./map.txt";
	
		std::ofstream f;
    	f.open(filepathMap.c_str());
		for(uint32_t i = 0; i<m_map.size(); i++){

				f << std::setprecision(9) << m_map[i].getX() << "\t" << m_map[i].getY() << std::endl;
		}
		f.close();
		std::cout << "map with " << m_map.size() << " points saved" << std::endl;


    std::string filepathPose;
    filepathPose = "./pose.txt";
		std::ofstream p;
    p.open(filepathPose.c_str());
		for(uint32_t i = 0; i<m_poses.size(); i++){

				p << std::setprecision(9) << m_poses[i](0) << "\t" << m_poses[i](1) << "\t" << m_poses[i](2) << std::endl;
		}
		p.close();

}
void Slam::tearDown()
{
}
Slam::~Slam()
{
}
