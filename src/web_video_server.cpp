/*********************************************************************
 *
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2014, Worcester Polytechnic Institute
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of the Worcester Polytechnic Institute nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 *********************************************************************/

#include <boost/foreach.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <vector>
#include <sensor_msgs/image_encodings.h>
#include <opencv2/opencv.hpp>

#include "web_video_server/web_video_server.h"
#include "web_video_server/jpeg_streamers.h"
#include "web_video_server/vp8_streamer.h"
#include "web_video_server/http_server/http_reply.hpp"

namespace web_video_server
{


static void ros_connection_logger(http_server::HttpServerRequestHandler forward,
				  const http_server::HttpRequest& request,
				  http_server::HttpConnectionPtr connection) {
  ROS_INFO_STREAM("Handling Request: " << request.uri);
  try {
    forward(request, connection);
  } catch(std::exception& e) {
    ROS_WARN_STREAM("Error Handling Request: " << e.what());
  }
}


WebVideoServer::WebVideoServer(ros::NodeHandle& nh, ros::NodeHandle& private_nh) :
  nh_(nh), image_transport_(nh),
  handler_group_(http_server::HttpReply::stock_reply(http_server::HttpReply::not_found)) {
  cleanup_timer_ = nh.createTimer(ros::Duration(0.5), boost::bind(&WebVideoServer::cleanup_inactive_streams, this));

  int port;
  private_nh.param("port", port, 8080);

  int server_threads;
  private_nh.param("server_threads", server_threads, 1);

  private_nh.param("ros_threads", ros_threads_, 2);

  stream_types_["mjpeg"] = boost::shared_ptr<ImageStreamerType>(new MjpegStreamerType());
  stream_types_["vp8"] = boost::shared_ptr<ImageStreamerType>(new Vp8StreamerType());
  stream_types_["h264"] = boost::shared_ptr<ImageStreamerType>(new LibavStreamerType("mp4", "libx264", "video/mp4"));

  handler_group_.addHandlerForPath("/", boost::bind(&WebVideoServer::handle_list_streams, this, _1, _2));
  handler_group_.addHandlerForPath("/stream", boost::bind(&WebVideoServer::handle_stream, this, _1, _2));
  handler_group_.addHandlerForPath("/stream_viewer", boost::bind(&WebVideoServer::handle_stream_viewer, this, _1, _2));
  handler_group_.addHandlerForPath("/snapshot", boost::bind(&WebVideoServer::handle_snapshot, this, _1, _2));

  server_.reset(new http_server::HttpServer("0.0.0.0", boost::lexical_cast<std::string>(port),
					    boost::bind(ros_connection_logger, handler_group_, _1, _2), server_threads));
}

WebVideoServer::~WebVideoServer() {
}

void WebVideoServer::spin() {
  server_->run();
  ROS_INFO("Waiting For connections");
  ros::MultiThreadedSpinner spinner(ros_threads_);
  spinner.spin();
  server_->stop();
}

void WebVideoServer::cleanup_inactive_streams(){
  boost::mutex::scoped_lock lock(subscriber_mutex_, boost::try_to_lock);
  if(lock) {
    typedef std::vector<boost::shared_ptr<ImageStreamer> >::iterator itr_type;
    itr_type new_end
      = std::remove_if(image_subscribers_.begin(), image_subscribers_.end(), boost::bind(&ImageStreamer::isInactive, _1));
    for(itr_type itr = new_end; itr < image_subscribers_.end(); ++ itr){
      ROS_INFO_STREAM("Removed Stream: " << (*itr)->getTopic());
    }
    image_subscribers_.erase(new_end,
			     image_subscribers_.end());
  }
}


void WebVideoServer::handle_stream(const http_server::HttpRequest& request,
				   http_server::HttpConnectionPtr connection) {
  std::string type = request.get_query_param_value_or_default("type", "mjpeg");
  if(stream_types_.find(type) != stream_types_.end()) {
    boost::shared_ptr<ImageStreamer> streamer = stream_types_[type]->create_streamer(request, connection, image_transport_);
    streamer->start();
    boost::mutex::scoped_lock lock(subscriber_mutex_);
    image_subscribers_.push_back(streamer);
  }
  else {
    http_server::HttpReply::stock_reply(http_server::HttpReply::not_found)(request, connection);
  }
}

void WebVideoServer::handle_snapshot(const http_server::HttpRequest& request,
				     http_server::HttpConnectionPtr connection) {
  boost::shared_ptr<ImageStreamer> streamer(new JpegSnapshotStreamer(request, connection, image_transport_));
  streamer->start();

  boost::mutex::scoped_lock lock(subscriber_mutex_);
  image_subscribers_.push_back(streamer);
}

void WebVideoServer::handle_stream_viewer(const http_server::HttpRequest& request,
					  http_server::HttpConnectionPtr connection) {
  std::string type = request.get_query_param_value_or_default("type", "mjpeg");
  if(stream_types_.find(type) != stream_types_.end()) {
    std::string topic = request.get_query_param_value_or_default("topic", "");

    http_server::HttpReply::builder(http_server::HttpReply::ok)
      .header("Connection", "close")
      .header("Server", "web_video_server")
      .header("Content-type", "text/html;")
      .write(connection);

    std::stringstream ss;
    ss << "<html><head><title>" << topic << "</title></head><body>";
    ss << "<h1>" << topic << "</h1>";
    ss << stream_types_[type]->create_viewer(request);
    ss << "</body></html>";
    connection->write(ss.str());
  }
  else {
    http_server::HttpReply::stock_reply(http_server::HttpReply::not_found)(request, connection);
  }
}

void WebVideoServer::handle_list_streams(const http_server::HttpRequest& request,
				      http_server::HttpConnectionPtr connection) {
  std::string image_message_type = ros::message_traits::datatype<sensor_msgs::Image>();
  std::string camera_info_message_type = ros::message_traits::datatype<sensor_msgs::CameraInfo>();

  ros::master::V_TopicInfo topics;
  ros::master::getTopics( topics );
  ros::master::V_TopicInfo::iterator it;
  std::vector<std::string> image_topics;
  std::vector<std::string> camera_info_topics;
  for( it = topics.begin(); it != topics.end(); ++it ) {
    const ros::master::TopicInfo& topic = *it;
    if (topic.datatype == image_message_type) {
	image_topics.push_back(topic.name);
    }
    else if (topic.datatype == camera_info_message_type) {
	camera_info_topics.push_back(topic.name);
    }
  }


  http_server::HttpReply::builder(http_server::HttpReply::ok)
    .header("Connection", "close")
    .header("Server", "web_video_server")
    .header("Cache-Control", "no-cache, no-store, must-revalidate, pre-check=0, post-check=0, max-age=0")
    .header("Pragma", "no-cache")
    .header("Content-type", "text/html;")
    .write(connection);

  connection->write("<html>"
		    "<head><title>ROS Image Topic List</title></head>"
		    "<body><h1>Available ROS Image Topics:</h1>");
  connection->write("<ul>");
  BOOST_FOREACH(std::string& camera_info_topic, camera_info_topics){
    if(boost::algorithm::ends_with(camera_info_topic, "/camera_info")){
	std::string base_topic = camera_info_topic.substr(0, camera_info_topic.size()-strlen("camera_info"));
	connection->write("<li>");
	connection->write(base_topic);
	connection->write("<ul>");
	std::vector<std::string>::iterator image_topic_itr = image_topics.begin();
	for ( ; image_topic_itr != image_topics.end(); ) {
	  if (boost::starts_with(*image_topic_itr, base_topic)) {
	    connection->write("<li><a href=\"/stream_viewer?topic=");
	    connection->write(*image_topic_itr);
	    connection->write("\">");
	    connection->write(image_topic_itr->substr(base_topic.size()));
	    connection->write("</a> (");
	    connection->write("<a href=\"/snapshot?topic=");
	    connection->write(*image_topic_itr);
	    connection->write("\">Snapshot</a>)");
	    connection->write("</li>");

	    image_topic_itr = image_topics.erase(image_topic_itr);
	  } else {
	    ++image_topic_itr;
	  }
	}
	connection->write("</ul>");
    }
    connection->write("</li>");
  }
  connection->write("</ul></body></html>");
}

}


int main(int argc, char** argv)
{
  ros::init(argc, argv, "web_video_server");

  ros::NodeHandle nh;
  ros::NodeHandle private_nh("~");

  web_video_server::WebVideoServer server(nh, private_nh);
  server.spin();

  return (0);
}

