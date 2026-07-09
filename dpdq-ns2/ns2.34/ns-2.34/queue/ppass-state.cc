#include "ppass-state.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>

static class PPassStateClass : public TclClass {
public:
  PPassStateClass() : TclClass("Queue/PPassState") {}
  TclObject *create(int, const char *const *) { return (new PPassState()); }
} class_ppass_state;

PPassState::PPassState() : qlen_(0) , last_time_(0.0) {}

namespace {
bool parse_uint64(const char *text, uint64_t &value) {
  if (text == 0)
    return false;
  errno = 0;
  char *end = nullptr;
  unsigned long long parsed = strtoull(text, &end, 10);
  if (errno != 0 || end == text || (end && *end != '\0'))
    return false;
  value = static_cast<uint64_t>(parsed);
  return true;
}
} // namespace

int PPassState::command(int argc, const char *const *argv) {
  Tcl &tcl = Tcl::instance();

  if (argc == 2) {
    if (strcmp(argv[1], "get") == 0) {
      tcl.resultf("%llu", static_cast<unsigned long long>(qlen_));
      return TCL_OK;
    }
  } else if (argc == 3) {
    uint64_t value = 0;
    if (!parse_uint64(argv[2], value))
      return TCL_ERROR;

    if ((strcmp(argv[1], "reset") == 0) || (strcmp(argv[1], "set") == 0)) {
      reset_qlen(value);
      return TCL_OK;
    }
    if (strcmp(argv[1], "incr") == 0) {
      increase_qlen(value);
      tcl.resultf("%llu", static_cast<unsigned long long>(qlen_));
      return TCL_OK;
    }
    if (strcmp(argv[1], "decr") == 0) {
      decrease_qlen(value);
      tcl.resultf("%llu", static_cast<unsigned long long>(qlen_));
      return TCL_OK;
    }
  }

  return TclObject::command(argc, argv);
}

void PPassState::reset_qlen(uint64_t value) { qlen_ = value; }

void PPassState::increase_qlen(uint64_t delta) {
  if (delta > 0) {
    const uint64_t max = std::numeric_limits<uint64_t>::max();
    if (qlen_ > max - delta) {
      qlen_ = max;
      return;
    }
    qlen_ += delta;
  }
}

void PPassState::decrease_qlen(uint64_t delta) {
  if (delta >= qlen_) {
    qlen_ = 0;
  } else {
    qlen_ -= delta;
  }
}

void PPassState::set_lasttime(double t) { last_time_ = t; }

double PPassState::get_lasttime() { return last_time_; }
