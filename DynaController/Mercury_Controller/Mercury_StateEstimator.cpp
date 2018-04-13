#include "Mercury_StateEstimator.hpp"
#include "Mercury_StateProvider.hpp"
#include "Mercury_interface.hpp"
#include <Utils/utilities.hpp>
#include <Mercury/Mercury_Model.hpp>
#include <Mercury/Mercury_Definition.h>
#include <Filter/filters.hpp>

// Mocap based Estimator
#include <Mercury_Controller/StateEstimator/BodyFootPosEstimator.hpp>

// Orientation Estimators
#include <Mercury_Controller/StateEstimator/OriEstAccObs.hpp>
#include <Mercury_Controller/StateEstimator/BasicAccumulation.hpp>
#include <Mercury_Controller/StateEstimator/NoBias.hpp>
#include <Mercury_Controller/StateEstimator/NoAccState.hpp>


Mercury_StateEstimator::Mercury_StateEstimator(RobotSystem* robot):
    is_floating_(true){
        sp_ = Mercury_StateProvider::getStateProvider();
        robot_sys_ = robot;

        body_foot_est_ = new BodyFootPosEstimator(robot);
        ori_est_ = new BasicAccumulation();
        // ori_est_ = new OriEstAccObs();
        // ori_est_ = new NoBias();
        //ori_est_ = new NoAccState();
    }

Mercury_StateEstimator::~Mercury_StateEstimator(){
    delete ori_est_;
}

void Mercury_StateEstimator::Initialization(Mercury_SensorData* data){
    sp_->Q_.setZero();
    sp_->Qdot_.setZero();
    sp_->Q_[mercury::num_qdot] = 1.;

    // Joint Set
    for (int i(0); i<mercury::num_act_joint; ++i){
        sp_->Q_[mercury::num_virtual + i] = data->joint_jpos[i];
        sp_->Qdot_[mercury::num_virtual + i] = data->motor_jvel[i];
    }
    // Update Orientation w/ Mocap data
    body_foot_est_->getMoCapBodyOri(sp_->body_ori_);
    std::vector<double> imu_acc(3);
    std::vector<double> imu_ang_vel(3);

    for(int i(0); i<3; ++i){
        imu_acc[i]  = data->imu_acc[i];
        imu_ang_vel[i] = data->imu_ang_vel[i];
    }

    ori_est_->EstimatorInitialization(sp_->body_ori_, imu_acc, imu_ang_vel);
    sp_->Q_[3] = sp_->body_ori_.x();
    sp_->Q_[4] = sp_->body_ori_.y();
    sp_->Q_[5] = sp_->body_ori_.z();
    sp_->Q_[mercury::num_qdot] = sp_->body_ori_.w();
    robot_sys_->UpdateSystem(sp_->Q_, sp_->Qdot_);

    // Local Frame Setting
    if(is_floating_){
        dynacore::Vect3 foot_pos, foot_vel;
        robot_sys_->getPos(sp_->stance_foot_, foot_pos);
        robot_sys_->getLinearVel(sp_->stance_foot_, foot_vel);
        sp_->Q_[0] = -foot_pos[0];
        sp_->Q_[1] = -foot_pos[1];
        sp_->Q_[2] = -foot_pos[2];
        sp_->Qdot_[0] = -foot_vel[0];
        sp_->Qdot_[1] = -foot_vel[1];
        sp_->Qdot_[2] = -foot_vel[2];

        // sp_->global_pos_local_.head(2) = foot_pos.head(2);
        robot_sys_->UpdateSystem(sp_->Q_, sp_->Qdot_);
    }
    robot_sys_->getCoMPosition(sp_->CoM_pos_);
    robot_sys_->getCoMVelocity(sp_->CoM_vel_);

    // Warning: state provider setup
    sp_->SaveCurrentData();

    // Right Contact 
    if(data->rfoot_contact) sp_->b_rfoot_contact_ = 1;
    else sp_->b_rfoot_contact_ = 0;
    // Left Contact 
    if(data->lfoot_contact) sp_->b_lfoot_contact_ = 1;
    else sp_->b_lfoot_contact_ = 0;
}

void Mercury_StateEstimator::Update(Mercury_SensorData* data){
    sp_->Q_.setZero();
    sp_->Qdot_.setZero();
    sp_->Q_[mercury::num_qdot] = 1.;

    for (int i(0); i<mercury::num_act_joint; ++i){
        sp_->Q_[mercury::num_virtual + i] = data->joint_jpos[i];
        sp_->Qdot_[mercury::num_virtual + i] = data->motor_jvel[i];
    }
    std::vector<double> imu_acc(3);
    std::vector<double> imu_ang_vel(3);
    std::vector<double> imu_inc(3);

    for(int i(0); i<3; ++i){
        imu_inc[i] = data->imu_inc[i];
        imu_acc[i]  = data->imu_acc[i];
        imu_ang_vel[i] = data->imu_ang_vel[i];
    }
    ori_est_->setSensorData(imu_acc, imu_inc, imu_ang_vel);
    ori_est_->getEstimatedState(sp_->body_ori_, sp_->body_ang_vel_);
    //dynacore::pretty_print(sp_->body_ori_, std::cout, "dynacore quat");
    //printf("\n");

    if(is_floating_){
        sp_->Q_[3] = sp_->body_ori_.x();
        sp_->Q_[4] = sp_->body_ori_.y();
        sp_->Q_[5] = sp_->body_ori_.z();
        sp_->Q_[mercury::num_qdot] = sp_->body_ori_.w();

        for(int i(0); i<3; ++i)
            sp_->Qdot_[i+3] = sp_->body_ang_vel_[i];

        robot_sys_->UpdateSystem(sp_->Q_, sp_->Qdot_);

        // Foot position based offset
        dynacore::Vect3 foot_pos, foot_vel;
        robot_sys_->getPos(sp_->stance_foot_, foot_pos);
        robot_sys_->getLinearVel(sp_->stance_foot_, foot_vel);

        sp_->Q_[0] = -foot_pos[0];
        sp_->Q_[1] = -foot_pos[1];
        sp_->Q_[2] = -foot_pos[2];
        sp_->Qdot_[0] = -foot_vel[0];
        sp_->Qdot_[1] = -foot_vel[1];
        sp_->Qdot_[2] = -foot_vel[2];

        robot_sys_->UpdateSystem(sp_->Q_, sp_->Qdot_);
    }else{
        robot_sys_->UpdateSystem(sp_->Q_, sp_->Qdot_);
    }
    // Warning: Save Sensor Data in StateProvider
    sp_->SaveCurrentData();
    robot_sys_->getCoMPosition(sp_->CoM_pos_);
    robot_sys_->getCoMVelocity(sp_->CoM_vel_);
    // Right Contact 
    if(data->rfoot_contact) sp_->b_rfoot_contact_ = 1;
    else sp_->b_rfoot_contact_ = 0;
    // Left Contact 
    if(data->lfoot_contact) sp_->b_lfoot_contact_ = 1;
    else sp_->b_lfoot_contact_ = 0;

    for(int i(0); i<3; ++i){
        sp_->imu_acc_inc_[i] = data->imu_inc[i];
        sp_->imu_acc_[i] = data->imu_acc[i];
        sp_->imu_ang_vel_[i] = data->imu_ang_vel[i];
    }
}
