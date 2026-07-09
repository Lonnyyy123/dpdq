#ifndef workload_distr_h
#define workload_distr_h

#include <vector>
#include <string>
#include <random>
#include <map>
#include <set>
#include <stdexcept>

class RndDistr
{
public:
    virtual ~RndDistr() {}
    virtual double get_next() = 0;
    virtual void set_mean(double new_mean) = 0;
};

/**
 * @brief Distribution, the intervals of which are explicitly specified in a file.
 * Not "random" but oh well.
 * The intervals file provided should be a csv. The first column of each row should be
 * the machine id. The other columns should be the intervals between events for the corresponding machine.
 * The first interval is relative to the beginning of the simulation.
 * At construction, this class will find the row that corresponds to machine_id and use it
 * to populate events_
 */
class ManualDistr : public RndDistr
{
public:
    ManualDistr(const char *intervals_file, int machine_id, size_t tuple_idx);
    virtual ~ManualDistr() {}
    double get_next() override;
    void set_mean(double new_mean) override{}; // no mean to set

protected:
    std::vector<double> events_;
};

class FixedDistr : public RndDistr
{
public:
    FixedDistr(double value);
    double get_next() override;
    void set_mean(double new_mean) override { value_ = new_mean; }

protected:
    double value_;
};

class ExpDistr : public RndDistr
{
public:
    ExpDistr(double mean, int seed);
    double get_next() override;
    void set_mean(double new_mean) override { mean_ = new_mean; }

protected:
    std::mt19937 gen_;
    std::exponential_distribution<double> dist_;
    double mean_;
};

class UnifIntDistr : public RndDistr
{
public:
    UnifIntDistr(int high, int seed);
    double get_next() override;
    void set_mean(double new_mean) override{}; // no mean to set

protected:
    std::mt19937 gen_;
    std::uniform_int_distribution<> dist_;
};

/**
 * @brief Converts (random) target index to target address
 *
 */
class UnifTargetIntDistr : public RndDistr
{
public:
    UnifTargetIntDistr(int high, std::vector<int32_t> *dst_ids, int seed);
    double get_next() override;
    void set_mean(double new_mean) override{}; // no mean to set

protected:
    UnifIntDistr distr_;
    std::vector<int32_t> *dst_ids_;
};

class UnifDoubleDistr;
/**
 * Sort of approximates a message size distibution with logarithimc x axis (size) by logarithmically
 * interpolating between given points (the curves drawn will be a straight line if the x axis is
 * logarithmic - y: cdf, x - size (log). Example HOMA figure 1)). The cdf expected is of the form:
 * {0: 3.1623, 0.0001: 10, 0.0002: 31.623, 0.01: 100, 0.1: 316.23, 0.6:1000, 0.68: 3162.3, 0.71:10000,\
 *       0.72: 31623, 0.89:100000, 0.95:316230, 0.97: 1000000, 0.99: 3162300, 1:10000000}
 */
class EmpiricalDistr : public RndDistr
{
public:
    EmpiricalDistr(unsigned int seed);
    virtual ~EmpiricalDistr() {}
    virtual double get_next() override { throw std::runtime_error("Not supported"); }
    virtual double get_next(std::map<double, double> cdf);
    virtual std::map<double, double> *load_cdf_from_file(std::string file);
    void set_mean(double new_mean) override{}; // no mean to set

protected:
    UnifDoubleDistr *unif_;
};

class W1Distr : public EmpiricalDistr
{
public:
    W1Distr(unsigned int seed, double avg_size);
    double get_next() override;

protected:
    std::map<double, double> cdf_;
};

class W2Distr : public EmpiricalDistr
{
public:
    W2Distr(unsigned int seed, double avg_size);
    double get_next() override;

protected:
    std::map<double, double> cdf_;
};

class W3Distr : public EmpiricalDistr
{
public:
    W3Distr(unsigned int seed, double avg_size);
    double get_next() override;

protected:
    std::map<double, double> cdf_;
};

class W4Distr : public EmpiricalDistr
{
public:
    W4Distr(unsigned int seed, double avg_size);
    double get_next() override;

protected:
    std::map<double, double> cdf_;
};

class W5Distr : public EmpiricalDistr
{
public:
    W5Distr(unsigned int seed, double avg_size);
    double get_next() override;

protected:
    std::map<double, double> cdf_; // prob -> size
};

/**
 * Returns values in [0,1)
 */
class UnifDoubleDistr : public RndDistr
{
public:
    UnifDoubleDistr(unsigned int seed);
    double get_next() override;
    void set_mean(double new_mean) override{}; // no mean to set

protected:
    std::mt19937 gen_;
    std::uniform_real_distribution<double> dist_;
};

/**
 * The goal of this class is to generate incast bursts like the ones described
 * in the HPCC paper.
 * The parameters of the generator are static and shared among all app instances
 * (=> there can be one such generator per simulation).
 * Likely not a good design but it makes TCL orchestration minimal..
 * Client applications register themselves calling register_client()
 */
class IncastGenerator : public RndDistr
{
public:
    /* Struct used to "atomically" return information to the app */
    struct BurstInfo
    {
        bool should_send_;
        int32_t incast_target_;
        size_t req_size_;
    };

    static void init(size_t num_clients, size_t num_servers, size_t req_size, size_t incast_size);
    static void register_client(int32_t client_addr);
    static void register_server(int32_t server_addr);
    /* Given the client's address, returns if this client should send in this round. Assumes all clients will call this function */
    static BurstInfo should_send(int32_t my_address);
    static size_t get_request_size() { return request_size_; }
    static int32_t get_incast_target() { return server_addr_; }

protected:
    static void generate_incast_members();
    static std::set<int32_t> client_addr_;
    static int32_t server_addr_;
    static std::vector<bool> clients_to_burst_; /* "bitmap" of the clients that will participate in the incast in this round */
    static size_t num_clients_;
    static size_t num_servers_;
    static size_t incast_size_;
    static size_t request_size_; /* Request size in bytes */
    static UnifIntDistr *client_distr_;
    static UnifIntDistr *server_distr_;
    static size_t queries_received_; /* Used to determine if all clients queried what they should do this round */
    static bool initialized_;

private:
    IncastGenerator() {}
};
#endif