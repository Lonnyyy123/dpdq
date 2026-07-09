#ifndef ns_simple_tracer_h
#define ns_simple_tracer_h

#include <string>
#include <iostream>
#include <fstream>
#include <vector>
#include <unordered_map>

/*
    Simple tracer instead of using ns-2's tracing.
    ns-2 is single-threaded.
    one file per class (or class group)
*/
class SimpleTracer
{
public:
    SimpleTracer();
    virtual ~SimpleTracer();
    void trace(const std::vector<std::string> &vals);
    void start_tracing(std::string);
    void stop_tracing();

private:
    // there can be one file stream per file
    static std::unordered_map<std::string, std::ofstream> s_file_path_to_file_stream;
    std::string file_path_;
};

#endif