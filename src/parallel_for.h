#ifndef PARALLEL_FOR_H
#define PARALLEL_FOR_H

#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <condition_variable>
#include <thread>
#include <queue>
#include <functional>
#include <iostream>
#include <assert.h>

#define ENABLE_PARALLEL_FOR 1
#define ENABLE_NESTING_PARALLEL_FOR 1

using parallel_for_fn = std::function<void(int)>;

// 通过condition_variable封装一个finish信号，用于当前thread等待所有子任务结束
class finish_signal {
public:
	void trigger()
	{
		std::unique_lock lock(mutex);
		triggered = true;
		cv.notify_all();
	}

	void wait()
	{
		std::unique_lock lock(mutex);
		cv.wait(lock, [this]{ return triggered; });
		triggered = false;
	}
private:
	std::mutex mutex;
	std::condition_variable cv;
	bool triggered = false;
};

struct parallel_for_data;
class task_t;
class parallel_for_executor;
class scheduler;

// 一个parallel_for中，所有worker共享的数据
struct alignas(64) parallel_for_data
{
	parallel_for_data(int start_index, int end_index, int step, int batch_size, int num_batches, int num_workers, const parallel_for_fn& callbody, finish_signal& finished_signal);

	std::atomic_int batch_item = 0;
	std::atomic_int incomplete_batches = 0;
	int start_index;							// 开始执行的index
	int end_index;								// 终止执行的index
	int step;									// 步长
	int batch_size;								// 每个batch执行多少次callbody
	int num_batches;							// batch总数
	parallel_for_fn callbody;					// 要运行的函数
	finish_signal& finished_signal;				// 结束信号
	std::vector<task_t> tasks;					// 单个parallel_for对应的所有task
};

// 每个task对应一个executor
class parallel_for_executor {
public:
	parallel_for_executor() = default;
	parallel_for_executor(std::shared_ptr<parallel_for_data>& data, int worker_index);

	bool operator()(const bool is_master = false);

	bool is_valid() { return !_data.expired() && worker_index > -1; }

	static void launch_task(std::shared_ptr<parallel_for_data>& data, int worker_index);

private:
	std::weak_ptr<parallel_for_data> _data;
	mutable int worker_index = -1;
};

// 单个task的封装，由scheduler调度，由worker执行
class task_t {
public:
	void init(parallel_for_executor&& runnable);	// TODO 后续可以兼容不同的executor

	void execute();

private:
	parallel_for_executor _runnable;
};

// 分配worker执行任务的scheduler
class scheduler {
public:
	scheduler(const scheduler &) = delete;

	scheduler& operator=(const scheduler &) = delete;

	static scheduler& instance();

	int get_number_of_workers(int num);

	void try_launch(task_t& task);

	void terminal();

private:
	scheduler();

	int num_worker;
	std::vector<std::thread> workers;
	std::queue<task_t> tasks;
	std::mutex _mutex;
	std::condition_variable cv_new_task;
	bool b_terminate;	// 终止运行
};

parallel_for_data::parallel_for_data(int start_index, int end_index, int step, int batch_size, int num_batches, int num_workers, const parallel_for_fn& callbody, finish_signal& finished_signal)
: start_index(start_index), end_index(end_index), step(step), batch_size(batch_size), num_batches(num_batches), callbody(callbody), finished_signal(finished_signal)
{
	incomplete_batches.store(num_batches);
	tasks = std::move(std::vector<task_t>(num_workers));
}

parallel_for_executor::parallel_for_executor(std::shared_ptr<parallel_for_data>& data, int worker_index): _data(data), worker_index(worker_index)
{
}

// 执行callbody
bool parallel_for_executor::operator()(const bool b_is_master)
{
	std::shared_ptr<parallel_for_data> data = _data.lock();
	assert(data);

	const int start_index = data->start_index;
	const int end_index = data->end_index;
	const int step = data->step;
	const int batch_size = data->batch_size;
	const int num_batches = data->num_batches;
	const parallel_for_fn &callbody = data->callbody;

	while (true)
	{
		int batch_index = data->batch_item.fetch_add(1);

		if (batch_index >= num_batches - 1)	// 最后一次，留给master thread完成
		{
			if (!b_is_master)
			{
				return false;
			}
			batch_index = num_batches - 1;	// 非master thread有可能让batch_index越界
		}

		int start_offset = start_index + (batch_index * batch_size) * step;
		int end_offset = std::min(start_offset + batch_size * step, end_index);
		for (int i = start_offset; i < end_offset; i += step)
		{
			callbody(i);
		}

		if (data->incomplete_batches.fetch_sub(1) == 1)	// 若当前是最后一次batch
		{
			if (!b_is_master)
			{
				data->finished_signal.trigger();
			}
			return true;
		}
		else if (end_offset >= end_index)	// 是最后一个batch，但不是最后完成的batch，当前worker结束
		{
			return false;
		}
		else	// 继续拿下一个batch执行
		{
			continue;
		}
	}
}

// 创建task实例并且让scheduler调度
void parallel_for_executor::launch_task(std::shared_ptr<parallel_for_data>& data, int worker_index)
{
	task_t new_task = data->tasks[worker_index];
	new_task.init(std::move(parallel_for_executor(data, worker_index)));
	scheduler::instance().try_launch(new_task);
}

void task_t::init(parallel_for_executor&& runnable)
{
	_runnable = std::move(runnable);
}

void task_t::execute()
{
	if (_runnable.is_valid())
		_runnable();
}

scheduler& scheduler::instance()
{
	static scheduler _instance{};
	return _instance;
}

int scheduler::get_number_of_workers(int num)
{
	int result = 0;
	if (num > 1)
	{
		result = std::min<int>(num_worker + 1, num); // 算上主线程
	}
	return std::max<int>(result, 1);
}

// 新task进入队列，通知所有worker
void scheduler::try_launch(task_t& task)
{
	{
		std::unique_lock<std::mutex> lock(_mutex);
		tasks.emplace(task);
		cv_new_task.notify_one();
	}
}

// 结束schcduler，需要等待所有worker执行完毕
void scheduler::terminal()
{
	b_terminate = true;
	cv_new_task.notify_all();
	for(std::thread& worker: workers)
	{
		worker.join();
	}
}

scheduler::scheduler()
{
	num_worker = std::thread::hardware_concurrency() - 1;	// 减去主线程
	for(int i = 0; i < num_worker; ++i)
	{
		workers.emplace_back([this]{
			while (true)
			{
				task_t task;
				{
					std::unique_lock<std::mutex> lock(_mutex);	// 获取锁
					cv_new_task.wait(lock, [this]{ return b_terminate || !tasks.empty(); });
					if (b_terminate && tasks.empty())
						return;
					task = std::move(tasks.front());
					tasks.pop();
				}
				task.execute();	// 释放锁后再执行task
			}
		});
	}
}

void parallel_for(int first, int last, int step, parallel_for_fn func)
{
	int num_func = (last - first - 1) / step + 1;	// 执行总次数
	assert(num_func >= 0);

	int num_workers = scheduler::instance().get_number_of_workers(num_func);

	if (num_workers <= 1)
	{
		for (int i = first; i < last; i += step)
		{
			func(i);
		}
		return;
	}

	int batch_size = 1;
	int num_batches = num_func;
	for (int div = 6; div > 0; div--)	// 6是magic number 假设一个worker最多执行6次batch
	{
		if (num_func >= num_workers * div)
		{
			batch_size = (num_func + num_workers * div - 1) / (num_workers * div);	// round up
			num_batches = (num_func + batch_size - 1) / batch_size;	// round up

			if (num_batches >= num_workers)
				break;
		}
	}
	num_workers -= 1;	// 减去主线程
	assert(batch_size * num_batches >= num_func);

	finish_signal finished_signal;
	std::shared_ptr<parallel_for_data> data = make_shared<parallel_for_data>(first, last, step, batch_size, num_batches, num_workers, func, finished_signal);

	for(int worker = 0; worker < num_workers; ++worker)
	{
		parallel_for_executor::launch_task(data, worker);	// 启动子任务
	}

	parallel_for_executor local_executor{data, num_workers};
	const bool b_finished_last = local_executor(true);

	if (!b_finished_last)
	{
		finished_signal.wait();
	}
}

void init_parallel_env()
{
}

void fini_parallel_env()
{
	scheduler::instance().terminal();
}

#endif
