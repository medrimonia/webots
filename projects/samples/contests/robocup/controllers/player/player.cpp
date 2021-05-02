// Copyright 1996-2021 Cyberbotics Ltd.
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

#include <assert.h>
#include <stdio.h>
#include <sys/time.h>

#ifdef _WIN32
#include <winsock.h>
typedef int socklen_t;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <google/protobuf/text_format.h>
#include "messages.pb.h"
#if GOOGLE_PROTOBUF_VERSION < 3006001
#define ByteSizeLong ByteSize
#endif

// #define TURBOJPEG 1
// It turns out that the libjpeg interface to turbojpeg runs faster than the native turbojpeg interface
// Alternatives to be considered: NVIDIA CUDA nvJPEG Encoder, Intel IPP JPEG encoder
#ifdef TURBOJPEG
#include <turbojpeg.h>
#else
#include <jpeglib.h>
#endif

#include <webots/Accelerometer.hpp>
#include <webots/Camera.hpp>
#include <webots/Gyro.hpp>
#include <webots/Motor.hpp>
#include <webots/Node.hpp>
#include <webots/PositionSensor.hpp>
#include <webots/Robot.hpp>
#include <webots/TouchSensor.hpp>

#include <chrono>

#include <opencv2/imgproc.hpp>

// teams are limited to a bandwdith of 100 MB/s from the server evaluated on a floating time window of 1000 milliseconds.
#define TEAM_QUOTA (1000 * 1024 * 1024)
#define RED 0
#define BLUE 1

using sc = std::chrono::steady_clock;
using time_point = std::chrono::time_point<sc>;
using duration = std::chrono::duration<double, std::milli>;

static fd_set rfds;
static int n_allowed_hosts;
static std::vector<std::string> allowed_hosts;

static bool set_blocking(int fd, bool blocking) {
#ifdef _WIN32
  unsigned long mode = blocking ? 0 : 1;
  return (ioctlsocket(fd, FIONBIO, &mode) == 0) ? true : false;
#else
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1)
    return false;
  flags = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
  return (fcntl(fd, F_SETFL, flags) == 0) ? true : false;
#endif
}

static void close_socket(int fd) {
#ifdef _WIN32
  closesocket(fd);
#else
  close(fd);
#endif
}

static bool send_all(int socket, const char *buffer, size_t length) {
  while (length > 0) {
    int i = send(socket, buffer, length, 0);
    if (i < 1)
      return false;
    buffer += i;
    length -= i;
  }
  return true;
}

static int accept_client(int server_fd) {
  int cfd;
  struct sockaddr_in client;
  socklen_t size = sizeof(struct sockaddr_in);
  cfd = accept(server_fd, (struct sockaddr *)&client, &size);
  if (cfd != -1) {
    struct hostent *client_info = gethostbyname((char *)inet_ntoa(client.sin_addr));
    bool allowed = false;
    for (int i = 0; i < n_allowed_hosts; i++) {
      if (std::string(client_info->h_name) == allowed_hosts[i]) {
        allowed = true;
        break;
      }
    }
    if (allowed) {
      printf("Accepted connection from %s.\n", client_info->h_name);
      send_all(cfd, "Welcome", 8);
    } else {
      printf("Refused connection from %s.\n", client_info->h_name);
      send_all(cfd, "Refused", 8);
      close_socket(cfd);
      cfd = -1;
    }
  }
  return cfd;
}

static int create_socket_server(int port) {
  int rc;
  int server_fd;
  struct sockaddr_in address;

#ifdef _WIN32
  WSADATA info;
  rc = WSAStartup(MAKEWORD(2, 2), &info);  // Winsock 2.2
  if (rc != 0) {
    fprintf(stderr, "Cannot initialize Winsock\n");
    return -1;
  }
#endif

  server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd == -1) {
    fprintf(stderr, "Cannot create socket\n");
    return -1;
  }
  memset(&address, 0, sizeof(struct sockaddr_in));
  address.sin_family = AF_INET;
  address.sin_port = htons((unsigned short)port);
  address.sin_addr.s_addr = INADDR_ANY;
  rc = bind(server_fd, (struct sockaddr *)&address, sizeof(struct sockaddr));
  if (rc == -1) {
    fprintf(stderr, "Cannot bind port %d\n", port);
    close_socket(server_fd);
    return -1;
  }
  if (listen(server_fd, 1) == -1) {
    fprintf(stderr, "Cannot listen for connections\n");
    close_socket(server_fd);
    return -1;
  }
  return server_fd;
}

static void encode_jpeg(const unsigned char *image, int width, int height, int quality, unsigned long *size,
                        unsigned char **buffer) {
#ifdef TURBOJPEG
  tjhandle compressor = tjInitCompress();
  tjCompress2(compressor, image, width, 0, height, TJPF_RGB, buffer, size, TJSAMP_444, quality, TJFLAG_FASTDCT);
  tjDestroy(compressor);
#else
  struct jpeg_compress_struct cinfo;
  struct jpeg_error_mgr jerr;
  JSAMPROW row_pointer[1];
  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_compress(&cinfo);
  cinfo.image_width = width;
  cinfo.image_height = height;
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, quality, TRUE);
  jpeg_mem_dest(&cinfo, buffer, size);
  jpeg_start_compress(&cinfo, TRUE);
  while (cinfo.next_scanline < cinfo.image_height) {
    row_pointer[0] = (unsigned char *)&image[cinfo.next_scanline * width * 3];
    jpeg_write_scanlines(&cinfo, row_pointer, 1);
  }
  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
#endif
}

static void free_jpeg(unsigned char *buffer) {
#ifdef TURBOJPEG
  tjFree(buffer);
#else
  free(buffer);
#endif
}

static void warn(SensorMeasurements &sensor_measurements, std::string text) {
  Message *message = sensor_measurements.add_messages();
  message->set_message_type(Message::WARNING_MESSAGE);
  message->set_text(text);
}

class PlayerServer {
public:
  PlayerServer(const std::vector<std::string> &allowed_hosts, int port, int player_id, int team, webots::Robot *robot) :
    allowed_hosts(allowed_hosts),
    port(port),
    player_id(player_id),
    team(team),
    client_fd(-1),
    controller_time(0),
    recv_index(0),
    recv_size(0),
    content_size(0),
    robot(robot) {
    basic_time_step = robot->getBasicTimeStep();
    printMessage("server started on port" + std::to_string(port));
    server_fd = create_socket_server(port);
    set_blocking(server_fd, false);
  }

  void step() {
    if (client_fd == -1) {
      client_fd = accept_client(server_fd);
      if (client_fd != -1) {
        set_blocking(client_fd, false);
        controller_time = 0;
      }
    } else {
      controller_time += basic_time_step;
      FD_ZERO(&rfds);
      FD_SET(client_fd, &rfds);
      struct timeval tv = {0, 0};
      auto start = sc::now();
      int retval = select(client_fd + 1, &rfds, NULL, NULL, &tv);
      auto after_select = sc::now();
      if (retval == -1) {
        perror("select()");
      } else {
        receiveMessages();
      }
      auto after_receive = sc::now();
      // Independently from if we received a message or not, send a message to the Controller
      prepareSensorMessage();
      auto after_prepare = sc::now();
      updateDevices();
      sendSensorMessage();
      auto after_send = sc::now();

      double step_time = duration(after_send - start).count();

      bool diagnose_time = step_time > budget_ms;

      if (benchmark_level >= 3 || diagnose_time) {
        benchmarkPrint("\tSelect time", after_select, start);
        benchmarkPrint("\tReceive time", after_receive, after_select);
        benchmarkPrint("\tPrepare time", after_prepare, after_receive);
        benchmarkPrint("\tSend time", after_send, after_prepare);
      }
      if (benchmark_level >= 2 || diagnose_time) {
        benchmarkPrint("Step time: ", after_send, start);
      }
    }
  }

  void receiveMessages() {
    while (client_fd != -1) {
      size_t bytes_received = 0;
      if (content_size == 0) {
        // If no content is available, receive header
        bytes_received = receiveData((char *)&content_size, sizeof(uint32_t));
        content_size = ntohl(content_size);
        recv_buffer = new char[content_size];
      } else {
        // If content is expected, read it and treat message if fully received
        bytes_received = receiveData(recv_buffer + recv_index, content_size - recv_index);
        recv_index += bytes_received;
        if (recv_index == content_size) {
          processBuffer();
        }
      }
      // If we consumed all data, stop trying to read
      if (bytes_received == 0)
        break;
    }
  }

  // Attempts to read up-to length from the client_fd, if data is missing, stops reading.
  // Returns the number of bytes read so far
  size_t receiveData(char *buffer, size_t length) {
    size_t received = 0;
    while (received < length) {
      int n = recv(client_fd, buffer, length - received, 0);
      if (n == -1) {
        if (errno != EAGAIN || errno != EWOULDBLOCK) {
          perror("recv()");
          printMessage("Unexpected failure while receiving data");
          close_socket(client_fd);
          client_fd = -1;
        }
        break;
      }
      if (n == 0) {
        printMessage("Client disconnected");
        close_socket(client_fd);
        client_fd = -1;
        break;
      }
      received += n;
    }
    return received;
  }

  void processBuffer() {
    ActuatorRequests actuatorRequests;
    actuatorRequests.ParseFromArray(recv_buffer, recv_index);
    // Reset buffer associated values
    recv_index = 0;
    content_size = 0;
    delete[] recv_buffer;
    // Processing actuatorRequests and adding warnings to the sensor message
    for (int i = 0; i < actuatorRequests.motor_positions_size(); i++) {
      const MotorPosition motorPosition = actuatorRequests.motor_positions(i);
      webots::Motor *motor = robot->getMotor(motorPosition.name());
      if (motor)
        motor->setPosition(motorPosition.position());
      else
        warn(sensor_measurements, "Motor \"" + motorPosition.name() + "\" not found, position command ignored.");
    }
    for (int i = 0; i < actuatorRequests.motor_velocities_size(); i++) {
      const MotorVelocity motorVelocity = actuatorRequests.motor_velocities(i);
      webots::Motor *motor = robot->getMotor(motorVelocity.name());
      if (motor)
        motor->setVelocity(motorVelocity.velocity());
      else
        warn(sensor_measurements, "Motor \"" + motorVelocity.name() + "\" not found, velocity command ignored.");
    }
    for (int i = 0; i < actuatorRequests.motor_forces_size(); i++) {
      const MotorForce motorForce = actuatorRequests.motor_forces(i);
      webots::Motor *motor = robot->getMotor(motorForce.name());
      if (motor)
        motor->setForce(motorForce.force());
      else
        warn(sensor_measurements, "Motor \"" + motorForce.name() + "\" not found, force command ignored.");
    }
    for (int i = 0; i < actuatorRequests.motor_torques_size(); i++) {
      const MotorTorque motorTorque = actuatorRequests.motor_torques(i);
      webots::Motor *motor = robot->getMotor(motorTorque.name());
      if (motor)
        motor->setTorque(motorTorque.torque());
      else
        warn(sensor_measurements, "Motor \"" + motorTorque.name() + "\" not found, torque command ignored.");
    }
    for (int i = 0; i < actuatorRequests.motor_pids_size(); i++) {
      const MotorPID motorPID = actuatorRequests.motor_pids(i);
      webots::Motor *motor = robot->getMotor(motorPID.name());
      if (motor)
        motor->setControlPID(motorPID.pid().x(), motorPID.pid().y(), motorPID.pid().z());
      else
        warn(sensor_measurements, "Motor \"" + motorPID.name() + "\" not found, PID command ignored.");
    }
    for (int i = 0; i < actuatorRequests.camera_qualities_size(); i++) {
      const CameraQuality cameraQuality = actuatorRequests.camera_qualities(i);
      webots::Camera *camera = robot->getCamera(cameraQuality.name());
      if (camera)
        warn(sensor_measurements, "CameraQuality is not yet implemented, ignored.");
      else
        warn(sensor_measurements, "Camera \"" + cameraQuality.name() + "\" not found, quality command ignored.");
    }
    for (int i = 0; i < actuatorRequests.camera_exposures_size(); i++) {
      const CameraExposure cameraExposure = actuatorRequests.camera_exposures(i);
      webots::Camera *camera = robot->getCamera(cameraExposure.name());
      if (camera)
        camera->setExposure(cameraExposure.exposure());
      else
        warn(sensor_measurements, "Camera \"" + cameraExposure.name() + "\" not found, exposure command ignored.");
    }
    // we need to enable the sensors after we sent the sensor value to avoid
    // sending values for disabled sensors.
    for (int i = 0; i < actuatorRequests.sensor_time_steps_size(); i++) {
      const SensorTimeStep sensorTimeStep = actuatorRequests.sensor_time_steps(i);
      webots::Device *device = robot->getDevice(sensorTimeStep.name());
      if (device) {
        const int sensor_time_step = sensorTimeStep.timestep();
        if (sensor_time_step) {
          if (sensors.count(device) == 0) {
            new_sensors.insert(device);
          }
        } else {
          sensors.erase(device);
        }
        if (sensor_time_step != 0 && sensor_time_step < basic_time_step)
          warn(sensor_measurements, "Time step for \"" + sensorTimeStep.name() + "\" should be greater or equal to " +
                                      std::to_string(basic_time_step) + ", ignoring " + std::to_string(sensor_time_step) +
                                      " value.");
        else if (sensor_time_step % basic_time_step != 0)
          warn(sensor_measurements, "Time step for \"" + sensorTimeStep.name() + "\" should be a multiple of " +
                                      std::to_string(basic_time_step) + ", ignoring " + std::to_string(sensor_time_step) +
                                      " value.");
        else
          switch (device->getNodeType()) {
            case webots::Node::ACCELEROMETER: {
              webots::Accelerometer *accelerometer = (webots::Accelerometer *)device;
              accelerometer->enable(sensor_time_step);
              break;
            }
            case webots::Node::CAMERA: {
              webots::Camera *camera = (webots::Camera *)device;
              camera->enable(sensor_time_step);
              break;
            }
            case webots::Node::GYRO: {
              webots::Gyro *gyro = (webots::Gyro *)device;
              gyro->enable(sensor_time_step);
              break;
            }
            case webots::Node::POSITION_SENSOR: {
              webots::PositionSensor *positionSensor = (webots::PositionSensor *)device;
              positionSensor->enable(sensor_time_step);
              break;
            }
            case webots::Node::TOUCH_SENSOR: {
              webots::TouchSensor *touchSensor = (webots::TouchSensor *)device;
              touchSensor->enable(sensor_time_step);
              break;
            }
            default:
              warn(sensor_measurements,
                   "Device \"" + sensorTimeStep.name() + "\" is not supported, time step command, ignored.");
          }
      } else
        warn(sensor_measurements, "Device \"" + sensorTimeStep.name() + "\" not found, time step command, ignored.");
    }
  }

  void prepareSensorMessage() {
    sensor_measurements.set_time(controller_time);
    struct timeval tp;
    gettimeofday(&tp, NULL);
    uint64_t real_time = tp.tv_sec * 1000 + tp.tv_usec / 1000;
    sensor_measurements.set_real_time(real_time);
    for (std::set<webots::Device *>::iterator it = sensors.begin(); it != sensors.end(); ++it) {
      webots::Accelerometer *accelerometer = dynamic_cast<webots::Accelerometer *>(*it);
      if (accelerometer) {
        if (controller_time % accelerometer->getSamplingPeriod())
          continue;
        AccelerometerMeasurement *measurement = sensor_measurements.add_accelerometers();
        measurement->set_name(accelerometer->getName());
        const double *values = accelerometer->getValues();
        Vector3 *vector3 = measurement->mutable_value();
        vector3->set_x(values[0]);
        vector3->set_y(values[1]);
        vector3->set_z(values[2]);
        continue;
      }
      webots::Camera *camera = dynamic_cast<webots::Camera *>(*it);
      if (camera) {
        if (controller_time % camera->getSamplingPeriod())
          continue;
        CameraMeasurement *measurement = sensor_measurements.add_cameras();
        const int width = camera->getWidth();
        const int height = camera->getHeight();
        measurement->set_name(camera->getName());
        measurement->set_width(width);
        measurement->set_height(height);
        measurement->set_quality(-1);  // raw image (JPEG compression not yet supported)
        auto img_start = sc::now();
        const unsigned char *rgba_image = camera->getImage();
        auto after_get = sc::now();
        cv::Mat rgba_mat(height, width, CV_8UC4);
        memcpy(rgba_mat.data, rgba_image, width * height * 4);
        auto after_memcpy = sc::now();
        cv::Mat rgb_mat;
        cv::cvtColor(rgba_mat, rgb_mat, cv::COLOR_BGRA2BGR);
        auto after_cvt_color = sc::now();
        std::cout << "Setting measurement with data size: " << (width * height * 3) << std::endl;
        measurement->set_image(rgb_mat.data, width * height * 3);
        auto after_set_image = sc::now();
        benchmarkPrint("camera->getImage", after_get, img_start);
        benchmarkPrint("memcpy", after_memcpy, after_get);
        benchmarkPrint("cvtColor", after_cvt_color, after_memcpy);
        benchmarkPrint("setImage", after_set_image, after_cvt_color);
        // const int rgb_image_size = width * height * 3;
        // unsigned char *rgb_image = new unsigned char[rgb_image_size];
        // for (int i = 0; i < width * height; i++) {
        //   rgb_image[3 * i] = rgba_image[4 * i];
        //   rgb_image[3 * i + 1] = rgba_image[4 * i + 1];
        //   rgb_image[3 * i + 2] = rgba_image[4 * i + 2];
        // }
        // measurement->set_image(rgb_image, rgb_image_size);
        // delete[] rgb_image;

        // testing JPEG compression (impacts the performance)
        // unsigned char *buffer = NULL;
        // long unsigned int bufferSize = 0;
        // encode_jpeg(rgba_image, width, height, 95, &bufferSize, &buffer);
        // free_jpeg(buffer);
        // buffer = NULL;
        continue;
      }
      webots::Gyro *gyro = dynamic_cast<webots::Gyro *>(*it);
      if (gyro) {
        if (controller_time % gyro->getSamplingPeriod())
          continue;
        GyroMeasurement *measurement = sensor_measurements.add_gyros();
        measurement->set_name(gyro->getName());
        const double *values = gyro->getValues();
        Vector3 *vector3 = measurement->mutable_value();
        vector3->set_x(values[0]);
        vector3->set_y(values[1]);
        vector3->set_z(values[2]);
        continue;
      }
      webots::PositionSensor *position_sensor = dynamic_cast<webots::PositionSensor *>(*it);
      if (position_sensor) {
        if (controller_time % position_sensor->getSamplingPeriod())
          continue;
        PositionSensorMeasurement *measurement = sensor_measurements.add_position_sensors();
        measurement->set_name(position_sensor->getName());
        measurement->set_value(position_sensor->getValue());
        continue;
      }
      webots::TouchSensor *touch_sensor = dynamic_cast<webots::TouchSensor *>(*it);
      if (touch_sensor) {
        if (controller_time % touch_sensor->getSamplingPeriod())
          continue;
        webots::TouchSensor::Type type = touch_sensor->getType();
        switch (type) {
          case webots::TouchSensor::BUMPER: {
            BumperMeasurement *measurement = sensor_measurements.add_bumpers();
            measurement->set_name(touch_sensor->getName());
            measurement->set_value(touch_sensor->getValue() == 1.0);
            continue;
          }
          case webots::TouchSensor::FORCE: {
            ForceMeasurement *measurement = sensor_measurements.add_forces();
            measurement->set_name(touch_sensor->getName());
            measurement->set_value(touch_sensor->getValue());
            continue;
          }
          case webots::TouchSensor::FORCE3D: {
            Force3DMeasurement *measurement = sensor_measurements.add_force3ds();
            measurement->set_name(touch_sensor->getName());
            const double *values = touch_sensor->getValues();
            Vector3 *vector3 = measurement->mutable_value();
            vector3->set_x(values[0]);
            vector3->set_y(values[1]);
            vector3->set_z(values[2]);
            continue;
          }
        }
      }
    }
  }

  void updateDevices() {
    for (webots::Device *d : new_sensors) {
      sensors.insert(d);
    }
    new_sensors.clear();
  }

  void sendSensorMessage() {
    uint32_t size = sensor_measurements.ByteSizeLong();
    if (bandwidth_usage(size) > TEAM_QUOTA) {
      sensor_measurements.Clear();
      Message *message = sensor_measurements.add_messages();
      message->set_message_type(Message::ERROR_MESSAGE);
      message->set_text(std::to_string(TEAM_QUOTA) + " MB/s quota exceeded.");
      size = sensor_measurements.ByteSizeLong();
      printMessage("Quota exceeded");
    }
    printMessage("Sending a message of size: " + std::to_string(size));
    char *output = new char[sizeof(uint32_t) + size];
    uint32_t *output_size = (uint32_t *)output;
    *output_size = htonl(size);
    sensor_measurements.SerializeToArray(&output[sizeof(uint32_t)], size);
    send_all(client_fd, output, sizeof(uint32_t) + size);
    delete[] output;
    sensor_measurements.Clear();
  }

  // this function updates the bandwith usage in the files quota-%d.txt and returns the total bandwith of the current time
  // window
  int bandwidth_usage(size_t new_packet_size) {
    static int *data_transferred = NULL;
    const int window_size = 1000 / basic_time_step;
    const int index = (controller_time / basic_time_step) % window_size;
    int sum = 0;
    char filename[32];
    if (data_transferred == NULL) {
      data_transferred = new int[window_size];
      for (int i = 0; i < window_size; i++)
        data_transferred[i] = 0;
    }
    data_transferred[index] = new_packet_size;
    snprintf(filename, sizeof(filename), "quota-%s-%d.txt", team == 0 ? "red" : "blue", player_id);
    FILE *fd = fopen(filename, "w");
    for (int i = 0; i < window_size; i++) {
      sum += data_transferred[i];
      fprintf(fd, "%d\n", data_transferred[i]);
    }
    fclose(fd);
    for (int i = 1; i < 5; i++) {
      if (i == player_id)
        continue;
      snprintf(filename, sizeof(filename), "quota-%s-%d.txt", team == 0 ? "red" : "blue", i);
      fd = fopen(filename, "r");
      if (fd == NULL)
        continue;
      while (!feof(fd)) {
        int v;
        if (fscanf(fd, "%d\n", &v) == 0)
          break;
        sum += v;
      }
      fclose(fd);
    }
    return sum;
  }

  void benchmarkPrint(const std::string &msg, const time_point &end, const time_point &start) {
    double elapsed_ms = duration(end - start).count();
    printMessage(msg + " " + std::to_string(elapsed_ms) + " ms");
  }

  void printMessage(const std::string &msg) {
    const char *team_name = team == RED ? "RED" : "BLUE";
    printf("%s %d: %s\n", team_name, player_id, msg.c_str());
  }

private:
  std::vector<std::string> allowed_hosts;
  int port;
  int player_id;
  int team;
  int server_fd;
  int client_fd;

  std::set<webots::Device *> sensors;
  // sensors that have just been added but that were previously disabled.
  // It's required to store them to avoid sending values of unitialized sensors
  std::set<webots::Device *> new_sensors;
  uint32_t controller_time;
  char *recv_buffer;
  int recv_index;
  int recv_size;
  int content_size;

  webots::Robot *robot;
  int basic_time_step;
  SensorMeasurements sensor_measurements;

  // 0: silent
  // 1: print global step cost and details if budget is exceeded
  // 2: additionally to 1: print global cost systematically
  // 3: print costs recap at each step
  // WARNING: any value higher than 1 significantly impacts simulation speed
  static int benchmark_level;
  // The allowed ms per step before producing a warning
  static double budget_ms;
};

int PlayerServer::benchmark_level = 1;
double PlayerServer::budget_ms = 1.0;

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fprintf(stderr, "Missing port argument");
    return 1;
  }
  const int port = atoi(argv[1]);
  n_allowed_hosts = argc - 2;
  for (int i = 0; i < n_allowed_hosts; i++) {
    allowed_hosts.push_back(argv[i + 2]);
  }
  webots::Robot *robot = new webots::Robot();
  const int basic_time_step = robot->getBasicTimeStep();
  const std::string name = robot->getName();
  int player_id = std::stoi(name.substr(name.find_last_of(' ') + 1));
  int player_team = name[0] == 'r' ? RED : BLUE;

  PlayerServer server(allowed_hosts, port, player_id, player_team, robot);

  std::set<webots::Device *> sensors;
  while (robot->step(basic_time_step) != -1) {
    server.step();
  }
  delete robot;
  return 0;
}
