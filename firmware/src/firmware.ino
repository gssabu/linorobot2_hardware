// Copyright (c) 2021 Juan Miguel Jimeno
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include <Arduino.h>
#include <micro_ros_arduino.h>
#include <stdio.h>

#include <rcl/rcl.h>
#include <rcl/error_handling.h>
#include <rclc/rclc.h>
#include <rclc/executor.h>

#include <nav_msgs/msg/odometry.h>
#include <sensor_msgs/msg/imu.h>
#include <sensor_msgs/msg/battery_state.h>
#include <sensor_msgs/msg/range.h>

#include <geometry_msgs/msg/twist.h>
#include <geometry_msgs/msg/vector3.h>

#include "config.h"
#include "motor.h"
#include "kinematics.h"
#include "pid.h"
#include "odometry.h"
#include "imu.h"
#define ENCODER_USE_INTERRUPTS
#define ENCODER_OPTIMIZE_INTERRUPTS
#include "encoder.h"

#define RCCHECK(fn) { rcl_ret_t temp_rc = fn; if((temp_rc != RCL_RET_OK)){rclErrorLoop();}}
#define RCSOFTCHECK(fn) { rcl_ret_t temp_rc = fn; if((temp_rc != RCL_RET_OK)){}}

rcl_publisher_t odom_publisher;
rcl_publisher_t imu_publisher;
rcl_publisher_t battstate_publisher;
rcl_subscription_t twist_subscriber;
rcl_subscription_t battstate_subscriber;
rcl_publisher_t range_publisher;
rcl_subscription_t range_subscriber;

nav_msgs__msg__Odometry odom_msg;
sensor_msgs__msg__Imu imu_msg;
sensor_msgs__msg__BatteryState battstate_msg;
geometry_msgs__msg__Twist twist_msg;
sensor_msgs__msg__Range range_msg;

rclc_executor_t executor;
rclc_support_t support;
rcl_allocator_t allocator;
rcl_node_t node;
rcl_timer_t control_timer;

unsigned long long time_offset = 0;
unsigned long prev_cmd_time = 0;
unsigned long prev_odom_update = 0;
bool micro_ros_init_successful = false;

Encoder motor1_encoder(MOTOR1_ENCODER_A, MOTOR1_ENCODER_B, COUNTS_PER_REV1, MOTOR1_ENCODER_INV);
Encoder motor2_encoder(MOTOR2_ENCODER_A, MOTOR2_ENCODER_B, COUNTS_PER_REV2, MOTOR2_ENCODER_INV);
Encoder motor3_encoder(MOTOR3_ENCODER_A, MOTOR3_ENCODER_B, COUNTS_PER_REV3, MOTOR3_ENCODER_INV);
Encoder motor4_encoder(MOTOR4_ENCODER_A, MOTOR4_ENCODER_B, COUNTS_PER_REV4, MOTOR4_ENCODER_INV);

Motor motor1_controller(PWM_FREQUENCY, PWM_BITS, MOTOR1_INV, MOTOR1_PWM, MOTOR1_IN_A, MOTOR1_IN_B);
Motor motor2_controller(PWM_FREQUENCY, PWM_BITS, MOTOR2_INV, MOTOR2_PWM, MOTOR2_IN_A, MOTOR2_IN_B);
Motor motor3_controller(PWM_FREQUENCY, PWM_BITS, MOTOR3_INV, MOTOR3_PWM, MOTOR3_IN_A, MOTOR3_IN_B);
Motor motor4_controller(PWM_FREQUENCY, PWM_BITS, MOTOR4_INV, MOTOR4_PWM, MOTOR4_IN_A, MOTOR4_IN_B);

PID motor1_pid(PWM_MIN, PWM_MAX, K_P, K_I, K_D);
PID motor2_pid(PWM_MIN, PWM_MAX, K_P, K_I, K_D);
PID motor3_pid(PWM_MIN, PWM_MAX, K_P, K_I, K_D);
PID motor4_pid(PWM_MIN, PWM_MAX, K_P, K_I, K_D);

Kinematics kinematics(
    Kinematics::LINO_BASE, 
    MOTOR_MAX_RPM, 
    MAX_RPM_RATIO, 
    MOTOR_OPERATING_VOLTAGE, 
    MOTOR_POWER_MAX_VOLTAGE, 
    WHEEL_DIAMETER, 
    LR_WHEELS_DISTANCE
);

Odometry odometry;
IMU imu;

/*void battstate_msg_init(sensor_msgs::msg::BatteryState &battstate_msg, char *frame_id_name)
{   
    // Populate battery parameters.
    battstate_msg.header.frame_id          = frame_id_name;
    battstate_msg.design_capacity          = 2500;  // mAh
    battstate_msg.power_supply_status      = 2;     // discharging
    battstate_msg.power_supply_health      = 0;     // unknown
    battstate_msg.power_supply_technology  = 2;     // Lion
    battstate_msg.present                  = 1;     // battery present
    battstate_msg.location.data            = "Linorobot2";        // unit location
}      

void range_msg_init(sensor_msgs::msg::Range &range_msg, char *frame_id_name)
{    
    // Populate IR parameters.
    range_msg.radiation_type = sensor_msgs::msg::Range::INFRARED;
    range_msg.header.frame_id = frame_id_name;
    range_msg.field_of_view = 0.25;
    range_msg.min_range = 0.02;
    range_msg.max_range = 0.02;
}*/

void setup() 
{   
    //***********************************************************************************************
    pinMode(LED_PIN, OUTPUT);
    
    bool imu_ok = imu.init();
    if(!imu_ok)
    {
        while(1)
        {
            flashLED(3);
        }
    }
    micro_ros_init_successful = false;
    set_microros_transports();
    
    battstate_msg.header.frame_id.data          = "";
    battstate_msg.design_capacity          = 2500;  // mAh
    battstate_msg.power_supply_status      = 2;     // discharging
    battstate_msg.power_supply_health      = 0;     // unknown
    battstate_msg.power_supply_technology  = 2;     // Lion
    battstate_msg.present                  = 1;     // battery present
    battstate_msg.location.data            = "Linorobot2";        // unit location
    
   // range_msg.header.stamp = rospy.Time.now()
    char irframeid[] = "range_link";
    range_msg.radiation_type = 1;
    range_msg.header.frame_id = irframeid;
    range_msg.field_of_view = 0.2;
    //range_msg.min_range = -0.01;
    range_msg.max_range = 1.0;
    range_msg.min_range = range_msg.max_range;
    
    
}

void loop() 
{
    //*****************************************************************************************
    static unsigned long prev_connect_test_time;
    // check if the agent got disconnected at 10Hz
    if(millis() - prev_connect_test_time >= 100)
    {
        prev_connect_test_time = millis();
        // check if the agent is connected
        if(RMW_RET_OK == rmw_uros_ping_agent(10, 2))
        {
            // reconnect if agent got disconnected or haven't at all
            if (!micro_ros_init_successful) 
            {
                createEntities();
                
            } 
            
        } 
        else if(micro_ros_init_successful)
        {
            // stop the robot when the agent got disconnected
            fullStop();
            // clean up micro-ROS components
            destroyEntities();
        }
        
    }
    
    if(micro_ros_init_successful)
    {
        rclc_executor_spin_some(&executor, RCL_MS_TO_NS(10));
    }
    ReadBatt();
    ReadIr();
}

void ReadBatt() 
{
          
    // Battery status.
    double battRVoltage = 0.0;
    double battVoltage = 0.0;

    // Reset Power Supply Health.
    battstate_msg.power_supply_health = 0;
  
    // Populate battery state message.
  
    {
      // Read raw voltage from analog pin.
      int battRVoltage = analogRead(0);
    
      // Scale reading to full voltage.
      battVoltage = battRVoltage*K* batt_const;
 

      // Set current cell voltage to message.
      battstate_msg.voltage = (float)battVoltage;

      // Check if battery is attached.
      if (battstate_msg.voltage >= 2.0)
      {
        if (battstate_msg.voltage <= 3.2)
          battstate_msg.power_supply_health = 5; // Unspecified failure.
        battstate_msg.present = 1;
      }
      else
        battstate_msg.present = 0;
    }

    // Update battery health.
    if (battstate_msg.present)
    {
      battstate_msg.voltage = (float)battVoltage;
      float volt = battstate_msg.voltage;
      float low  = 3.2 * CELLS;
      float high = 4.2 * CELLS;
      battstate_msg.percentage = constrain((volt - low) / (high - low), 0.0, 1.0);    
    }
    else 
    {
      battstate_msg.voltage = 0.0;
      battstate_msg.percentage = 0.0;
    }
  
    // Update power supply health if not failed.
    if (battstate_msg.power_supply_health == 0 && battstate_msg.present)
    {
      if (battstate_msg.voltage > CELLS * 4.2)
        battstate_msg.power_supply_health = 4; // overvoltage
      else if (battstate_msg.voltage < CELLS * 3.0)
        battstate_msg.power_supply_health = 3; // dead
      else
        battstate_msg.power_supply_health = 1; // good 
    }
    
}

void ReadIr() {
  float ir;
  float irRead = digitalRead(irPin);
  
  if (irRead > 0)
  {
      ir = (INFINITY) ;
  	 // Serial.println(ir);
  }
  else
  { 
       ir = (-INFINITY) ;
  }
  range_msg.range = ir; 
}

void controlCallback(rcl_timer_t * timer, int64_t last_call_time) 
{
    RCLC_UNUSED(last_call_time);
    if (timer != NULL) 
    {
       moveBase();
       publishData();
    }
}

void twistCallback(const void * msgin) 
{
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));

    prev_cmd_time = millis();
}



void createEntities()
{
    allocator = rcl_get_default_allocator();
    //create init_options
    RCCHECK(rclc_support_init(&support, 0, NULL, &allocator));
    // create node
    RCCHECK(rclc_node_init_default(&node, "linorobot_base_node", "", &support));
    // create odometry publisher
    RCCHECK(rclc_publisher_init_default( 
        &odom_publisher, 
        &node,
        ROSIDL_GET_MSG_TYPE_SUPPORT(nav_msgs, msg, Odometry),
        "odom/unfiltered"
    ));
    // create IMU publisher
    RCCHECK(rclc_publisher_init_default( 
        &imu_publisher, 
        &node,
        ROSIDL_GET_MSG_TYPE_SUPPORT(sensor_msgs, msg, Imu),
        "imu/data"
    ));
    // create Batterystate publisher
    RCCHECK(rclc_publisher_init_default( 
        &battstate_publisher, 
        &node,
        ROSIDL_GET_MSG_TYPE_SUPPORT(sensor_msgs, msg, BatteryState),
        "BatteryState"
    ));
    // create Batterystate subscribe
    /*RCCHECK(rclc_subscription_init_default( 
        &battstate_subscriber, 
        &node,
        ROSIDL_GET_MSG_TYPE_SUPPORT(sensor_msgs, msg, BatteryState),
        "BatteryState"
    ));*/
    // create Range publisher
    RCCHECK(rclc_publisher_init_default( 
        &range_publisher, 
        &node,
        ROSIDL_GET_MSG_TYPE_SUPPORT(sensor_msgs, msg, Range),
        "Range"
    ));
    // create Range subscribe
    /*RCCHECK(rclc_subscription_init_default( 
        &range_subscriber, 
        &node,
        ROSIDL_GET_MSG_TYPE_SUPPORT(sensor_msgs, msg, Range),
        "Range"
    )); */   
    // create twist command subscriber
    RCCHECK(rclc_subscription_init_default( 
        &twist_subscriber, 
        &node,
        ROSIDL_GET_MSG_TYPE_SUPPORT(geometry_msgs, msg, Twist),
        "cmd_vel"
    ));
    // create timer for actuating the motors at 50 Hz (1000/20)
    const unsigned int control_timeout = 20;
    RCCHECK(rclc_timer_init_default( 
        &control_timer, 
        &support,
        RCL_MS_TO_NS(control_timeout),
        controlCallback
    ));
    executor = rclc_executor_get_zero_initialized_executor();
    RCCHECK(rclc_executor_init(&executor, &support.context, 2, & allocator));
    RCCHECK(rclc_executor_add_subscription(
        &executor, 
        &twist_subscriber, 
        &twist_msg, 
        &twistCallback, 
        ON_NEW_DATA
    ));
    RCCHECK(rclc_executor_add_timer(&executor, &control_timer));
    // synchronize time with the agent
    syncTime();
    digitalWrite(LED_PIN, HIGH);
    micro_ros_init_successful = true;
}

void destroyEntities()
{
    digitalWrite(LED_PIN, LOW);

    rcl_publisher_fini(&odom_publisher, &node);
    rcl_publisher_fini(&imu_publisher, &node);
    rcl_publisher_fini(&battstate_publisher, &node);
   // rcl_subscription_fini(&battstate_subscriber, &node);
    rcl_publisher_fini(&range_publisher, &node);
   // rcl_subscription_fini(&range_subscriber, &node);
    rcl_subscription_fini(&twist_subscriber, &node);
    rcl_node_fini(&node);
    rcl_timer_fini(&control_timer);
    rclc_executor_fini(&executor);
    rclc_support_fini(&support);

    micro_ros_init_successful = false;
}

void fullStop()
{
    twist_msg.linear.x = 0.0;
    twist_msg.linear.y = 0.0;
    twist_msg.angular.z = 0.0;

    motor1_controller.brake();
    motor2_controller.brake();
    motor3_controller.brake();
    motor4_controller.brake();
}

void moveBase()
{
    // brake if there's no command received, or when it's only the first command sent
    if(((millis() - prev_cmd_time) >= 200)) 
    {
        twist_msg.linear.x = 0.0;
        twist_msg.linear.y = 0.0;
        twist_msg.angular.z = 0.0;

        digitalWrite(LED_PIN, HIGH);
    }
    // get the required rpm for each motor based on required velocities, and base used
    Kinematics::rpm req_rpm = kinematics.getRPM(
        twist_msg.linear.x, 
        twist_msg.linear.y, 
        twist_msg.angular.z
    );

    // get the current speed of each motor
    float current_rpm1 = motor1_encoder.getRPM();
    float current_rpm2 = motor2_encoder.getRPM();
    float current_rpm3 = motor3_encoder.getRPM();
    float current_rpm4 = motor4_encoder.getRPM();

    // the required rpm is capped at -/+ MAX_RPM to prevent the PID from having too much error
    // the PWM value sent to the motor driver is the calculated PID based on required RPM vs measured RPM
    motor1_controller.spin(motor1_pid.compute(req_rpm.motor1, current_rpm1));
    motor2_controller.spin(motor2_pid.compute(req_rpm.motor2, current_rpm2));
    motor3_controller.spin(motor3_pid.compute(req_rpm.motor3, current_rpm3));
    motor4_controller.spin(motor4_pid.compute(req_rpm.motor4, current_rpm4));

    Kinematics::velocities current_vel = kinematics.getVelocities(
        current_rpm1, 
        current_rpm2, 
        current_rpm3, 
        current_rpm4
    );

    unsigned long now = millis();
    float vel_dt = (now - prev_odom_update) / 1000.0;
    prev_odom_update = now;
    odometry.update(
        vel_dt, 
        current_vel.linear_x, 
        current_vel.linear_y, 
        current_vel.angular_z
    );
}

void publishData()
{
    odom_msg = odometry.getData();
    imu_msg = imu.getData();
    //battstate_msg = battstate.getData();

    struct timespec time_stamp = getTime();

    odom_msg.header.stamp.sec = time_stamp.tv_sec;
    odom_msg.header.stamp.nanosec = time_stamp.tv_nsec;

    imu_msg.header.stamp.sec = time_stamp.tv_sec;
    imu_msg.header.stamp.nanosec = time_stamp.tv_nsec;
    
    battstate_msg.header.stamp.sec = time_stamp.tv_sec;
    battstate_msg.header.stamp.nanosec = time_stamp.tv_nsec;
    
    range_msg.header.stamp.sec = time_stamp.tv_sec;
    range_msg.header.stamp.nanosec = time_stamp.tv_nsec;

    RCSOFTCHECK(rcl_publish(&imu_publisher, &imu_msg, NULL));
    RCSOFTCHECK(rcl_publish(&odom_publisher, &odom_msg, NULL));
    RCSOFTCHECK(rcl_publish(&battstate_publisher, &battstate_msg, NULL));
    RCSOFTCHECK(rcl_publish(&range_publisher, &range_msg, NULL));
}

void syncTime()
{
    // get the current time from the agent
    unsigned long now = millis();
    RCCHECK(rmw_uros_sync_session(10));
    unsigned long long ros_time_ms = rmw_uros_epoch_millis(); 
    // now we can find the difference between ROS time and uC time
    time_offset = ros_time_ms - now;
}

struct timespec getTime()
{
    struct timespec tp = {0};
    // add time difference between uC time and ROS time to
    // synchronize time with ROS
    unsigned long long now = millis() + time_offset;
    tp.tv_sec = now / 1000;
    tp.tv_nsec = (now % 1000) * 1000000;

    return tp;
}

void rclErrorLoop() 
{
    while(true)
    {
        flashLED(2);
    }
}

void flashLED(int n_times)
{
    for(int i=0; i<n_times; i++)
    {
        digitalWrite(LED_PIN, HIGH);
        delay(150);
        digitalWrite(LED_PIN, LOW);
        delay(150);
    }
    delay(1000);
}
