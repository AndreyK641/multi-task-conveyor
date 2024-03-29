// MultiThreadTask.cpp : This file contains the 'main' function. Program execution begins and ends there.
//

#include <iostream>
#include <chrono>
#include "MultiThreadTask.h"

using namespace multi_task_conveyor;

// define custom task class derrived from Task
class CalcTask : public Task
{
public:
    CalcTask(const JOBID jobid, const int val, double& res) : Task(jobid), m_val(val), m_res(res) {}

    // override process() function
    void process() override {
      
        m_res = 1;
        for (int i = 0; i < m_val; ++i)
            m_res *= 1.01;
    }

protected:
    const int m_val;
    double& m_res;
};

#define TASKS 100000
#define VAL 100000
// define custom job class derrived from Job
class CalcJob : public Job
{
public:
    CalcJob() : Job(), m_res{} {}

    // override process() function
    void process() override {

        for (int i = 0; i < TASKS; ++i)
            m_conveyor->emplace_task<CalcTask>(get_id(), VAL, std::ref(m_res[i]));
    }

    double m_res[TASKS];
};

int main()
{
    // create multitask object
    MultiTask mt;

    auto start = chrono::high_resolution_clock::now();

    // create and start new job
    CalcJob* myjob = mt.emplace_job<CalcJob>();

    // wait untill job is done
    myjob->wait_until_done();

    auto stop = chrono::high_resolution_clock::now();
    auto duration = chrono::duration_cast<chrono::microseconds>(stop - start);
    cout << "first pass duration: " << duration << "\n";

    start = chrono::high_resolution_clock::now();

    // start job again
    mt.restart_job(myjob->get_id());

    myjob->wait_until_done();

    stop = chrono::high_resolution_clock::now();
    duration = chrono::duration_cast<chrono::microseconds>(stop - start);

    cout << "second pass duration: " << duration << "\n";

    start = chrono::high_resolution_clock::now();

    double res[TASKS]{};
    for (int j = 0; j < TASKS; ++j)
    {
        res[j] = 1;
        for (int i = 0; i < VAL; ++i)
            res[j] *= 1.01;
    }

    stop = chrono::high_resolution_clock::now();

    duration = chrono::duration_cast<chrono::microseconds>(stop - start);

    for (int j = 0; j < TASKS; ++j)
    {
        cout << res[j] << "\r";
    }

    cout << "Single thread duration: " << duration;

    return 0;
}

