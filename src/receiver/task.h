/*
 * Neumo dvb (C) 2019-2021 deeptho@gmail.com
 * Copyright notice:
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#pragma once

#include <future>
#include <thread>
#include <functional>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <linux/limits.h>
#include <queue>
#include "util/util.h"
#include "util/logger.h"
#include "util/safe/threads.h"


class periodic_t {
	steady_time_t last_time{};
	std::chrono::duration<double> period;
public:
	periodic_t(time_t period=30)
		: period(period)
		{}

	template<typename fn_t>
	void run(fn_t fn, system_time_t now) {
		auto now_ = steady_clock_t::now();
		if (now_ > last_time + period) {
			fn(now);
			last_time = now_;
		}
	}
};



class task_queue_t {
	template<typename T> friend typename T::cb_t& cb(T& t);
	template<typename T> friend const typename T::cb_t& cb(const T& t);
public:
	typedef std::packaged_task<int()> task_t;
	typedef std::future<int> future_t;
	typedef std::queue<task_t> queue_t;

private:
	thread_group_t thread_group;
	std::array<struct epoll_event, 8> events;
	int num_pending_events =0;
	int _current_event =0;
protected:
	//time_t now {}; //current time
	bool must_exit_ = false ; //a command can set this to true to request thread exit
private:
	mutable std::mutex mutex;
	mutable queue_t tasks;
protected:
	int timer_fd = -1;
	mutable event_handle_t notify_fd;
public:
	epoll_t epx;
protected:
	std::thread thread_;
	//std::thread::id owner;
	std::future<int> status_future;
	virtual int run() = 0;
	virtual int exit() = 0;

public:
	void join() {
		if(thread_.joinable())
			thread_.join();
	}

	void detach() {
		if(thread_.joinable())
			thread_.detach();
	}


protected:
	void set_name(const char*name) {
		pthread_setname_np(pthread_self(), name);
		log4cxx::MDC::put( "thread_name", name);
	}

	bool is_timer_fd(const epoll_event* event) const {
		bool ret =event->data.fd== int(timer_fd);
		if(!ret)
			return ret;
		for(;;) {
			uint64_t val;
			if(read(timer_fd, (void*)&val, sizeof(val))>=0)
				return ret;
			if(errno!=EINTR) {
				dterrorx("error while reading timerfd: %s", strerror(errno));
				return true;
			}
		}
	}

	void timer_start(double period_sec=2.0)
		{
			timer_fd  = ::timer_start(period_sec);
			epx.add_fd(timer_fd, EPOLLIN|EPOLLERR|EPOLLHUP);
		}

	void timer_set_period(double period_sec)
		{
			::timer_set_period(timer_fd, period_sec);
		}


	void timer_stop() {
		epx.remove_fd(timer_fd);
		::timer_stop(timer_fd);
	}

	int run_() {
#ifdef DTDEBUG
		epx.set_owner();
#endif
		try {
			::thread_group = this->thread_group;
			return run();
		} catch (const std::exception& e) { // caught by reference to base
			dterror("exception was caught: " << e.what());
    }
		return -1;
	}

	const epoll_event* current_event() const {
		return _current_event < num_pending_events ? &events[_current_event] : NULL;
	}
public:


	void start_running() {
		auto task = std::packaged_task<int(void)>(std::bind(&task_queue_t::run_, this));
		status_future= task.get_future();
		thread_= std::thread(std::move(task));
	}


	void stop_running(bool wait) {
		auto f = push_task( [this](){
			must_exit_=true;
			exit();
			return -1;
		});
		if(wait) {
			f.wait();
			status_future.wait();
			thread_.join();
		} else
			thread_.detach();
	}

	int wait() {
		return status_future.get();
	}

	//std::future<int(void)> tuner_queue_status;

	task_queue_t(thread_group_t thread_group) :thread_group(thread_group)  {
		//owner=std::this_thread::get_id();
		epx.add_fd(int(notify_fd), EPOLLIN|EPOLLERR|EPOLLHUP);
#ifdef DTDEBUG
		epx.set_owner((pid_t)-1);
#endif
	}

	virtual ~task_queue_t() {
	}

	std::future<int> push_task(std::function<int()>&& callback) const {
		if(std::this_thread::get_id() == this->thread_.get_id()) {
			dterror("Thread calls back to itself");
			//assert(0);
		}

		auto perr = ::error_;
		std::packaged_task<int()> task([perr, callback{std::move(callback)}]() {
			auto ret = callback();
			if(perr.get())
				*perr = *error_;
			return ret;
		});
		auto f = task.get_future();

		std::lock_guard<std::mutex> lk(mutex);
		tasks.push(std::move(task));
		notify_fd.unblock();
		return f;
	}

	void acknowledge() {
		notify_fd.reset();
	}
	void _pop_task() {
		//assert(mutex.is_locked());
		tasks.pop();
	}

	int epoll_add_fd(int fd, int mask) {
		return epx.add_fd(fd, mask);
	}

	int epoll_remove_fd(int fd) {
		return epx.remove_fd(fd);
	}

	int epoll_modify_fd(int fd, int mask) {
		return epx.mod_fd(fd, mask);
	}

	/*returns <0 in case of error which is not an interrupt. In this case errno can be checked
		return n==0 in case of timeout
		returns the number of epoll events in case n>0
	*/
	int epoll_wait(struct epoll_event* events, int maxevents, int timeout)
		{
			return epx.wait(events, maxevents, timeout);
		}

	/*
		version which saves events internally
	*/
	int epoll_wait(int timeout)
		{
			if(_current_event == num_pending_events) {
				int n = epx.wait(&this->events[0], this->events.size(), timeout);
				if(n<=0)
					return n;
				num_pending_events = n;
				_current_event =0;
			}
			return num_pending_events - _current_event;
		}


	const epoll_event* next_event() {
		auto* ret = current_event();
		if(ret)
			_current_event++;
		return ret;
	}

	bool is_event_fd(const epoll_event* event) const {
		return event->data.fd== int(notify_fd);
	}


	bool empty() const {
		return tasks.empty();
	}

	const task_t& front() const {
		return tasks.front();
	}

	task_t& front() {
		return tasks.front();
	}

protected:
	int run_tasks(system_time_t now_);

};



template<typename T> typename T::cb_t& cb(T& t) { //activate callbacks
//	auto* self = dynamic_cast<typename T::cb_t*>(&t);
	auto* self = (typename T::cb_t*)(&t);
	auto* q = dynamic_cast<task_queue_t*>(&t);
	if(!self || !q)
		dterror("Implementation error");
	if(std::this_thread::get_id() != q->thread_.get_id()) {
		dterror("Callback called from the wrong thread");
		assert(0);
	}
	return *self;
}

template<typename T> const typename T::cb_t& cb(const T& t) { //activate callbacks
	auto* self = (const typename T::cb_t*)(&t);
	if(!self)
		dterror("Implementation error");
	return *self;
}
bool wait_for_all(std::vector<task_queue_t::future_t>& futures);

#if 0
template<typename T> typename T::thread_safe_t& ts(T& t) { //activate callbacks
//	auto* self = dynamic_cast<typename T::cb_t*>(&t);
	auto* self = (typename T::cb_t*)(&t);
	auto* q = dynamic_cast<task_queue_t*>(&t);
	if(!self || !q)
		dterror("Implementation error");
	if(std::this_thread::get_id() != q->thread_.get_id()) {
		dterror("Callback called from the wrong thread");
		assert(0);
	}
	return *self;
}

template<typename T> const typename T::thread_safe_t& ts(const T& t) { //activate callbacks
	//auto* self = dynamic_cast<const typename T::cb_t*>(&t);
	auto* self = (const typename T::cb_t*)(&t);
	auto* q = dynamic_cast<const task_queue_t*>(&t);
	if(!self || !q)
		dterror("Implementation error");
	if(std::this_thread::get_id() != q->thread_.get_id()) {
		dterror("Callback called from the wrong thread");
		assert(0);
	}
	return *self;
}
#endif
