#include <iomanip>
#include "simple-tracer.h"
#include "scheduler.h"

SimpleTracer::SimpleTracer() {}

SimpleTracer::~SimpleTracer() {}

void SimpleTracer::start_tracing(std::string file_path)
{
    file_path_ = file_path;
    if (!s_file_path_to_file_stream.count(file_path))
    {
        s_file_path_to_file_stream[file_path] = std::ofstream(file_path);
    }
}

void SimpleTracer::stop_tracing()
{
    s_file_path_to_file_stream.at(file_path_).close();
}

void SimpleTracer::trace(const std::vector<std::string> &vals)
{
    std::ofstream &trace_file = s_file_path_to_file_stream.at(file_path_);
    trace_file << std::fixed << std::setprecision(9)
               << Scheduler::instance().clock() << " ";
    for (unsigned int i = 0; i < vals.size() - 1; ++i)
    {
        trace_file << std::fixed << std::setprecision(9)
                   << vals[i] << " ";
    }
    trace_file << std::fixed << std::setprecision(9)
               << vals[vals.size() - 1];
    trace_file << "\n";
    // trace_file.flush();
}

std::unordered_map<std::string, std::ofstream> SimpleTracer::s_file_path_to_file_stream;
