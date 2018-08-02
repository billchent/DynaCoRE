#include "WalkingJPosTest.hpp"
#include <Mercury_Controller/Mercury_StateProvider.hpp>
#include <Mercury_Controller/Mercury_InvKinematics.hpp>
#include <ParamHandler/ParamHandler.hpp>
#include <Planner/PIPM_FootPlacementPlanner/Reversal_LIPM_Planner.hpp>
#include <Utils/DataManager.hpp>
#include <Mercury/Mercury_Model.hpp>
#include <Mercury_Controller/Mercury_DynaControl_Definition.h>


#include <Mercury_Controller/CtrlSet/JPosTargetCtrl.hpp>
#include <Mercury_Controller/CtrlSet/JPosPostureFixCtrl.hpp>
#include <Mercury_Controller/CtrlSet/JPosSwingCtrl.hpp>

WalkingJPosTest::WalkingJPosTest(RobotSystem* robot):Test(robot),
    num_step_(0)
{
    sp_ = Mercury_StateProvider::getStateProvider();
    sp_->global_pos_local_[1] = 0.15;
    robot_sys_ = robot;
    reversal_planner_ = new Reversal_LIPM_Planner();
    inv_kin_ = new Mercury_InvKinematics();

    phase_ = WkJPosPhase::initiation;
    state_list_.clear();

    jpos_ctrl_ = new JPosTargetCtrl(robot);
    body_up_ctrl_ = new JPosTargetCtrl(robot);
    body_fix_ctrl_ = new JPosPostureFixCtrl(robot);

    // Right
    right_swing_start_trans_ctrl_ = new JPosPostureFixCtrl(robot);
    right_swing_ctrl_ = 
        new JPosSwingCtrl(robot, mercury_link::rightFoot, reversal_planner_);
    right_swing_end_trans_ctrl_ = new JPosPostureFixCtrl(robot);
    // Left
    left_swing_start_trans_ctrl_ = new JPosPostureFixCtrl(robot);
    left_swing_ctrl_ = 
        new JPosSwingCtrl(robot, mercury_link::leftFoot, reversal_planner_);
    left_swing_end_trans_ctrl_ = new JPosPostureFixCtrl(robot);


    _SettingParameter();
    state_list_.push_back(jpos_ctrl_);
    state_list_.push_back(body_up_ctrl_);
    state_list_.push_back(body_fix_ctrl_);
    state_list_.push_back(right_swing_start_trans_ctrl_);
    state_list_.push_back(right_swing_ctrl_);
    state_list_.push_back(right_swing_end_trans_ctrl_);
    state_list_.push_back(body_fix_ctrl_);
    state_list_.push_back(left_swing_start_trans_ctrl_);
    state_list_.push_back(left_swing_ctrl_);
    state_list_.push_back(left_swing_end_trans_ctrl_);

    printf("[Walking JPos Test] Constructure\n");
}
WalkingJPosTest::~WalkingJPosTest(){

    delete jpos_ctrl_;
    delete body_up_ctrl_;

    delete inv_kin_;
}
void WalkingJPosTest::TestInitialization(){
    // Planner
    reversal_planner_->PlannerInitialization(MercuryConfigPath"PLANNER_velocity_reversal");
    // Yaml file name
    jpos_ctrl_->CtrlInitialization("CTRL_jpos_initialization");
    body_up_ctrl_->CtrlInitialization("CTRL_jpos_initialization");
    body_fix_ctrl_->CtrlInitialization("CTRL_jpos_initialization");

    right_swing_start_trans_ctrl_->CtrlInitialization("CTRL_jpos_initialization");
    right_swing_end_trans_ctrl_->CtrlInitialization("CTRL_jpos_initialization");
    left_swing_start_trans_ctrl_->CtrlInitialization("CTRL_jpos_initialization");
    left_swing_end_trans_ctrl_->CtrlInitialization("CTRL_jpos_initialization");

    right_swing_ctrl_->CtrlInitialization("CTRL_right_jpos_swing");
    left_swing_ctrl_->CtrlInitialization("CTRL_left_jpos_swing");
}

int WalkingJPosTest::_NextPhase(const int & phase){
    int next_phase = phase + 1;
     printf("next phase: %i\n", next_phase);

    if(phase == WkJPosPhase::double_contact_1) {
        ++num_step_;
        printf("%i th step:\n", num_step_);
        sp_->stance_foot_ = mercury_link::leftFoot;

        // Global Frame Update
        dynacore::Vect3 next_local_frame_location;
        robot_sys_->getPos(mercury_link::leftFoot, next_local_frame_location);
        // when it start the left leg is already stance foot and 
        // global set by 0.15
        if(num_step_>1) { 
            sp_->global_pos_local_ += next_local_frame_location; 
        }
    }

    if(phase == WkJPosPhase::double_contact_2){
        ++num_step_;
        printf("%i th step:\n", num_step_);

        sp_->stance_foot_ = mercury_link::rightFoot;

        // Global Frame Update
        dynacore::Vect3 next_local_frame_location;
        robot_sys_->getPos(mercury_link::rightFoot, next_local_frame_location);
        sp_->global_pos_local_ += next_local_frame_location;
    }
    sp_->num_step_copy_ = num_step_;
    if(next_phase == WkJPosPhase::NUM_WALKING_PHASE) {
        return WkJPosPhase::double_contact_1;
    }
    else{ return next_phase; }
}

void WalkingJPosTest::_SettingParameter(){
    ParamHandler handler(MercuryConfigPath"TEST_walking_jpos.yaml");
    double tmp; bool b_tmp;
    std::vector<double> tmp_vec;
    std::string tmp_str;
    handler.getVector("initial_jpos", tmp_vec);
    ((JPosTargetCtrl*)jpos_ctrl_)->setTargetPosition(tmp_vec);
    ((JPosTargetCtrl*)body_up_ctrl_)->setInitialPosition(tmp_vec);

    handler.getValue("body_height", tmp);
    ((Reversal_LIPM_Planner*)reversal_planner_)->setOmega(tmp);

    _SetupStancePosture(tmp);
    ((JPosTargetCtrl*)body_up_ctrl_)->setTargetPosition(stance_jpos_);
    // Double Contact
    ((JPosPostureFixCtrl*)body_fix_ctrl_)->setPosture(stance_jpos_);
    // Transition
    ((JPosPostureFixCtrl*)right_swing_start_trans_ctrl_)->setPosture(stance_jpos_);
    ((JPosPostureFixCtrl*)right_swing_end_trans_ctrl_)->setPosture(stance_jpos_);
    ((JPosPostureFixCtrl*)left_swing_start_trans_ctrl_)->setPosture(stance_jpos_);
    ((JPosPostureFixCtrl*)left_swing_end_trans_ctrl_)->setPosture(stance_jpos_);
    // Swing
    ((JPosSwingCtrl*)right_swing_ctrl_)->setStancePosture(stance_jpos_);
    ((JPosSwingCtrl*)left_swing_ctrl_)->setStancePosture(stance_jpos_);

    //// Timing Setup
    handler.getValue("jpos_initialization_time", tmp);
    ((JPosTargetCtrl*)jpos_ctrl_)->setMovingTime(tmp);
    handler.getValue("body_lifting_time", tmp);
    ((JPosTargetCtrl*)body_up_ctrl_)->setMovingTime(tmp);

    // Stance Time
    handler.getValue("stance_time", tmp);
    ((JPosPostureFixCtrl*)body_fix_ctrl_)->setMovingTime(tmp);
    ((JPosSwingCtrl*)right_swing_ctrl_)->notifyStanceTime(tmp);
    ((JPosSwingCtrl*)left_swing_ctrl_)->notifyStanceTime(tmp);

    // Swing & prime Time
    handler.getValue("swing_time", tmp);
    ((JPosSwingCtrl*)right_swing_ctrl_)->setMovingTime(tmp);
    ((JPosSwingCtrl*)left_swing_ctrl_)->setMovingTime(tmp);

    // Transition Time
    handler.getValue("st_transition_time", tmp);
    ((JPosPostureFixCtrl*)right_swing_start_trans_ctrl_)->setMovingTime(tmp);
    ((JPosPostureFixCtrl*)right_swing_end_trans_ctrl_)->setMovingTime(tmp);
    ((JPosPostureFixCtrl*)left_swing_start_trans_ctrl_)->setMovingTime(tmp);
    ((JPosPostureFixCtrl*)left_swing_end_trans_ctrl_)->setMovingTime(tmp);

    ((JPosSwingCtrl*)right_swing_ctrl_)->notifyTransitionTime(tmp);
    ((JPosSwingCtrl*)left_swing_ctrl_)->notifyTransitionTime(tmp);

    //// Planning Setup
    handler.getBoolean("contact_switch_check", b_tmp);
    ((JPosSwingCtrl*)right_swing_ctrl_)->setContactSwitchCheck(b_tmp);
    ((JPosSwingCtrl*)left_swing_ctrl_)->setContactSwitchCheck(b_tmp);

    handler.getBoolean("replanning", b_tmp);
    ((JPosSwingCtrl*)right_swing_ctrl_)->setReplanning(b_tmp);
    ((JPosSwingCtrl*)left_swing_ctrl_)->setReplanning(b_tmp);

    handler.getValue("double_stance_mix_ratio", tmp);
    ((JPosSwingCtrl*)right_swing_ctrl_)->setDoubleStanceRatio(tmp);
    ((JPosSwingCtrl*)left_swing_ctrl_)->setDoubleStanceRatio(tmp);

    handler.getValue("transition_phase_mix_ratio", tmp);
    ((JPosSwingCtrl*)right_swing_ctrl_)->setTransitionPhaseRatio(tmp);
    ((JPosSwingCtrl*)left_swing_ctrl_)->setTransitionPhaseRatio(tmp);
}

void WalkingJPosTest::_SetupStancePosture(double body_height){
    dynacore::Vector config_sol;
    dynacore::Vect3 rfoot_pos; rfoot_pos.setZero();
    dynacore::Vect3 lfoot_pos; lfoot_pos.setZero();
    rfoot_pos[1] = -0.1;
    lfoot_pos[1] = 0.1;

    double theta(0.0);
    dynacore::Quaternion des_quat(cos(theta/2.), 0., sin(theta/2.), 0);
    
    inv_kin_->solveFullInvKinematics(
            body_height, des_quat, rfoot_pos, lfoot_pos, config_sol);
    stance_jpos_ = config_sol.segment(mercury::num_virtual,  mercury::num_act_joint);
    //dynacore::pretty_print(sp_->Q_, std::cout, "sp-Q");
    //dynacore::pretty_print(config_sol, std::cout, "config sol");
}