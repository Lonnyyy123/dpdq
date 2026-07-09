#ifndef ns_simple_log_h
#define ns_simple_log_h

#include <iostream>
#include <iomanip>
#include <string>
#include "scheduler.h"

template <class T>
static void log_internal(T t);

template <class T, class... Args>
static void log_internal(T t, Args... args);

static bool other_conditions(int32_t addr);

static void print_header(int debug_lvl);

const double START = 0.0;
// const double START = 10.0;
const double STOP = 99999999999.0;
// const int32_t ADDR_OF_INTEREST = -1;

// const double START = 10.000960710;
// const double STOP = 10.005240000;
const int32_t ADDR_OF_INTEREST = -1;

namespace slog
{

    /**
     * Indicative debug levels for code using this (value of debug_):
     *   0: Errors etc - always displayed
     *   1: Only important information that does not cause clutter
     *   2: Debug info that will not cause output regularly (eg for every request/pkt)
     *   3: Secondary debug info that will not cause output regularly (eg for every request/pkt)
     *   4: Debug info that will cause frequent output (eg for every request)
     *   5: Debug info that will cause very frequent output (eg for every packet)
     *   6: Debug info that will cause deterministcally very frequent output (eg polling/timers)
     *   7: Secondary debug info that will cause deterministcally very frequent output (eg polling/timers)
     */

    template <class T, class... Args>
    void error(int debug_lvl, T t, Args... args)
    {
        print_header(0);
        log_internal(t, args...);
    }

    template <class T, class... Args>
    void log1(int debug_lvl, T t, Args... args)
    {
        if (debug_lvl >= 1)
        {
            print_header(1);
            log_internal(t, args...);
        }
    }

    template <class T, class... Args>
    void log2(int debug_lvl, T t, Args... args)
    {
        if (debug_lvl >= 2)
        {
            print_header(2);
            log_internal(t, args...);
        }
    }

    template <class T, class... Args>
    void log3(int debug_lvl, T t, Args... args)
    {
        if (debug_lvl >= 3 && other_conditions(static_cast<int32_t>(t)))
        {
            print_header(3);
            log_internal(t, args...);
        }
    }

    template <class T, class... Args>
    void log4(int debug_lvl, T t, Args... args)
    {
        if (debug_lvl >= 4 && other_conditions(static_cast<int32_t>(t)))
        {
            print_header(4);
            log_internal(t, args...);
        }
    }

    template <class T, class... Args>
    void log5(int debug_lvl, T t, Args... args)
    {
        if (debug_lvl >= 5 && other_conditions(static_cast<int32_t>(t)))
        {
            print_header(5);
            log_internal(t, args...);
        }
    }

    template <class T, class... Args>
    void log6(int debug_lvl, T t, Args... args)
    {
        if (debug_lvl >= 6 && other_conditions(static_cast<int32_t>(t)))
        {
            print_header(6);
            log_internal(t, args...);
        }
    }

    template <class T, class... Args>
    void log7(int debug_lvl, T t, Args... args)
    {
        if (debug_lvl >= 7 && other_conditions(static_cast<int32_t>(t)))
        {
            print_header(7);
            log_internal(t, args...);
        }
    }
}

template <class T>
static void log_internal(T t)
{
    std::cout << t << std::endl;
}

template <class T, class... Args>
static void log_internal(T t, Args... args)
{
    std::cout << t << " ";
    log_internal(args...);
}

static void print_header(int debug_lvl)
{
    std::string lvl_viz = "";
    for (int i = 0; i < debug_lvl; i++)
    {
        lvl_viz.append("*");
    }
    std::cout << std::fixed << std::setprecision(9) << Scheduler::instance().clock() << " | " << lvl_viz << " ";
}

static bool other_conditions(int32_t addr)
{
    if (ADDR_OF_INTEREST == -1)
        return Scheduler::instance().clock() >= START && Scheduler::instance().clock() <= STOP;
    else
        return Scheduler::instance().clock() >= START && Scheduler::instance().clock() <= STOP && addr == ADDR_OF_INTEREST;
}

#endif
