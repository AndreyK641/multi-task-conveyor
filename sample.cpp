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
      
        m_res = 0.00001;
        std::this_thread::sleep_for(std::chrono::milliseconds(m_val));
    }

protected:
    const int m_val;
    double& m_res;
};

#define TASKS 1000
#define VAL 5
// define custom job class derrived from Job
class CalcJob : public Job
{
public:
    CalcJob(double &res) : Job(), m_res{}, res_sum(res) {}

    // override process() function
    void process() override {

        for (int i = 0; i < TASKS; ++i)
            get_conveyor()->emplace_task<CalcTask>(get_id(), VAL, std::ref(m_res[i]));
    }

    void process_after_done() override {
        res_sum = 0;
        for (int i = 0; i < TASKS; ++i)
            res_sum += m_res[i];
    }

    double &res_sum;
    double m_res[TASKS];
};

int main()
{
    // create multitask object
    MultiTask mt;

    auto start = chrono::high_resolution_clock::now();

    // create and start new job
    double res_sum = 0;
    CalcJob* myjob = mt.emplace_job<CalcJob>(ref(res_sum));

    // wait untill job is done
    mt.wait_job_done(myjob);

    auto stop = chrono::high_resolution_clock::now();
    auto duration = chrono::duration_cast<chrono::milliseconds>(stop - start);
    cout << "first pass duration: " << duration << "\n" << "result: " << res_sum << "\n";

    start = chrono::high_resolution_clock::now();

    // start job again
    mt.restart_job(myjob);

    mt.wait_job_done(myjob);

    stop = chrono::high_resolution_clock::now();
    duration = chrono::duration_cast<chrono::milliseconds>(stop - start);
    cout << "second pass duration: " << duration << "\n" << "result: " << res_sum << "\n";

    auto my_job = mt.pop_job(myjob);

    double res[TASKS]{};
    res_sum = 0;

    // start single thread calculations
    start = chrono::high_resolution_clock::now();

    for (int j = 0; j < TASKS; ++j)
    {
        res[j] = 0.00001;
        std::this_thread::sleep_for(std::chrono::milliseconds(VAL));
        res_sum += res[j];
    }


    stop = chrono::high_resolution_clock::now();
    duration = chrono::duration_cast<chrono::milliseconds>(stop - start);
    cout << "Single thread duration: " << duration << "\n" << "result: " << res_sum << "\n";

    return 0;
}

