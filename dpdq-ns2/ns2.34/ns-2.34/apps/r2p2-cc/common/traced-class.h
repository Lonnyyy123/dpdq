#ifndef ns_traced_class
#define ns_traced_class

#include <string>
class SimpleTracer;

class SimpleTracedClass
{
public:
    virtual void start_tracing(std::string file_path);
    virtual void stop_tracing();

protected:
    SimpleTracedClass();
    virtual ~SimpleTracedClass();
    bool do_trace_;
    SimpleTracer *tracer_;
    virtual void trace_state(std::string event);
};

#endif