#pragma once
#include <deque>
#include <thread>
#include <shared_mutex>
#include <condition_variable>
#include <atomic>
#include <map>
#include <vector>
#include <algorithm>

using namespace std;

class MultiTask;
class Job;
typedef Job* JOBID;

class Job
{
public:
	Job() : m_conveyor(nullptr) {}

	// main function where tasks are prepared and pushed 
	virtual void process() = 0;

	// callback function that is called after all pushed tasks are done
	virtual void process_after_done() {}

	void set_conveyor(MultiTask* conveyor)	{ m_conveyor = conveyor; }

	JOBID get_id() { return static_cast<JOBID>(this); }

	bool is_done() { return m_is_done.test(); }

	void wait_until_done() { m_is_done.wait(false); }

	void wait_until_all_tasks_pushed() { m_is_all_task_pushed.wait(false); }

	void set_done() 
	{ 
		m_is_done.test_and_set();
		m_is_done.notify_all();

		process_after_done();
	}

	void set_all_tasks_pushed()
	{
		m_is_all_task_pushed.test_and_set();
		m_is_all_task_pushed.notify_all();
	}

	void reset()
	{
		m_is_all_task_pushed.clear();
		m_is_done.clear();
	}

	virtual ~Job() {}

protected:
	atomic_flag m_is_all_task_pushed;
	atomic_flag m_is_done;
	MultiTask* m_conveyor;
};

class Task 
{
public:
	Task(JOBID jobid, bool is_terminator = false) : m_is_terminator(is_terminator), m_jobid(jobid) {}

	bool is_terminator() { return m_is_terminator; }
	JOBID get_id() { return m_jobid; }

	virtual void process() = 0;

	virtual ~Task() {}

protected:
	const bool m_is_terminator;
	const JOBID m_jobid;
};

class Terminator : public Task
{
public:
	Terminator() : Task(0, true) {}

	void process() override {}
};

class MultiTask
{
public:
	MultiTask(const unsigned int task_threads = 0, const unsigned int max_tasks = 0)
	{
		init(task_threads, max_tasks);
	}

	~MultiTask()
	{
		terminate();
	}

	void init(const unsigned int task_threads, const unsigned int max_tasks)
	{
		m_max_tasks = max_tasks;

		int task_threads_count = (task_threads == 0 ? thread::hardware_concurrency() - 1 : task_threads);
		if (!task_threads_count)
			++task_threads_count;

		// tasks init
		m_tasks.reserve(task_threads_count);
		for (int i = 0; i < task_threads_count; ++i)
			m_tasks.emplace_back(&MultiTask::process_task, this);
	}

	void terminate()
	{
		m_task_queue_mutex.lock();

		m_tasks_queue.clear();
		m_tasks_queue.push_back(make_unique<Terminator>());

		m_task_queue_mutex.unlock();

		m_new_task_cv.notify_all();

		for (auto& t : m_tasks)
			t.join();

		m_tasks.clear();
		m_tasks_queue.clear();
	}

	// tasks functions
	template<class T>
	void push_task(unique_ptr<T>&& task)
	{
		unique_lock lk(m_task_queue_mutex);

		if (m_max_tasks && m_tasks_queue.size() == m_max_tasks)
			m_task_done.wait(lk, [this]() {return m_tasks_queue.size() < m_max_tasks; });

		m_tasks_queue.push_back(forward<unique_ptr<T>>(task));

		lk.unlock();

		m_new_task_cv.notify_all();
	}

	template<class T, class... Args>
	void emplace_task(Args&& ...args)
	{
		push_task<T>( forward<unique_ptr<T>>(make_unique<T>(forward<Args>(args)...)) );
	}

	// jobs functions
	template<class T>
	T* push_job(unique_ptr<T>&& job)
	{
		unique_lock ul(m_job_map_mutex);

		auto job_ptr = job.get();

		JOBID jobid = static_cast<JOBID>(job_ptr);
		job->set_conveyor(this);

		auto [new_obj_itt, is_success] = m_jobs_map.insert(pair{ jobid, forward<unique_ptr<T>>(job) });

		if (!is_success)
			return 0;

		thread job_thread(&MultiTask::process_job, this, job_ptr);
		job_thread.detach();

		return job_ptr;
	}

	template<class T, class... Args>
	T* emplace_job(Args&& ...args)
	{
		return push_job<T>(forward<unique_ptr<T>>(make_unique<T>(forward<Args>(args)...)));
	}

	void restart_job(const JOBID jobid)
	{
		if (jobid == nullptr)
			return;

		shared_lock lk(m_job_map_mutex);
		
		jobid->reset();

		thread job_thread(&MultiTask::process_job, this, jobid);
		job_thread.detach();
	}

	bool check_job_is_done(const JOBID jobid)
	{
		if (jobid == nullptr)
			return true;

		shared_lock lk(m_task_queue_mutex);

		return is_job_tasks_queue_empty(jobid);
	}

	void wait_job_tasks_done(const JOBID jobid)
	{
		if (jobid == nullptr)
			return;

		shared_lock lk(m_task_queue_mutex);

		while (!is_job_tasks_queue_empty(jobid))
			m_task_done.wait(lk);
	}

	void process_task() 
	{
		while (1)
		{
			unique_lock lk(m_task_queue_mutex);

			if (!m_tasks_queue.size())
				m_new_task_cv.wait(lk, [this]() {return m_tasks_queue.size() > 0; });

			auto &task_ref = m_tasks_queue.front();

			if (task_ref->is_terminator())
				break;

			auto task = move(task_ref);
			m_tasks_queue.pop_front();

			lk.unlock();

			task->process();

			m_task_done.notify_all();
		} 
	}

	void process_job(Job* job)
	{
		job->process();

		job->set_all_tasks_pushed();

		wait_job_tasks_done(job->get_id());

		job->set_done();
	}

private:
	
	// jobs map
	map<JOBID, unique_ptr<Job>> m_jobs_map;

	// syncronisation objects for jobs map
	shared_mutex m_job_map_mutex;

	// max tasks quantity
	unsigned int m_max_tasks;

	// task executing threads
	vector<thread> m_tasks;

	// syncronisation objects for tasks queue
	shared_mutex m_task_queue_mutex;
	condition_variable_any m_new_task_cv;
	condition_variable_any m_task_done;

	// tasks queue
	deque<unique_ptr<Task>> m_tasks_queue;

	bool is_job_tasks_queue_empty(JOBID jobid) 
	{
		return none_of(m_tasks_queue.begin(), m_tasks_queue.end(), [jobid](const unique_ptr<Task>& elem) { return elem->get_id() == jobid; });
	}
};
