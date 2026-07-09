#include "workload-distr.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <fstream>
#include <algorithm>
#include <cstring> // for strerror

// executable must be run from nwsim/scripts/r2p2/coord. FIXME
std::string cdf_path = "./config/homa-size-distributions";

/**
 * @brief Construct a new Manual Distr:: Manual Distr object
 * For manual request distributions, the file to be read is expected as follows:
 * Each row describes the actions of a different host.
 * Each row is comma-separated. The first item is the host address.
 * Each subsequent comma-separated item is "$relative_interval|$target_addr|$flow_sz"
 *   relative_interval (seconds) tells the simulator when to start the event compared to the previous one for the same host.
 *   the first interval is relative to the beginning of the simulation.
 *   target addr is the address of the target server
 *   flow_sz is the size of the flow in bytes
 *
 * Example:
 * 0,0.0001|2|200000,0.001|2|1000000
 * 1,0.0001|2|200000
 *
 * Explanation:
 * hosts 0 and 1 each send a 200KB flow to host 2 100 microsecnds after the beinning of the simulation.
 * host 0 sends a 1MB flow to host 2 1ms after the first flow started.
 *
 * @param intervals_file
 * @param machine_id
 * @param tuple_idx
 */
ManualDistr::ManualDistr(const char *intervals_file, int machine_id, size_t tuple_idx)
{
    std::fstream ef;
    ef.open(intervals_file);
    if (ef.fail())
    {
        std::cout << "Failed to open file: " << intervals_file << std::endl;
        std::cerr << "Error: " << strerror(errno) << std::endl;
        exit(1);
    }
    std::string line, word, value;
    while (std::getline(ef, line))
    {
        assert(line.find(",") != std::string::npos);
        assert(line.find("|") != std::string::npos);
        std::istringstream ss(line);
        size_t word_count = 0;
        while (std::getline(ss, word, ','))
        {
            if (word_count == 0)
            {
                if (stoi(word) != machine_id)
                {
                    break;
                }
            }
            else
            {
                std::istringstream ssw(word);
                for (size_t idx = 0; idx <= tuple_idx; idx++)
                    std::getline(ssw, value, '|');
                events_.push_back(stod(value));
            }
            word_count++;
        }
    }
    std::cout << tuple_idx << " Intervals/targets/sizes for machine " << machine_id << std::endl;
    for (double d : events_)
    {
        std::cout << d << " ";
    }
    std::cout << std::endl;
}

double ManualDistr::get_next()
{
    double res;
    if (!events_.empty())
    {

        res = events_.front();
        events_.erase(events_.begin());
    }
    else
    {
        // return an event in the far future that will never happen
        res = 1.0 * 1000 * 1000;
    }
    return res;
}

W1Distr::W1Distr(unsigned int seed, double avg_size) : EmpiricalDistr(seed)
{
    assert(0); // part of this distribution is analytically derived
    cdf_ = {
        {0.0, 1.0},
        {0.27, 3.1623},
        {0.35, 10.0},
        {0.5, 31.623},
        {0.63, 100.0},
        {0.83, 316.23},
        {0.96, 1000.0},
        {0.99453, 3162.3},
        {0.99971856, 10000.0},
        {0.99998841, 31623.0},
        {0.99999956, 100000.0},
        {0.9999999837, 316230.0},
        {1.0, 1000000.0}};
}

double W1Distr::get_next()
{
    return EmpiricalDistr::get_next(cdf_);
}

W2Distr::W2Distr(unsigned int seed, double avg_size) : EmpiricalDistr(seed)
{
    std::map<double, double> *this_cdf = load_cdf_from_file(cdf_path + std::string("/Google_SearchRPC.txt"));
    cdf_ = *this_cdf;
    delete this_cdf;
    assert(abs(avg_size - 440.7907) < 0.001);
}

double W2Distr::get_next()
{
    return EmpiricalDistr::get_next(cdf_);
}

W3Distr::W3Distr(unsigned int seed, double avg_size) : EmpiricalDistr(seed)
{
    std::map<double, double> *this_cdf = load_cdf_from_file(cdf_path + std::string("/Websearch.txt"));
    cdf_ = *this_cdf;
    delete this_cdf;
    assert(abs(avg_size - 2927.354) < 0.001);
}

double W3Distr::get_next()
{
    return EmpiricalDistr::get_next(cdf_);
}

W4Distr::W4Distr(unsigned int seed, double avg_size) : EmpiricalDistr(seed)
{
    // Facebook_HadoopDist_All (https://github.com/PlatformLab/HomaSimulation)
    // average = 121848 bytes
    std::map<double, double> *this_cdf = load_cdf_from_file(cdf_path + std::string("/Facebook_HadoopDist_All.txt"));
    cdf_ = *this_cdf;
    delete this_cdf;
    assert(abs(avg_size - 121848) < 0.001);
}

double W4Distr::get_next()
{
    return EmpiricalDistr::get_next(cdf_);
}

W5Distr::W5Distr(unsigned int seed, double avg_size) : EmpiricalDistr(seed)
{
    std::map<double, double> *this_cdf = load_cdf_from_file(cdf_path + std::string("/DCTCP_MsgSizeDist.txt"));
    cdf_ = *this_cdf;
    delete this_cdf;
    assert(abs(avg_size - 2515857.4) < 0.001);
    // hdr_homa homaPkt = hdr_homa();
    // homaPkt.pktType_var() = PktType::UNSCHED_DATA;
    // uint32_t maxDataBytesPerEthFrame = MAX_ETHERNET_PAYLOAD -
    //                                    IP_HEADER_SIZE - UDP_HEADER_SIZE; // 1472
    // uint32_t maxDataBytesPerPkt = maxDataBytesPerEthFrame -
    //                               homaPkt.headerSize(); // 1442
    for (auto it = cdf_.begin(); it != cdf_.end(); it++)
    {
        it->second = it->second * 1442;
    }
}

double W5Distr::get_next()
{
    return EmpiricalDistr::get_next(cdf_);
}

EmpiricalDistr::EmpiricalDistr(unsigned int seed)
{
    unif_ = new UnifDoubleDistr(seed);
}

std::map<double, double> *
EmpiricalDistr::load_cdf_from_file(std::string file)
{
    // "inspired" by workload-estimator.cc from https://github.com/PlatformLab/HomaSimulation
    std::ifstream distFileStream;
    std::string avg;
    std::string size_prob_str;

    distFileStream.open(file.c_str());
    if (distFileStream.fail())
    {
        std::cout << "Failed to open file: " << file << std::endl;
        exit(1);
    }
    getline(distFileStream, avg);

    std::map<double, double> *cdf = new std::map<double, double>();
    double prob;
    while (getline(distFileStream, size_prob_str))
    {
        double value;
        sscanf(size_prob_str.c_str(), "%lf %lf",
               &value, &prob);
        (*cdf)[prob] = value;
    }
    distFileStream.close();
    // std::cout << "Loaded file: " << file << std::endl;
    // for (auto it = cdf->begin(); it != cdf->end(); it++)
    // {
    //     std::cout << "|| " << it->first << " | " << it->second << std::endl;
    // }
    return cdf;
}

/**
 * Quick and dirty, see class description
 */
double EmpiricalDistr::get_next(std::map<double, double> cdf)
{
    double x = unif_->get_next();

    if (x <= 0.0)
        x += 0.00000000001;

    double x0 = 0.0;
    double x1 = 0.0;
    for (auto it = cdf.begin(); it != cdf.end(); it++)
    {
        if (x > it->first)
        {
            x0 = it->first;
        }
        else
        {
            x1 = it->first;
            break;
        }
    }

    double y0;
    double y1;
    try
    {
        y0 = cdf.at(x0);
        y1 = cdf.at(x1);
    }
    catch (const std::out_of_range &e)
    {
        std::cout << "Unable to find y0 or y1 with x0 and x1: " << x0 << " " << x1 << " x was: " << x << std::endl;
        throw;
    }

    assert(y0 != 0.0);
    assert(y1 != 0.0);
    assert((x1 - x0) != 0.0);
    return pow(10.0, log10(y0) + (x - x0) * (log10(y1) - log10(y0)) / (x1 - x0));
}

FixedDistr::FixedDistr(double value) : value_(value) {}

double FixedDistr::get_next()
{
    return value_;
}

ExpDistr::ExpDistr(double mean_interval, int seed) : mean_(mean_interval), dist_(1.0 / mean_interval)
{
    gen_.seed(seed);
}

double ExpDistr::get_next()
{
    return dist_(gen_);
}

UnifIntDistr::UnifIntDistr(int high, int seed) : dist_(0, high)
{
    gen_.seed(seed);
}

double UnifIntDistr::get_next()
{
    return dist_(gen_);
}

UnifTargetIntDistr::UnifTargetIntDistr(int high, std::vector<int32_t> *dst_ids, int seed) : distr_(UnifIntDistr(high, seed)),
                                                                                            dst_ids_(dst_ids) {}

/**
 * @brief Returns the address at a uniformly random index of dst_ids
 *
 * @param dst_ids
 * @return int32_t
 */
double UnifTargetIntDistr::get_next()
{
    size_t host_idx = distr_.get_next();
    assert(host_idx < dst_ids_->size());
    double ret;
    try
    {
        ret = dst_ids_->at(host_idx);
    }
    catch (const std::out_of_range &e)
    {
        std::cout << "Did not find server destination at index " << host_idx << std::endl;
        throw;
    }
    return ret;
}

UnifDoubleDistr::UnifDoubleDistr(unsigned int seed) : dist_(0.0, 1.0)
{
    gen_.seed(seed);
}

double UnifDoubleDistr::get_next()
{
    return dist_(gen_);
}

std::set<int32_t> IncastGenerator::client_addr_;
std::vector<bool> IncastGenerator::clients_to_burst_;
int32_t IncastGenerator::server_addr_ = -1;
size_t IncastGenerator::num_clients_ = 0;
size_t IncastGenerator::num_servers_ = 0;
size_t IncastGenerator::request_size_ = 0;
size_t IncastGenerator::incast_size_ = 0;
UnifIntDistr *IncastGenerator::client_distr_ = nullptr;
UnifIntDistr *IncastGenerator::server_distr_ = nullptr;
size_t IncastGenerator::queries_received_ = 0;
bool IncastGenerator::initialized_ = false;

void IncastGenerator::init(size_t num_clients, size_t num_servers, size_t req_size, size_t incast_size)
{
    if (initialized_)
        return;
    initialized_ = true;
    num_clients_ = num_clients;
    num_servers_ = num_servers;
    request_size_ = req_size;
    incast_size_ = incast_size;
    client_distr_ = new UnifIntDistr(num_clients - 1, 2);
    server_distr_ = new UnifIntDistr(num_servers - 1, 3);
    /* TODO: This will not work if client addresses are not contiguous and starting at 0 */
    clients_to_burst_.resize(num_clients);
    std::fill(clients_to_burst_.begin(), clients_to_burst_.end(), false);

    assert(request_size_ > 0);
    assert(incast_size_ > 0);
    assert(num_clients > 0);
    assert(num_servers > 0);
}

// TODO: Needed? Depends on addressing scheme
void IncastGenerator::register_client(int32_t client_addr)
{
    client_addr_.insert(client_addr);
}

void IncastGenerator::register_server(int32_t server_addr)
{
    server_addr_ = server_addr;
}

void IncastGenerator::generate_incast_members()
{
    /* Decide server */
    server_addr_ = server_distr_->get_next();
    // std::cout << "Decided server " << server_addr_ << std::endl;

    /* Decide clients */
    std::set<int> numbers_found;
    std::fill(clients_to_burst_.begin(), clients_to_burst_.end(), false); // reset
    // std::cout << "numbers_found.size(): " << numbers_found.size() << " incast_size_ " << incast_size_ << std::endl;

    while (numbers_found.size() < incast_size_)
    {
        int random_number = client_distr_->get_next();
        // std::cout << "Random number: " << random_number << " server_addr_: " << server_addr_ << std::endl;
        if (random_number == server_addr_)
        {
            continue;
        }
        auto ret = numbers_found.insert(random_number);
        if (ret.second)
        {
            // new elementserver_addr_
            clients_to_burst_[random_number] = true;
        }
    }
    // std::cout << "==== Clients ====" << std::endl;
    // for (int client : numbers_found)
    // {
    //     std::cout << "Decided client " << client << std::endl;
    // }

    assert(numbers_found.size() == incast_size_);
}

IncastGenerator::BurstInfo IncastGenerator::should_send(int32_t my_address)
{
    if (queries_received_ == 0)
    {
        // First client that queries. Prepare new round
        generate_incast_members();
    }
    queries_received_++;

    bool send = false;
    try
    {
        send = clients_to_burst_.at(my_address);
    }
    catch (const std::exception &e)
    {
        std::cerr << "Could not find client at address " << my_address << std::endl;
        throw;
    }

    if (queries_received_ == num_clients_)
        queries_received_ = 0;

    return BurstInfo{.should_send_ = send, .incast_target_ = server_addr_, .req_size_ = request_size_};
}
