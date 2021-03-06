#include <fstream>
#include <math.h>
#include <uWS/uWS.h>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>
#include "Eigen-3.3/Eigen/Core"
#include "Eigen-3.3/Eigen/QR"
#include "json.hpp"

#include "vehicle.h"
#include "road.h"
#include "lane.h"
#include "spline.h"
#include "egocar.h"

using namespace std;

// for convenience
using json = nlohmann::json;

// For converting back and forth between radians and degrees.
constexpr double pi() { return M_PI; }
double deg2rad(double x) { return x * pi() / 180; }
double rad2deg(double x) { return x * 180 / pi(); }

// starting lane position
int starting_lane = 1;

// lane width
double lane_width = 4.0;

// reference velocity in mph
double ref_vel = 0.0;

// max velocity
double max_velocity = 50.0;

// change lane threshold
int change_lane_threshold = 30;

// our self driving car
EgoCar car(max_velocity,starting_lane, change_lane_threshold);

// Checks if the SocketIO event has JSON data.
// If there is data the JSON object in string format will be returned,
// else the empty string "" will be returned.
string hasData(string s) {
  auto found_null = s.find("null");
  auto b1 = s.find_first_of("[");
  auto b2 = s.find_first_of("}");
  if (found_null != string::npos) {
    return "";
  } else if (b1 != string::npos && b2 != string::npos) {
    return s.substr(b1, b2 - b1 + 2);
  }
  return "";
}

double distance(double x1, double y1, double x2, double y2)
{
	return sqrt((x2-x1)*(x2-x1)+(y2-y1)*(y2-y1));
}
int ClosestWaypoint(double x, double y, vector<double> maps_x, vector<double> maps_y)
{

	double closestLen = 100000; //large number
	int closestWaypoint = 0;

	for(int i = 0; i < maps_x.size(); i++)
	{
		double map_x = maps_x[i];
		double map_y = maps_y[i];
		double dist = distance(x,y,map_x,map_y);
		if(dist < closestLen)
		{
			closestLen = dist;
			closestWaypoint = i;
		}

	}

	return closestWaypoint;

}

int NextWaypoint(double x, double y, double theta, vector<double> maps_x, vector<double> maps_y)
{

	int closestWaypoint = ClosestWaypoint(x,y,maps_x,maps_y);

	double map_x = maps_x[closestWaypoint];
	double map_y = maps_y[closestWaypoint];

	double heading = atan2( (map_y-y),(map_x-x) );

	double angle = abs(theta-heading);

	if(angle > pi()/4)
	{
		closestWaypoint++;
	}

	return closestWaypoint;

}

// Transform from Cartesian x,y coordinates to Frenet s,d coordinates
vector<double> getFrenet(double x, double y, double theta, vector<double> maps_x, vector<double> maps_y)
{
	int next_wp = NextWaypoint(x,y, theta, maps_x,maps_y);

	int prev_wp;
	prev_wp = next_wp-1;
	if(next_wp == 0)
	{
		prev_wp  = maps_x.size()-1;
	}

	double n_x = maps_x[next_wp]-maps_x[prev_wp];
	double n_y = maps_y[next_wp]-maps_y[prev_wp];
	double x_x = x - maps_x[prev_wp];
	double x_y = y - maps_y[prev_wp];

	// find the projection of x onto n
	double proj_norm = (x_x*n_x+x_y*n_y)/(n_x*n_x+n_y*n_y);
	double proj_x = proj_norm*n_x;
	double proj_y = proj_norm*n_y;

	double frenet_d = distance(x_x,x_y,proj_x,proj_y);

	//see if d value is positive or negative by comparing it to a center point

	double center_x = 1000-maps_x[prev_wp];
	double center_y = 2000-maps_y[prev_wp];
	double centerToPos = distance(center_x,center_y,x_x,x_y);
	double centerToRef = distance(center_x,center_y,proj_x,proj_y);

	if(centerToPos <= centerToRef)
	{
		frenet_d *= -1;
	}

	// calculate s value
	double frenet_s = 0;
	for(int i = 0; i < prev_wp; i++)
	{
		frenet_s += distance(maps_x[i],maps_y[i],maps_x[i+1],maps_y[i+1]);
	}

	frenet_s += distance(0,0,proj_x,proj_y);

	return {frenet_s,frenet_d};

}

// Transform from Frenet s,d coordinates to Cartesian x,y
vector<double> getXY(double s, double d, vector<double> maps_s, vector<double> maps_x, vector<double> maps_y)
{
	int prev_wp = -1;

	while(s > maps_s[prev_wp+1] && (prev_wp < (int)(maps_s.size()-1) ))
	{
		prev_wp++;
	}

	int wp2 = (prev_wp+1)%maps_x.size();

	double heading = atan2((maps_y[wp2]-maps_y[prev_wp]),(maps_x[wp2]-maps_x[prev_wp]));
	// the x,y,s along the segment
	double seg_s = (s-maps_s[prev_wp]);

	double seg_x = maps_x[prev_wp]+seg_s*cos(heading);
	double seg_y = maps_y[prev_wp]+seg_s*sin(heading);

	double perp_heading = heading-pi()/2;

	double x = seg_x + d*cos(perp_heading);
	double y = seg_y + d*sin(perp_heading);

	return {x,y};

}

int main() {
  uWS::Hub h;

  // Load up map values for waypoint's x,y,s and d normalized normal vectors
  vector<double> map_waypoints_x;
  vector<double> map_waypoints_y;
  vector<double> map_waypoints_s;
  vector<double> map_waypoints_dx;
  vector<double> map_waypoints_dy;

  // Waypoint map to read from
  string map_file_ = "../data/highway_map.csv";

  // The max s value before wrapping around the track back to 0
  double max_s = 6945.554;

  ifstream in_map_(map_file_.c_str(), ifstream::in);

  string line;
  while (getline(in_map_, line)) {
  	istringstream iss(line);
  	double x;
  	double y;
  	float s;
  	float d_x;
  	float d_y;
  	iss >> x;
  	iss >> y;
  	iss >> s;
  	iss >> d_x;
  	iss >> d_y;
  	map_waypoints_x.push_back(x);
  	map_waypoints_y.push_back(y);
  	map_waypoints_s.push_back(s);
  	map_waypoints_dx.push_back(d_x);
  	map_waypoints_dy.push_back(d_y);
  }



  h.onMessage([&map_waypoints_x,&map_waypoints_y,&map_waypoints_s,&map_waypoints_dx,&map_waypoints_dy](uWS::WebSocket<uWS::SERVER> ws, char *data, size_t length,
                     uWS::OpCode opCode) {
    // "42" at the start of the message means there's a websocket message event.
    // The 4 signifies a websocket message
    // The 2 signifies a websocket event
    //auto sdata = string(data).substr(0, length);
	//cout << sdata << endl;


    if (length && length > 2 && data[0] == '4' && data[1] == '2') {

      auto s = hasData(data);

      if (s != "") {
        auto j = json::parse(s);

        string event = j[0].get<string>();

        if (event == "telemetry") {
          // j[1] is the data JSON object

        	// Main car's localization Data

          // Previous path data given to the Planner
        	auto previous_path_x = j[1]["previous_path_x"];
        	auto previous_path_y = j[1]["previous_path_y"];
        	// Previous path's end s and d values
        	double end_path_s = j[1]["end_path_s"];
        	double end_path_d = j[1]["end_path_d"];

          car._x = j[1]["x"];
          car._y = j[1]["y"];
          car._s = j[1]["s"];
          car._d = j[1]["d"];
          car._yaw = j[1]["yaw"];
          car._v = j[1]["speed"];
          car._end_path_s = j[1]["end_path_s"];
          car._end_path_d = j[1]["end_path_d"];
          car._is_too_close = false;

        	// Sensor Fusion Data, a list of all other cars on the same side of the road.
        	auto sensor_fusion = j[1]["sensor_fusion"];

        	json msgJson;

    			auto prev_size = previous_path_x.size();
          Road road(3);

    			// SENSOR FUSION
    			for(int i = 0; i<sensor_fusion.size();i++)
    			{
    				// is car in my lane
            int id = sensor_fusion[i][0];
            float x = sensor_fusion[i][1];
    				float y =  sensor_fusion[i][2];
            float vx = sensor_fusion[i][3];
    				float vy =  sensor_fusion[i][4];
            float s = sensor_fusion[i][5];
    				float d =  sensor_fusion[i][6];

            if(d>=0 && d<=12){
              int vehicle_lane_id = (int)(d/4.0)%3;

              Vehicle v(id,x,y,vx,vy,s,d);
              car.sense_vehicle(road,v, vehicle_lane_id);
            }
			   }

        //car.process_road_stats(road);
        car.drive(road);

        ref_vel = car._target_velocity;

  			// list of (x,y) waypoints evenly spaced at 30m
  			// these waypoints will be interpolated with a spline to fill it
  			// with more points that set the trajectory for the car

  			vector<double> ptsx;
  			vector<double> ptsy;

  			// reference state
  			double ref_x = car._x;
  			double ref_y = car._y;
  			double ref_yaw = deg2rad(car._yaw);

  			if(prev_size < 2){
  				// we need two points that make the path tangent to the car
  				// ideally the last 'straight' line in which the car was moving
  				double prev_car_x = car._x - cos(car._yaw);
  				double prev_car_y = car._y - sin(car._yaw);

  				ptsx.push_back(prev_car_x);
  				ptsx.push_back(car._x);

  				ptsy.push_back(prev_car_y);
  				ptsy.push_back(car._y);

  			}
			  else{
  				// redefine reference state based on previous path
  				ref_x = previous_path_x[prev_size-1];
  				ref_y = previous_path_y[prev_size-1];

  				double ref_x_previous = previous_path_x[prev_size-2];
  				double ref_y_previous = previous_path_y[prev_size-2];

  				ref_yaw = atan2(ref_y-ref_y_previous,ref_x-ref_x_previous);
          //ref_yaw -= 0,349066;//0.174533;

  				ptsx.push_back(ref_x_previous);
  				ptsx.push_back(ref_x);

  				ptsy.push_back(ref_y_previous);
  				ptsy.push_back(ref_y);
			}
  			// In Frenet coordinates add evenly spaced 30m points ahead of starting reference
  			vector<double> next_wp0 = getXY(car._s+50,2+4*car._lane,map_waypoints_s,map_waypoints_x,map_waypoints_y);
  			vector<double> next_wp1 = getXY(car._s+80,2+4*car._lane,map_waypoints_s,map_waypoints_x,map_waypoints_y);
  			vector<double> next_wp2 = getXY(car._s+90,2+4*car._lane,map_waypoints_s,map_waypoints_x,map_waypoints_y);

  			ptsx.push_back(next_wp0[0]);
  			ptsx.push_back(next_wp1[0]);
  			ptsx.push_back(next_wp2[0]);

  			ptsy.push_back(next_wp0[1]);
  			ptsy.push_back(next_wp1[1]);
  			ptsy.push_back(next_wp2[1]);

			 for(int i =0;i<ptsx.size(); i++){
  				// refocus the car to make car yaw angle 0 degrees
  				double shift_x = ptsx[i] - ref_x;
  				double shift_y = ptsy[i] - ref_y;

  				ptsx[i] = (shift_x * cos(0-ref_yaw) - shift_y * sin(0-ref_yaw));
  				ptsy[i] = (shift_x * sin(0-ref_yaw) + shift_y * cos(0-ref_yaw));
			}

			// the spline
			tk::spline s;

			// spline points
			s.set_points(ptsx,ptsy);

			// actual (x,y) points
			vector<double> next_x_vals;
			vector<double> next_y_vals;

			// start with previous path points from last iteration
			for(int i=0;i<previous_path_x.size();i++){
				next_x_vals.push_back(previous_path_x[i]);
				next_y_vals.push_back(previous_path_y[i]);
			}

			double target_x = 30;
			double target_y = s(target_x); //y value for target_x
			double target_dist = sqrt((target_x*target_x) + (target_y*target_y));

			double x_add_on = 0;

			// cout<<"Size: "<< 50 - previous_path_x.size()<<endl<<endl;
			for(int i = 1; i < 50 - previous_path_x.size() + 1;i++){
				double N = (target_dist/(0.02*ref_vel/2.24)); // m/s to mph = 2.24
				double x_point = x_add_on+(target_x)/N;
				double y_point = s(x_point);

				x_add_on = x_point;

				double x = x_point;
				double y = y_point;

				// rotation back to normal position
				x_point = (x*cos(ref_yaw) - y*sin(ref_yaw));
				y_point = (x*sin(ref_yaw) + y*cos(ref_yaw));

				x_point += ref_x;
				y_point += ref_y;

				next_x_vals.push_back(x_point);
				next_y_vals.push_back(y_point);
			}

          	msgJson["next_x"] = next_x_vals;
          	msgJson["next_y"] = next_y_vals;

          	auto msg = "42[\"control\","+ msgJson.dump()+"]";

          	//this_thread::sleep_for(chrono::milliseconds(1000));
          	ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);

        }
      } else {
        // Manual driving
        std::string msg = "42[\"manual\",{}]";
        ws.send(msg.data(), msg.length(), uWS::OpCode::TEXT);
      }
    }
  });

  // We don't need this since we're not using HTTP but if it's removed the
  // program
  // doesn't compile :-(
  h.onHttpRequest([](uWS::HttpResponse *res, uWS::HttpRequest req, char *data,
                     size_t, size_t) {
    const std::string s = "<h1>Hello world!</h1>";
    if (req.getUrl().valueLength == 1) {
      res->end(s.data(), s.length());
    } else {
      // i guess this should be done more gracefully?
      res->end(nullptr, 0);
    }
  });

  h.onConnection([&h](uWS::WebSocket<uWS::SERVER> ws, uWS::HttpRequest req) {
    std::cout << "Connected!!!" << std::endl;
  });

  h.onDisconnection([&h](uWS::WebSocket<uWS::SERVER> ws, int code,
                         char *message, size_t length) {
    ws.close();
    std::cout << "Disconnected" << std::endl;
  });

  int port = 4567;
  if (h.listen(port)) {
    std::cout << "Listening to port " << port << std::endl;
  } else {
    std::cerr << "Failed to listen to port" << std::endl;
    return -1;
  }
  h.run();
}
