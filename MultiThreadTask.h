#pragma once
#include <deque>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <map>
#include <vector>
#include <algorithm>

using namespace std;

namespace multi_task_conveyor {

	class MultiTask;
	class Job;
	typedef Job* JOBID;

	class Job
	{
	public:
		Job() : m_conveyor(nullptr), m_running_tasks(0) {}
		virtual ~Job() {}

		// override this function to prepare and push tasks
		virtual void process() = 0;

		// override this function to implement some logic after all pushed tasks are done
		virtual void process_after_done() = 0;

		void set_conveyor(MultiTask* conveyor) { m_conveyor = conveyor; }
		MultiTask* get_conveyor() {	return m_conveyor; }

		JOBID get_id() { return static_cast<JOBID>(this); }

		bool is_done() { return m_is_done.test(); }

		void inc_task_count() { ++m_running_tasks; }
		void dec_task_count() { --m_running_tasks; m_running_tasks.notify_one(); }
		unsigned long long get_task_count() { return m_running_tasks.load(); }

		void wait_until_done() { m_is_done.wait(false); }

		void wait_until_all_tasks_done() { 
			unsigned long t = m_running_tasks;
			while (t) {
				m_running_tasks.wait(t);
				t = m_running_tasks;
			}

			process_after_done();

			m_is_done.test_and_set();
			m_is_done.notify_all();
		}

		void wait_until_all_tasks_pushed() { m_is_all_task_pushed.wait(false); }

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

	private:
		atomic_flag m_is_all_task_pushed;
		atomic_flag m_is_done;
		MultiTask* m_conveyor;
		atomic_ulong m_running_tasks;
	};

	class Task
	{
	public:
		Task(JOBID jobid) : m_jobid(jobid) {}
		virtual ~Task() {}

		JOBID get_id() { return m_jobid; }

		// override this function for processing logic
		virtual void process() = 0;

	private:
		const JOBID m_jobid;
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

			m_tasks_queue.push_back(unique_ptr<Task>());

			m_task_queue_mutex.unlock();

			m_new_task_cv.notify_all();

			for (auto& t : m_tasks)
				t.join();

			m_tasks.clear();
			m_tasks_queue.clear();
		}

		// tasks functions
		template<derived_from<Task> T>
		void push_task(unique_ptr<T>&& task)
		{
			unique_lock lk(m_task_queue_mutex);

			if (m_max_tasks && m_tasks_queue.size() == m_max_tasks)
				m_task_done.wait(lk, [this]() {return m_tasks_queue.size() < m_max_tasks; });

			task->get_id()->inc_task_count();
			m_tasks_queue.push_back(forward<unique_ptr<T>>(task));

			lk.unlock();

			m_new_task_cv.notify_all();
		}

		template<derived_from<Task> T, class... Args>
		void emplace_task(Args&& ...args)
		{
			push_task<T>(forward<unique_ptr<T>>(make_unique<T>(forward<Args>(args)...)));
		}

		// jobs functions
		template<derived_from<Job> T>
		T* push_job(unique_ptr<T>&& job)
		{
			auto job_ptr = job.get();

			JOBID jobid = static_cast<JOBID>(job_ptr);
			job->set_conveyor(this);

			unique_lock ul(m_job_map_mutex);
			if (auto [new_obj_itt, is_success] = m_jobs_map.try_emplace(jobid, forward<unique_ptr<T>>(job)); !is_success)
				return job_ptr;

			ul.unlock();

			thread job_thread(&MultiTask::process_job, this, job_ptr);
			job_thread.detach();

			return job_ptr;
		}

		template<derived_from<Job> T, class... Args>
		T* emplace_job(Args&& ...args)
		{
			return push_job<T>(forward<unique_ptr<T>>(make_unique<T>(forward<Args>(args)...)));
		}

		auto pop_job(const JOBID jobid)
		{
			unique_lock ul(m_job_map_mutex);
			auto node = m_jobs_map.extract(jobid);

			if (node.empty())
				return unique_ptr<remove_pointer_t<decltype(jobid)>>{};
			else
				return unique_ptr<remove_pointer_t<decltype(jobid)>>(static_cast<decltype(jobid)>(node.mapped().release()));
		}

		void restart_job(const JOBID jobid)
		{
			if (jobid == nullptr)
				return;

			jobid->reset();

			thread job_thread(&MultiTask::process_job, this, jobid);
			job_thread.detach();
		}

		bool check_job_is_done(const JOBID jobid)
		{
			if (jobid == nullptr)
				return true;

			return jobid->get_task_count() == 0;
		}

		void wait_job_done(const JOBID jobid)
		{
			if (jobid == nullptr)
				return;

			jobid->wait_until_done();
		}

		void process_task()
		{
			while (1)
			{
				unique_lock lk(m_task_queue_mutex);

				m_new_task_cv.wait(lk, [this]() {return m_tasks_queue.size() > 0; });

				auto& task_ref = m_tasks_queue.front();

				if (task_ref.get() == nullptr)
					break;

				auto task = move(task_ref);
				m_tasks_queue.pop_front();

				lk.unlock();

				task->process();

				task->get_id()->dec_task_count();

				m_task_done.notify_all();
			}
		}

		void process_job(Job* job)
		{
			job->process();

			job->set_all_tasks_pushed();

			job->wait_until_all_tasks_done();
		}

	private:

		// jobs map
		map<JOBID, unique_ptr<Job>> m_jobs_map;

		// syncronisation objects for jobs map
		mutex m_job_map_mutex;

		// max tasks quantity
		unsigned int m_max_tasks;

		// task executing threads
		vector<thread> m_tasks;

		// syncronisation objects for tasks queue
		mutex m_task_queue_mutex;
		condition_variable_any m_new_task_cv;
		condition_variable_any m_task_done;

		// tasks queue
		deque<unique_ptr<Task>> m_tasks_queue;
	};
}
