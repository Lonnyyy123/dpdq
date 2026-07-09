#ifndef ns_ppass_state_h
#define ns_ppass_state_h

#include <cstdint>

#include "config.h"
#include "tclcl.h"

class PPassState : public TclObject {
public:
  PPassState();
  int command(int argc, const char *const *argv) override;
  void reset_qlen(uint64_t value = 0);
  void increase_qlen(uint64_t delta);
  void decrease_qlen(uint64_t delta);
  double get_lasttime();
  void set_lasttime(double t);

  inline uint64_t qlen() const { return qlen_; }

private:
  uint64_t qlen_;
  double last_time_;
};

#endif /* ns_ppass_state_h */
