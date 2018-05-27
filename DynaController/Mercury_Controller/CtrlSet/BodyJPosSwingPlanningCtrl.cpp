#include "BodyJPosSwingPlanningCtrl.hpp"
#include <Configuration.h>
#include <Mercury_Controller/Mercury_StateProvider.hpp>
#include <Mercury_Controller/TaskSet/ConfigTask.hpp>
#include <Mercury_Controller/ContactSet/SingleContact.hpp>
#include <WBDC_Rotor/WBDC_Rotor.hpp>
#include <Mercury/Mercury_Model.hpp>
#include <Mercury/Mercury_Definition.h>
#include <ParamHandler/ParamHandler.hpp>
#include <Planner/PIPM_FootPlacementPlanner/Reversal_LIPM_Planner.hpp>
#include <Utils/DataManager.hpp>

#define MEASURE_TIME_WBDC 0

#if MEASURE_TIME_WBDC
#include <chrono>
#endif 

BodyJPosSwingPlanningCtrl::BodyJPosSwingPlanningCtrl(
        RobotSystem* robot, int swing_foot, Planner* planner):
    Controller(robot),
    swing_foot_(swing_foot),
    num_planning_(0),
    planning_frequency_(0.),
    replan_moment_(0.),
    push_down_height_(0.),
    ctrl_start_time_(0.),
    b_contact_switch_check_(false),
    des_jpos_(mercury::num_act_joint),
    des_jvel_(mercury::num_act_joint)
{
    planner_ = planner;
    curr_foot_pos_des_.setZero();
    curr_foot_vel_des_.setZero();
    curr_foot_acc_des_.setZero();

    curr_jpos_des_.setZero();
    curr_jvel_des_.setZero();
    curr_jacc_des_.setZero();


    config_body_foot_task_ = new ConfigTask();
    if(swing_foot == mercury_link::leftFoot) {
        single_contact_ = new SingleContact(robot, mercury_link::rightFoot); 
        swing_leg_jidx_ = mercury_joint::leftAbduction;
    }
    else if(swing_foot == mercury_link::rightFoot) {
        single_contact_ = new SingleContact(robot, mercury_link::leftFoot);
        swing_leg_jidx_ = mercury_joint::rightAbduction;
    }
    else printf("[Warnning] swing foot is not foot: %i\n", swing_foot);

    task_kp_ = dynacore::Vector::Zero(config_body_foot_task_->getDim());
    task_kd_ = dynacore::Vector::Zero(config_body_foot_task_->getDim());

    std::vector<bool> act_list;
    act_list.resize(mercury::num_qdot, true);
    for(int i(0); i<mercury::num_virtual; ++i) act_list[i] = false;


    wbdc_rotor_ = new WBDC_Rotor(act_list);
    wbdc_rotor_data_ = new WBDC_Rotor_ExtraData();
    wbdc_rotor_data_->A_rotor = 
        dynacore::Matrix::Zero(mercury::num_qdot, mercury::num_qdot);

    wbdc_rotor_data_->cost_weight = 
        dynacore::Vector::Constant(
                config_body_foot_task_->getDim() + 
                single_contact_->getDim(), 100.0);

    // for(int i(0); i<mercury::num_virtual; ++i) 
    //     wbdc_rotor_data_->cost_weight[i] = 350.;

    wbdc_rotor_data_->cost_weight.tail(single_contact_->getDim()) = 
        dynacore::Vector::Constant(single_contact_->getDim(), 1.0);
    wbdc_rotor_data_->cost_weight[config_body_foot_task_->getDim() + 2]  = 0.001; // Fr_z

    com_estimator_ = new LIPM_KalmanFilter();
    sp_ = Mercury_StateProvider::getStateProvider();
    printf("[BodyFootJPosPlanning Controller] Constructed\n");
}

BodyJPosSwingPlanningCtrl::~BodyJPosSwingPlanningCtrl(){
    delete config_body_foot_task_;
    delete single_contact_;
    delete wbdc_rotor_;
    delete wbdc_rotor_data_;
}

void BodyJPosSwingPlanningCtrl::OneStep(void* _cmd){
    _PreProcessing_Command();
    state_machine_time_ = sp_->curr_time_ - ctrl_start_time_;

    dynacore::Vector gamma;
    _single_contact_setup();
    _task_setup();
    _body_foot_ctrl(gamma);

    for(int i(0); i<mercury::num_act_joint; ++i){
        ((Mercury_Command*)_cmd)->jtorque_cmd[i] = gamma[i];
        ((Mercury_Command*)_cmd)->jpos_cmd[i] = des_jpos_[i];
        ((Mercury_Command*)_cmd)->jvel_cmd[i] = des_jvel_[i];
    }

    _PostProcessing_Command();
}
void BodyJPosSwingPlanningCtrl::_body_foot_ctrl(dynacore::Vector & gamma){
    dynacore::Vector fb_cmd = dynacore::Vector::Zero(mercury::num_act_joint);
    for (int i(0); i<mercury::num_act_joint; ++i){
        wbdc_rotor_data_->A_rotor(i + mercury::num_virtual, i + mercury::num_virtual)
            = sp_->rotor_inertia_[i];
    }
    wbdc_rotor_->UpdateSetting(A_, Ainv_, coriolis_, grav_);
    wbdc_rotor_->MakeTorque(task_list_, contact_list_, fb_cmd, wbdc_rotor_data_);

    gamma = wbdc_rotor_data_->cmd_ff;

    int offset(0);
    if(swing_foot_ == mercury_link::rightFoot) offset = 3;
    dynacore::Vector reaction_force = 
        (wbdc_rotor_data_->opt_result_).tail(single_contact_->getDim());
    for(int i(0); i<3; ++i)
        sp_->reaction_forces_[i + offset] = reaction_force[i];

    sp_->qddot_cmd_ = wbdc_rotor_data_->result_qddot_;
}


void BodyJPosSwingPlanningCtrl::_task_setup(){
    dynacore::Vector pos_des(config_body_foot_task_->getDim());
    dynacore::Vector vel_des(config_body_foot_task_->getDim());
    dynacore::Vector acc_des(config_body_foot_task_->getDim());
    pos_des.setZero(); vel_des.setZero(); acc_des.setZero();

    // Body height
    double target_height = ini_body_pos_[2];
    if(b_set_height_target_) target_height = des_body_height_;

    // Orientation
    dynacore::Vect3 rpy_des;
    dynacore::Quaternion des_quat;
    rpy_des.setZero();

    dynacore::convert(rpy_des, des_quat);

    _CoMEstiamtorUpdate();
    _CheckPlanning();        


    double traj_time = state_machine_time_ - replan_moment_;
    // Swing Foot Config Task
    double pos[3];
    double vel[3];
    double acc[3];

    // printf("time: %f\n", state_machine_time_);
    foot_traj_.getCurvePoint(traj_time, pos);
    foot_traj_.getCurveDerPoint(traj_time, 1, vel);
    foot_traj_.getCurveDerPoint(traj_time, 2, acc);
    // printf("pos:%f, %f, %f\n", pos[0], pos[1], pos[2]);

    for(int i(0); i<3; ++i){
        curr_foot_pos_des_[i] = pos[i];
        curr_foot_vel_des_[i] = vel[i];
        curr_foot_acc_des_[i] = acc[i];
    }

    dynacore::Vector config_sol, qdot_cmd, qddot_cmd;
   inv_kin_.getSingleSupportFullConfigSeperation(
            sp_->Q_, des_quat, target_height, 
            swing_foot_, curr_foot_pos_des_, curr_foot_vel_des_, curr_foot_acc_des_,
            config_sol, qdot_cmd, qddot_cmd);

    swing_leg_traj_.getCurvePoint(traj_time, pos);
    swing_leg_traj_.getCurveDerPoint(traj_time, 1, vel);
    swing_leg_traj_.getCurveDerPoint(traj_time, 2, acc);

    for(int i(0); i<3; ++i){
        config_sol[swing_leg_jidx_ + i] = pos[i];
        qdot_cmd[swing_leg_jidx_ + i] = vel[i];
        qddot_cmd[swing_leg_jidx_ + i] = acc[i];

        curr_jpos_des_[i] = pos[i];
        curr_jvel_des_[i] = vel[i];
        curr_jacc_des_[i] = acc[i];
    }
 
    for (int i(0); i<mercury::num_act_joint; ++i){
        pos_des[mercury::num_virtual + i] = config_sol[mercury::num_virtual + i];  
        vel_des[mercury::num_virtual + i] = qdot_cmd[mercury::num_virtual + i];
        acc_des[mercury::num_virtual + i] = qddot_cmd[mercury::num_virtual + i];

        des_jpos_[i] = pos_des[mercury::num_virtual + i];
        des_jvel_[i] = vel_des[mercury::num_virtual + i];
    }

    // Feedback gain decreasing
    //double tot_decreasing_time = gain_decreasing_period_portion_ * end_time_; 
    //double remain_time = end_time_ - state_machine_time_;
    //if(remain_time < 1.0e-5)  remain_time = 1.0e-5;
    //if(remain_time < tot_decreasing_time){        
        //dynacore::Vector Kp = task_kp_;
        //dynacore::Vector Kd = task_kd_;
        //Kp.segment(swing_leg_jidx_, 3) = 
            //remain_time/tot_decreasing_time * task_kp_.segment(swing_leg_jidx_, 3) 
            //+ (1. - remain_time/tot_decreasing_time) * gain_decreasing_ratio_
            //* task_kp_.segment(swing_leg_jidx_, 3); 
        //Kd.segment(swing_leg_jidx_, 3) = 
            //remain_time/tot_decreasing_time * task_kd_.segment(swing_leg_jidx_, 3) 
            //+ (1. - remain_time/tot_decreasing_time) * gain_decreasing_ratio_
            //* task_kd_.segment(swing_leg_jidx_, 3); 
        //_setTaskGain(Kp, Kd);
    //}

     //dynacore::pretty_print(vel_des, std::cout, "[Ctrl] vel des");
    // Push back to task list
    config_body_foot_task_->UpdateTask(&(pos_des), vel_des, acc_des);
    task_list_.push_back(config_body_foot_task_);
}

void BodyJPosSwingPlanningCtrl::_CheckPlanning(){
    if( state_machine_time_ > 
            (end_time_/(planning_frequency_ + 1.) * (num_planning_ + 1.) + 0.002) ){

        //if(state_machine_time_ > 0.5 * end_time_ + 0.002 && (num_planning_ < 1)){
        //+ 0.002 is to account one or two more ticks before the end of phase
        dynacore::Vect3 target_loc;
        _Replanning(target_loc);
        dynacore::Vector guess_q = sp_->Q_;
        _SetBspline(guess_q, 
            curr_jpos_des_, curr_jvel_des_, curr_jacc_des_, target_loc);
        _SetBspline(
            curr_foot_pos_des_, curr_foot_vel_des_, curr_foot_acc_des_, target_loc);
        ++num_planning_;
    }

    // Earlier planning
    //_Replanning();
    //++num_planning_;
    //}

    //printf("time (state/end): %f, %f\n", state_machine_time_, end_time_);
    // printf("planning freq: %f\n", planning_frequency_);
    // printf("num_planning: %i\n", num_planning_);
}

void BodyJPosSwingPlanningCtrl::_Replanning(dynacore::Vect3 & target_loc){
    dynacore::Vect3 com_pos, com_vel;
    dynacore::Vect3 del_com_pos;
    // Direct value used
    robot_sys_->getCoMPosition(com_pos);
    robot_sys_->getCoMVelocity(com_vel);

    // Estimated value used
    // for(int i(0); i<2; ++i){
    //     com_pos[i] = sp_->estimated_com_state_[i];
    //     com_vel[i] = sp_->estimated_com_state_[i + 2];
    // }
    // TEST
    // for(int i(0); i<2; ++i){
    //     del_com_pos[i] = com_pos[i] - ini_com_pos_[i];
    //     com_vel[i] = del_com_pos[i]/state_machine_time_;
    // }

    // TEST 2
    for(int i(0); i<2; ++i){
        com_pos[i] = sp_->Q_[i] + body_pt_offset_[i];
        del_com_pos[i] = sp_->Q_[i] - ini_config_[i];
        //sp_->average_vel_[i] = del_com_pos[i]/state_machine_time_;
        com_vel[i] = sp_->average_vel_[i]; 
    }
    // TEST 3 Y-axis
    // com_pos[1] = sp_->estimated_com_state_[1];
    // com_vel[1] = sp_->estimated_com_state_[3];


    printf("planning com state: %f, %f, %f, %f\n",
        com_pos[0], com_pos[1],
        com_vel[0], com_vel[1]);
    //com_pos[0] = sp_->Q_[0]-0.01; // use body position
    //com_vel[0] = 0.;  // forward backward velocity is small
    //com_vel[1] = 0.;

    OutputReversalPL pl_output;
    ParamReversalPL pl_param;
    pl_param.swing_time = end_time_ - state_machine_time_
        + transition_time_ * transition_phase_ratio_
        + stance_time_ * double_stance_ratio_;

    pl_param.des_loc = sp_->des_location_;
    pl_param.stance_foot_loc = sp_->global_pos_local_;

    if(swing_foot_ == mercury_link::leftFoot)
        pl_param.b_positive_sidestep = true;
    else
        pl_param.b_positive_sidestep = false;

    planner_->getNextFootLocation(com_pos + sp_->global_pos_local_,
            com_vel,
            target_loc,
            &pl_param, &pl_output);
    // Time Modification
    replan_moment_ = state_machine_time_;
    end_time_ += pl_output.time_modification;

    // dynacore::pretty_print(target_loc, std::cout, "planed foot loc");
    // dynacore::pretty_print(sp_->global_pos_local_, std::cout, "global loc");

    target_loc -= sp_->global_pos_local_;

    // TEST
    // if(sp_->num_step_copy_ < 2){
    //     target_loc[1] = sp_->Q_[1] + default_target_loc_[1];
    // }

    //target_loc[2] -= push_down_height_;
    target_loc[2] = default_target_loc_[2];
    dynacore::pretty_print(target_loc, std::cout, "next foot loc");
    //curr_foot_acc_des_.setZero();
    //curr_foot_vel_des_.setZero();


    // TEST
    // guess_q[1] += (pl_output.switching_state[1] - sp_->global_pos_local_[1]);

}

void BodyJPosSwingPlanningCtrl::_single_contact_setup(){
    single_contact_->UpdateContactSpec();
    contact_list_.push_back(single_contact_);
}

void BodyJPosSwingPlanningCtrl::FirstVisit(){
    ini_config_ = sp_->Q_;
    ctrl_start_time_ = sp_->curr_time_;
    state_machine_time_ = 0.;
    replan_moment_ = 0.;

    ini_swing_leg_config_ = sp_->Q_.segment(swing_leg_jidx_, 3);
    robot_sys_->getPos(swing_foot_, ini_foot_pos_);
    dynacore::Vect3 zero;
    zero.setZero();

    dynacore::Vect3 target_loc = default_target_loc_;
    target_loc[0] += sp_->Q_[0];
    target_loc[1] += sp_->Q_[1];
    target_loc[2] = ini_foot_pos_[2] - push_down_height_;
    // Compute JPos trajectory


    if(b_initial_planning_){
        // target_loc[0] = (sp_->Q_[0] + body_pt_offset_[0]) + 
        //             kp_x_ * (sp_->Q_[0] + body_pt_offset_[0]);

        _Replanning(target_loc);
        target_loc[1] = 
        (sp_->Q_[1] + body_pt_offset_[1])
         + default_target_loc_[1]
         + kp_y_ * (sp_->Q_[1] + body_pt_offset_[1]);
    }
    _SetBspline(sp_->Q_, ini_swing_leg_config_, zero, zero, target_loc);
    _SetBspline(ini_foot_pos_, zero, zero, target_loc);
    default_target_loc_[2] = target_loc[2];


    // _Replanning();
    num_planning_ = 0;

    dynacore::Vect3 com_vel;
    robot_sys_->getCoMPosition(ini_com_pos_);
    robot_sys_->getCoMVelocity(com_vel);
    dynacore::Vector input_state(4);
    input_state[0] = ini_com_pos_[0];   
    input_state[1] = ini_com_pos_[1];
    input_state[2] = com_vel[0];   input_state[3] = com_vel[1];

    com_estimator_->EstimatorInitialization(input_state);
    _CoMEstiamtorUpdate();

    //dynacore::pretty_print(ini_foot_pos_, std::cout, "ini foot pos");
    //dynacore::pretty_print(target_loc, std::cout, "target loc");
}

void BodyJPosSwingPlanningCtrl::_CoMEstiamtorUpdate(){
    dynacore::Vect3 com_pos, com_vel;
    robot_sys_->getCoMPosition(com_pos);
    robot_sys_->getCoMVelocity(com_vel);
    dynacore::Vector input_state(4);
    input_state[0] = com_pos[0];   input_state[1] = com_pos[1];
    input_state[2] = com_vel[0];   input_state[3] = com_vel[1];
    com_estimator_->InputData(input_state);
    com_estimator_->Output(sp_->estimated_com_state_);
}

void BodyJPosSwingPlanningCtrl::_SetBspline(
        const dynacore::Vector & guess_q,
        const dynacore::Vect3 & st_config,
        const dynacore::Vect3 & st_jvel,
        const dynacore::Vect3 & st_jacc,
        const dynacore::Vect3 & target_pos){
    // Trajectory Setup
    double init[9];
    double fin[9];
    double** middle_pt = new double*[1];
    middle_pt[0] = new double[3];

    // printf("time (state/end): %f, %f\n", state_machine_time_, end_time_);
    double portion = (1./end_time_) * (end_time_/2. - state_machine_time_);

    // Find Target Config
    dynacore::Vector config_sol;
    inv_kin_.getLegConfigAtVerticalPosture(swing_foot_, target_pos, 
            guess_q, config_sol);
    dynacore::Vect3 target_config = config_sol.segment(swing_leg_jidx_, 3);

    // Find Mid Config
    dynacore::Vect3 st_pos;
    inv_kin_.getFootPosAtVerticalPosture(swing_foot_, st_config, guess_q, st_pos);
    dynacore::Vect3 middle_pos;
    if(portion > 0.){
        middle_pos = (st_pos + target_pos)*portion;
        middle_pos[2] = swing_height_ + target_pos[2];
    } else {
        middle_pos = (st_pos + target_pos)/2.;
    }
    inv_kin_.getLegConfigAtVerticalPosture(swing_foot_, middle_pos, 
            guess_q, config_sol);
    dynacore::Vect3 mid_config = config_sol.segment(swing_leg_jidx_, 3);
    
    for(int i(0); i<3; ++i){
        // Initial
        init[i] = st_config[i];
        init[i+3] = st_jvel[i];
        init[i+6] = st_jacc[i];
        // Final
        fin[i] = target_config[i];
        fin[i+3] = 0.;
        fin[i+6] = 0.;
        // mid
        middle_pt[0][i] = mid_config[i];
   }

    swing_leg_traj_.SetParam(init, fin, middle_pt, end_time_ - replan_moment_);

    delete [] *middle_pt;
    delete [] middle_pt;
}

void BodyJPosSwingPlanningCtrl::_SetBspline(const dynacore::Vect3 & st_pos,
        const dynacore::Vect3 & st_vel,
        const dynacore::Vect3 & st_acc,
        const dynacore::Vect3 & target_pos){
    // Trajectory Setup
    double init[9];
    double fin[9];
    double** middle_pt = new double*[1];
    middle_pt[0] = new double[3];

    // printf("time (state/end): %f, %f\n", state_machine_time_, end_time_);
    double portion = (1./end_time_) * (end_time_/2. - state_machine_time_);
    // printf("portion: %f\n\n", portion);
    // Initial and final position & velocity & acceleration
    for(int i(0); i<3; ++i){
        // Initial
        init[i] = st_pos[i];
        init[i+3] = st_vel[i];
        init[i+6] = st_acc[i];
        // Final
        fin[i] = target_pos[i];
        fin[i+3] = 0.;
        fin[i+6] = 0.;

        if(portion > 0.)
            middle_pt[0][i] = (st_pos[i] + target_pos[i])*portion;
        else
            middle_pt[0][i] = (st_pos[i] + target_pos[i])/2.;
    }
    if(portion > 0.)  middle_pt[0][2] = swing_height_ + st_pos[2];

    foot_traj_.SetParam(init, fin, middle_pt, end_time_ - replan_moment_);

    delete [] *middle_pt;
    delete [] middle_pt;
}

void BodyJPosSwingPlanningCtrl::LastVisit(){
}

bool BodyJPosSwingPlanningCtrl::EndOfPhase(){
    if(state_machine_time_ > end_time_){
        //printf("[Body Foot Ctrl] End, state_machine time/ end time: (%f, %f)\n", 
        //state_machine_time_, end_time_);
        return true;
    }
    // Swing foot contact = END
    if(b_contact_switch_check_){
        bool contact_happen(false);
        if(swing_foot_ == mercury_link::leftFoot && sp_->b_lfoot_contact_){
            contact_happen = true;
        }
        if(swing_foot_ == mercury_link::rightFoot && sp_->b_rfoot_contact_){
            contact_happen = true;
        }
        if(state_machine_time_ > end_time_ * 0.5 && contact_happen){
            printf("[Body Foot JPos Ctrl] contact happen, state_machine_time/ end time: (%f, %f)\n",
                    state_machine_time_, end_time_);
            return true;
        }
    }
    return false;
}

void BodyJPosSwingPlanningCtrl::_setTaskGain(
        const dynacore::Vector & Kp, const dynacore::Vector & Kd){
    ((ConfigTask*)config_body_foot_task_)->Kp_vec_ = Kp;
    ((ConfigTask*)config_body_foot_task_)->Kd_vec_ = Kd;
}

void BodyJPosSwingPlanningCtrl::CtrlInitialization(const std::string & setting_file_name){
    ini_body_pos_ = sp_->Q_.head(3);
    std::vector<double> tmp_vec;

    // Setting Parameters
    ParamHandler handler(MercuryConfigPath + setting_file_name + ".yaml");
    handler.getValue("swing_height", swing_height_);
    handler.getValue("push_down_height", push_down_height_);

    handler.getVector("default_target_foot_location", tmp_vec);
    for(int i(0); i<3; ++i){
        default_target_loc_[i] = tmp_vec[i];
    }

    // Feedback Gain
    handler.getVector("Kp", tmp_vec);
    for(int i(0); i<tmp_vec.size(); ++i){
        task_kp_[i] = tmp_vec[i];
    }
    handler.getVector("Kd", tmp_vec);
    for(int i(0); i<tmp_vec.size(); ++i){
        task_kd_[i] = tmp_vec[i];
    }

    ((ConfigTask*)config_body_foot_task_)->Kp_vec_ = task_kp_;
    ((ConfigTask*)config_body_foot_task_)->Kd_vec_ = task_kd_;

    // Feedback gain decreasing near to the landing moment
    handler.getValue("gain_decreasing_ratio", gain_decreasing_ratio_);
    handler.getValue("gain_decreasing_period_portion", 
            gain_decreasing_period_portion_);

    // Body Point offset
    handler.getVector("body_pt_offset", tmp_vec);
    for(int i(0); i<2; ++i){
        body_pt_offset_[i] = tmp_vec[i];
    }

    handler.getBoolean("initial_planning", b_initial_planning_);
    handler.getValue("kp_x", kp_x_);
    handler.getValue("kp_y", kp_y_);

    static bool b_bodypute_eigenvalue(true);
    if(b_bodypute_eigenvalue){
        ((Reversal_LIPM_Planner*)planner_)->
            CheckEigenValues(double_stance_ratio_*stance_time_ + 
                    transition_phase_ratio_*transition_time_ + 
                    end_time_);
        b_bodypute_eigenvalue = false;
    }
    //printf("[Body Foot JPos Planning Ctrl] Parameter Setup Completed\n");
}