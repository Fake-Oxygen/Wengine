// rclcpp control-plane services (rmw TU).
//
// Built only with BUILD_ROS2=ON. Six uniform SigCommand services; every
// callback parses + validates (same strict parser as TCP/UART), enqueues
// the owned line on the driver's bus, and responds the enqueue verdict.
// try/catch around everything: a callback never throws into rclcpp, never
// touches state, never blocks waiting for execution.

#include "wengine/ros2/ros_control.hpp"

#include <functional>
#include <utility>

namespace wengine::ros2 {

RosControlService::RosControlService(rclcpp::Node &node, std::string_view ns, solvers::ControlBus &bus, const control::CensusCache *census)
    : node_(&node), bus_(&bus), census_(census) {
  channel_ = bus_->add_channel();
  const std::string_view services[6] = {control::kServiceAdd,  control::kServiceSet,
                                        control::kServiceStart, control::kServiceStop,
                                        control::kServiceDrop, control::kServiceList};
  for (std::size_t k = 0; k < 6; ++k) {
    try {
      const std::string name = control::service_name(ns, services[k]);
      // Absolute name (leading '/'): the node spins in the root namespace
      // so `ros2 service list` shows these verbatim.
      services_[k] = node_->create_service<Srv>(
          name.substr(1), [this, svc = services[k]](
                              const std::shared_ptr<Srv::Request> &req,
                              const std::shared_ptr<Srv::Response> &resp) { handle(svc, req, resp); });
      ++service_count_;
    } catch (...) {
      ok_ = false;
      services_[k].reset();
    }
  }
  if (service_count_ != services_.size()) {
    ok_ = false;
  }
}

void RosControlService::handle(std::string_view service, const std::shared_ptr<Srv::Request> &req, const std::shared_ptr<Srv::Response> &resp) noexcept {
  control::ServiceOutcome out{false, "ERR internal service failed", 4};
  try {
    if (!ok_ || census_ == nullptr) {
      out = control::outcome_error({core::StatusCode::kInternal}, "service unavailable");
    } else if (service == control::kServiceList) {
      std::string line;
      const core::Status st = control::validate_service_line(service, req->line, line);
      if (!st.ok()) {
        out = control::outcome_error(st, "LIST takes no arguments");
        ++refused_;
      } else if (census_->lines.empty()) {
        out = control::outcome_error({core::StatusCode::kInternal}, "no census yet");
        ++refused_;
      } else {
        out = {true, control::census_text(*census_), 0};
      }
    } else {
      std::string line;
      const core::Status st = control::validate_service_line(service, req->line, line);
      if (!st.ok()) {
        const std::string_view what = (st.code == core::StatusCode::kNotImplemented) ? "table/lua need bulk payloads" : "bad arguments";
        out = control::outcome_error(st, what);
        ++refused_;
      } else if (!bus_->enqueue(channel_, std::move(line))) {
        out = control::outcome_error({core::StatusCode::kInternal}, "enqueue failed");
        ++refused_;
      } else {
        out = control::outcome_enqueued();
        ++enqueued_;
      }
    }
  } catch (...) {
    out = control::outcome_error({core::StatusCode::kInternal}, "service failed");
    ++refused_;
  }
  try {
    resp->ok = out.ok;
    resp->message = std::move(out.message);
    resp->code = out.code;
  } catch (...) {
    // Response assignment must not throw out of a service callback.
  }
}

} // namespace wengine::ros2
