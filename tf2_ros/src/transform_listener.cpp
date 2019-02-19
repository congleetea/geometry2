/*
 * Copyright (c) 2008, Willow Garage, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Willow Garage, Inc. nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/** \author Tully Foote */

#include "tf2_ros/transform_listener.h"


using namespace tf2_ros;

// 坐标转换监听器。需要带入一个保存转换关系的buffer，这个buffer会保存一段时间内的坐
// 标信息(默认10seconds)。spin_thread 默认 true, 会单独开一个线程.
// spin_thread = false 的时候，使用单线程订阅，一般通过 ros::spin() 或者 ros::spinOnce()
// 在 spin 中执行该节点的所有回调信息，这样就可能在一个节点订阅多中信息的时候，因为只能同时
// 处理一个订阅而导致其他订阅信息被阻塞。因此可以通过 spin_thread = true，使用多线程回调。
// 这样就保证了消息的流畅。
// 当实例化该对象的时候，会自动监听"/tf"话题，然后处理之后按照时间排序保存起来。
TransformListener::TransformListener(tf2::BufferCore& buffer, bool spin_thread):
  dedicated_listener_thread_(NULL), buffer_(buffer), using_dedicated_thread_(false)
{
  if (spin_thread)
    initWithThread();
  else
    init();
}

TransformListener::TransformListener(tf2::BufferCore& buffer, const ros::NodeHandle& nh, bool spin_thread)
: dedicated_listener_thread_(NULL)
, node_(nh)
, buffer_(buffer)
, using_dedicated_thread_(false)
{
  if (spin_thread)
    initWithThread();
  else
    init();
}


TransformListener::~TransformListener()
{
  using_dedicated_thread_ = false;
  if (dedicated_listener_thread_)
  {
    dedicated_listener_thread_->join();
    delete dedicated_listener_thread_;
  }
}

/**
 * @brief 如果 spin_thread = false，那么只要该节点订阅 "/tf" 和 "/tf_static" 即可。 
 *             收到 broadcaster 广播出来的消息之后，执行回调函数。回调函数中会处理这个坐标消息。
 */
void TransformListener::init()
{
  message_subscriber_tf_ = node_.subscribe<tf2_msgs::TFMessage>("/tf", 100, boost::bind(&TransformListener::subscription_callback, this, _1)); ///\todo magic number
  message_subscriber_tf_static_ = node_.subscribe<tf2_msgs::TFMessage>("/tf_static", 100, boost::bind(&TransformListener::static_subscription_callback, this, _1)); ///\todo magic number
}

/**
 * @brief 如果 spin_thread = true, 则启动一个专门的线程, 使用多线程订阅。
 *        专用线程会一直循环执行tf_message_callback_queue_.callAvailable(ros::WallDuration(0.01));
 *        从队列中取出当前所有消息来处理，(还有一个借口callOne()则值调用队列中最早的回调).
 *
 *        具体执行的就是 subscription_callback 和 static_subscription_callback.
 */
void TransformListener::initWithThread()
{
  using_dedicated_thread_ = true;
  ros::SubscribeOptions ops_tf = ros::SubscribeOptions::create<tf2_msgs::TFMessage>("/tf", 100,
                         boost::bind(&TransformListener::subscription_callback, this, _1),
                         ros::VoidPtr(),
                         &tf_message_callback_queue_); ///\todo magic number
  message_subscriber_tf_ = node_.subscribe(ops_tf);
  
  ros::SubscribeOptions ops_tf_static = ros::SubscribeOptions::create<tf2_msgs::TFMessage>("/tf_static", 100,
                         boost::bind(&TransformListener::static_subscription_callback, this, _1),
                         ros::VoidPtr(),
                         &tf_message_callback_queue_); ///\todo magic number
  message_subscriber_tf_static_ = node_.subscribe(ops_tf_static);

  // 专用线程一直循环调用 tf_message_callback_queue_.callAvailable(ros::WallDuration(0.01));
  dedicated_listener_thread_ = new boost::thread(boost::bind(&TransformListener::dedicatedListenerThread, this));

  //Tell the buffer we have a dedicated thread to enable timeouts
  buffer_.setUsingDedicatedThread(true);
}


// 最终都是调用统一的 subscription_callback_impl。
void TransformListener::subscription_callback(const ros::MessageEvent<tf2_msgs::TFMessage const>& msg_evt)
{
  subscription_callback_impl(msg_evt, false);
}
void TransformListener::static_subscription_callback(const ros::MessageEvent<tf2_msgs::TFMessage const>& msg_evt)
{
  subscription_callback_impl(msg_evt, true);
}

void TransformListener::subscription_callback_impl(const ros::MessageEvent<tf2_msgs::TFMessage const>& msg_evt, bool is_static)
{
  ros::Time now = ros::Time::now();
  // 如果当前时间比之前最近的时间还早，说明时间出现跳变，保存这些到 buffer 就没有意义了。
  // 将 buffer_ 全部清除，重新来。
  if(now < last_update_){
    ROS_WARN_STREAM("Detected jump back in time of " << (last_update_ - now).toSec() << "s. Clearing TF buffer.");
    buffer_.clear();
  }
  last_update_ = now;

  const tf2_msgs::TFMessage& msg_in = *(msg_evt.getConstMessage());
  std::string authority = msg_evt.getPublisherName(); // lookup the authority

  // 遍历vector: tf2_msgs::TFMessage 的所有转换消息。一次调用 buffer_ 来设置转换信息。 
  for (unsigned int i = 0; i < msg_in.transforms.size(); i++)
  {
    try
    {
      buffer_.setTransform(msg_in.transforms[i], authority, is_static);
    }
    
    catch (tf2::TransformException& ex)
    {
      ///\todo Use error reporting
      std::string temp = ex.what();
      ROS_ERROR("Failure to set recieved transform from %s to %s with error: %s\n", msg_in.transforms[i].child_frame_id.c_str(), msg_in.transforms[i].header.frame_id.c_str(), temp.c_str());
    }
  }
};





