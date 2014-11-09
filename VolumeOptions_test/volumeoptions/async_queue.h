/*
Copyright (c) 2014, Paul Dolcet
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

	* Redistributions of source code must retain the above copyright notice, this
	list of conditions and the following disclaimer.

	* Redistributions in binary form must reproduce the above copyright notice,
	this list of conditions and the following disclaimer in the documentation
	and/or other materials provided with the distribution.

	* Neither the name of [project] nor the names of its
	contributors may be used to endorse or promote products derived from
	this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
/*
	TODO: tiene memory leaks, revisar algun dia.
	algo del pop o los punteros a function de function
*/

#ifndef ASYNC_QUEUE_H
#define ASYNC_QUEUE_H

#include <queue>
#include <condition_variable>
#include <atomic>


struct Function 
{};

// Derived class template for functions with a particular signature.
template <typename T>
struct GenericFunction : Function 
{
	std::function<T> function;
	GenericFunction(std::function<T> function) : function(function) {}
};

//template<typename Data>
class async_queue
{
private:
	//typedef std::function<void()> call;
	typedef std::unique_ptr<Function> call;
	std::queue<call> m_queue;
	std::atomic<bool> stop;
	mutable std::mutex m_mutex;
	std::condition_variable m_condition_variable;
public:
	async_queue() : stop(false) {}

	//void push(call const& data)
	void push(call&& data)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		m_queue.push(std::move(data));
		lock.unlock();
		m_condition_variable.notify_one();
	}

	bool empty() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_queue.empty();
	}

	size_t size()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_queue.size();
	}

	bool try_pop(call&& popped_value)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_queue.empty())
		{
			return false;
		}

		popped_value = std::move(m_queue.front());
		m_queue.pop();

		return true;
	}

	void exit_wait()
	{
		stop = true;
		m_condition_variable.notify_one();
	}

	bool wait_and_pop(call&& popped_value)
	{
		std::unique_lock<std::mutex> lock(m_mutex);
		while (m_queue.empty() && !stop)
		{
			m_condition_variable.wait(lock);
		}
		if (stop)
			return false;

		popped_value = std::move(m_queue.front());
		m_queue.pop();

		return true;
	}

};

#endif