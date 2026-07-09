#include "traced-class.h"
#include "simple-tracer.h"

SimpleTracedClass::SimpleTracedClass() : do_trace_(0), tracer_(new SimpleTracer()) {}

SimpleTracedClass::~SimpleTracedClass()
{
    delete tracer_;
}

void SimpleTracedClass::trace_state(std::string event)
{
    std::vector<std::string> vars;
    vars.push_back(event);
    tracer_->trace(vars);
}

void SimpleTracedClass::start_tracing(std::string file_path)
{
    tracer_->start_tracing(file_path);
    do_trace_ = true;
}

void SimpleTracedClass::stop_tracing()
{
    tracer_->stop_tracing();
    do_trace_ = false;
}