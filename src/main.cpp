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

//#include "utility.h"
#include "map.h"
#include "behavior.h"
#include "trajectory.h"
#include "cost.h"
#include "predictions.h"

#include "params.h"

#include <map>

using namespace std;

// for convenience
using json = nlohmann::json;


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


int main() {
  uWS::Hub h;

  //////////////////////////////////////////////////////////////////////
  Map map;

  if (PARAM_MAP_BOSCH == true) {
    map.read(map_bosch_file_);
  } else {
    map.read(map_file_);
  }

  map.plot();

  bool start = true;

  // car_speed: current speed
  // car_speed_target: speed at end of the planned trajectory
  double car_speed_target = 1.0; // mph (non 0 for XY spline traj generation to avoid issues)

  // keep track of previous s and d paths: to initialize for continuity the new trajectory
  TrajectorySD prev_path_sd;
  //////////////////////////////////////////////////////////////////////


  h.onMessage([&map, &car_speed_target, &start, &prev_path_sd](uWS::WebSocket<uWS::SERVER> ws, char *data, size_t length,
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

            CarData car;
            TrajectoryXY previous_path_xy;
          
        	// Main car's localization Data
          	car.x = j[1]["x"];
          	car.y = j[1]["y"];
          	car.s = j[1]["s"];
          	car.d = j[1]["d"];
          	car.yaw = j[1]["yaw"];
          	car.speed = j[1]["speed"];
          	car.speed_target = car_speed_target;

            cout << "SPEEDOMETER: car.speed=" << car.speed << " car.speed_target=" << car.speed_target << '\n';
          	// Previous path data given to the Planner
          	vector<double> previous_path_x = j[1]["previous_path_x"];
          	vector<double> previous_path_y = j[1]["previous_path_y"];
          	previous_path_xy.x_vals = previous_path_x;
          	previous_path_xy.y_vals = previous_path_y;

          	// Previous path's end s and d values 
          	double end_path_s = j[1]["end_path_s"];
          	double end_path_d = j[1]["end_path_d"];

          	// Sensor Fusion Data, a list of all other cars on the same side of the road.
            vector<vector<double>> sensor_fusion = j[1]["sensor_fusion"];

          	json msgJson;

            //////////////////////////////////////////////////////////////////////

            map.testError(car.x, car.y, car.yaw);

            int prev_size = previous_path_xy.x_vals.size();
            cout << "prev_size=" << prev_size << " car.x=" << car.x << " car.y=" << car.y << " car.s=" << 
                    car.s << " car.d=" << car.d << " car.speed=" << car.speed << " car.speed_target=" << car.speed_target << endl;

            vector<double> frenet_car = map.getFrenet(car.x, car.y, deg2rad(car.yaw));
            car.s = frenet_car[0];
            car.d = frenet_car[1];
            car.lane = get_lane(car.d);
            cout << "car.s=" << car.s << " car.d=" << car.d << endl;

            if (start) {
              TrajectoryJMT traj_jmt = JMT_init(car.s, car.d);
              prev_path_sd = traj_jmt.path_sd;
              start = false;
            }

            // --- 6 car predictions x 50 points x 2 coord (x,y): 6 objects predicted over 1 second horizon ---
            Predictions predictions = Predictions(sensor_fusion, car, PARAM_NB_POINTS /* 50 */);

            Behavior behavior = Behavior(sensor_fusion, car);
            vector<Target> targets = behavior.get_targets();

            // -- short time horizon (close to 100 msec when possible; not lower bcz of simulator latency) for trajectory (re)generation ---
            prev_size = min(prev_size, PARAM_TRUNCATED_PREV_SIZE);

            vector<Cost> costs;
            vector<TrajectoryXY> trajectories;
            vector<TrajectorySD> prev_paths_sd;

            for (size_t i = 0; i < targets.size(); i++) {
              TrajectoryXY trajectory;
              if (PARAM_TRAJECTORY_JMT) {
                TrajectoryJMT traj_jmt;

                // generate JMT trajectory in s and d: converted then to (x,y) for trajectory output
                traj_jmt = generate_trajectory_jmt(targets[i], map, previous_path_xy, prev_size, prev_path_sd);
                trajectory = traj_jmt.trajectory;
                prev_paths_sd.push_back(traj_jmt.path_sd);
              } else {
                // generate SPLINE trajectory in x and y
                trajectory = generate_trajectory(targets[i], map, car, previous_path_xy, prev_size);
              }

              Cost cost = Cost(trajectory, targets[i], predictions, car.lane);
              costs.push_back(cost);
              trajectories.push_back(trajectory);
            }

            // --- retrieve the lowest cost trajectory ---
            double min_cost = INF;
            int min_cost_index = 0;
            for (size_t i = 0; i < costs.size(); i++) {
              if (costs[i].get_cost() < min_cost) {
                min_cost = costs[i].get_cost();
                min_cost_index = i;
              }
            }
            int target_lane = targets[min_cost_index].lane;
            car_speed_target = targets[min_cost_index].velocity;
            car.speed_target = car_speed_target;
            if (PARAM_TRAJECTORY_JMT) {
              prev_path_sd = prev_paths_sd[min_cost_index];
            }

            if (target_lane != car.lane) {
              cout << "====================> CHANGE LANE: lowest cost for target " << min_cost_index << " = (target_lane=" << target_lane
                   << " target_vel=" << car.speed_target << " car.lane=" << car.lane << " cost="<< min_cost << ")" << endl;
            }


            //////////////////////////////////////////////////////////////////////


          	// TODO: define a path made up of (x,y) points that the car will visit sequentially every .02 seconds
          	msgJson["next_x"] = trajectories[min_cost_index].x_vals; //next_x_vals;
          	msgJson["next_y"] = trajectories[min_cost_index].y_vals; //next_y_vals;

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
