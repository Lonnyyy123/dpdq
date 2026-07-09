#include "r2p2-cc.h"

static class R2p2CCNoopClass : public TclClass {
 public:
	R2p2CCNoopClass() : TclClass("R2P2_CC_NOOP") {}
	TclObject* create(int, const char*const*) {
		return (new R2p2CCNoop);
	}
} class_r2p2_cc_noop;

R2p2CCNoop::R2p2CCNoop() {}

R2p2CCNoop::~R2p2CCNoop() {}
