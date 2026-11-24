/*
	Copyright (C) 2026 by the Wesnoth Access contributors
	Part of the Battle for Wesnoth Project https://www.wesnoth.org/

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.
	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY.

	See the COPYING file for more details.
*/

#include "accessibility/speech.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace accessibility::speech
{
namespace
{
enum class request_type { say, stop, configure, quit };

struct request
{
	request_type type;
	std::string text;
	bool interrupt = false;
};

/** Interface implemented by every backend. Only used on the worker thread. */
class backend
{
public:
	virtual ~backend() = default;
	virtual void say(const std::string& utf8, bool interrupt) = 0;
	virtual void stop() = 0;
	virtual void configure(bool /*sapi_fallback*/, const std::string& /*engine*/) {}
	/** Called periodically when idle so engines can refresh detection. */
	virtual std::string current_engine() { return std::string(); }
	virtual std::vector<std::string> engines() { return {}; }
};

class null_backend : public backend
{
public:
	void say(const std::string&, bool) override {}
	void stop() override {}
};

class stream_backend : public backend
{
public:
	explicit stream_backend(const std::string& path)
	{
		if(!path.empty()) {
			file_ = std::make_unique<std::ofstream>(path, std::ios::out | std::ios::app | std::ios::binary);
		}
	}

	void say(const std::string& utf8, bool interrupt) override
	{
		std::ostream& os = (file_ && file_->good()) ? *file_ : std::cout;
		os << (interrupt ? "SAY! " : "SAY+ ") << utf8 << '\n';
		os.flush();
	}

	void stop() override
	{
		std::ostream& os = (file_ && file_->good()) ? *file_ : std::cout;
		os << "STOP\n";
		os.flush();
	}

	std::string current_engine() override { return file_ ? "log file" : "standard output"; }

private:
	std::unique_ptr<std::ofstream> file_;
};

#ifdef _WIN32

std::wstring widen(const std::string& utf8)
{
	if(utf8.empty()) {
		return std::wstring();
	}
	int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
	std::wstring res(static_cast<std::size_t>(std::max(len, 0)), L'\0');
	if(len > 0) {
		MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), res.data(), len);
	}
	return res;
}

std::string narrow(const wchar_t* w)
{
	if(!w) {
		return std::string();
	}
	int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	if(len <= 1) {
		return std::string();
	}
	std::string res(static_cast<std::size_t>(len - 1), '\0');
	WideCharToMultiByte(CP_UTF8, 0, w, -1, res.data(), len, nullptr, nullptr);
	return res;
}

/** Universal Speech (https://github.com/qtnc/UniversalSpeech), loaded at runtime. */
class universal_speech_backend : public backend
{
	// Values from UniversalSpeech.h (enum speechparam).
	static constexpr int SP_ENABLE_NATIVE_SPEECH = 0xFFFF;
	static constexpr int SP_ENGINE = 0x40000;
	static constexpr int SP_ENGINE_AVAILABLE = 0x50000;

	using say_fn = int(__cdecl*)(const wchar_t*, int);
	using stop_fn = int(__cdecl*)();
	using getv_fn = int(__cdecl*)(int);
	using setv_fn = int(__cdecl*)(int, int);
	using gets_fn = const wchar_t*(__cdecl*)(int);

public:
	universal_speech_backend(bool sapi_fallback, const std::string& engine)
	{
		// COM is used by the JAWS and SAPI drivers; initialise it on this thread.
		com_ok_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));

		wchar_t path[MAX_PATH] = {0};
		GetModuleFileNameW(nullptr, path, MAX_PATH);
		std::wstring dir(path);
		auto pos = dir.find_last_of(L"\\/");
		dir = (pos == std::wstring::npos) ? std::wstring() : dir.substr(0, pos + 1);
		std::wstring dll = dir + L"UniversalSpeech.dll";

		lib_ = LoadLibraryW(dll.c_str());
		if(!lib_) {
			error_ = "UniversalSpeech.dll could not be loaded (error " + std::to_string(GetLastError()) + ")";
			return;
		}
		say_ = reinterpret_cast<say_fn>(reinterpret_cast<void*>(GetProcAddress(lib_, "speechSay")));
		stop_ = reinterpret_cast<stop_fn>(reinterpret_cast<void*>(GetProcAddress(lib_, "speechStop")));
		getv_ = reinterpret_cast<getv_fn>(reinterpret_cast<void*>(GetProcAddress(lib_, "speechGetValue")));
		setv_ = reinterpret_cast<setv_fn>(reinterpret_cast<void*>(GetProcAddress(lib_, "speechSetValue")));
		gets_ = reinterpret_cast<gets_fn>(reinterpret_cast<void*>(GetProcAddress(lib_, "speechGetString")));
		if(!say_ || !stop_ || !getv_ || !setv_ || !gets_) {
			error_ = "UniversalSpeech.dll does not export the expected functions";
			FreeLibrary(lib_);
			lib_ = nullptr;
			return;
		}
		configure(sapi_fallback, engine);
	}

	~universal_speech_backend() override
	{
		if(lib_) {
			if(stop_) {
				stop_();
			}
			FreeLibrary(lib_);
		}
		if(com_ok_) {
			CoUninitialize();
		}
	}

	bool loaded() const { return lib_ != nullptr; }
	const std::string& error() const { return error_; }

	std::vector<std::string> engines() override
	{
		std::vector<std::string> res;
		if(!lib_) {
			return res;
		}
		for(int i = 0; i < 64; ++i) {
			const wchar_t* n = gets_(SP_ENGINE + i);
			if(!n) {
				break;
			}
			res.push_back(narrow(n));
		}
		return res;
	}

	int engine_index(const std::string& name)
	{
		auto list = engines();
		for(std::size_t i = 0; i < list.size(); ++i) {
			std::string a = list[i], b = name;
			std::transform(a.begin(), a.end(), a.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			std::transform(b.begin(), b.end(), b.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if(a == b || (b == "sapi" && a == "sapi5")) {
				return static_cast<int>(i);
			}
		}
		return -1;
	}

	void configure(bool sapi_fallback, const std::string& engine) override
	{
		if(!lib_) {
			return;
		}
		sapi_fallback_ = sapi_fallback;
		setv_(SP_ENABLE_NATIVE_SPEECH, sapi_fallback ? 1 : 0);
		forced_ = -1;
		if(!engine.empty() && engine != "auto") {
			forced_ = engine_index(engine);
		}
		setv_(SP_ENGINE, forced_);
	}

	/**
	 * Universal Speech lists Narrator as an engine, but its driver cannot
	 * speak. If it got selected automatically, use SAPI instead so the user
	 * still hears something.
	 */
	void avoid_silent_engines()
	{
		if(forced_ >= 0) {
			return;
		}
		int cur = getv_(SP_ENGINE);
		auto list = engines();
		if(cur >= 0 && static_cast<std::size_t>(cur) < list.size() && list[cur] == "Narrator") {
			int sapi = engine_index("sapi");
			if(sapi >= 0 && sapi_fallback_) {
				setv_(SP_ENGINE, sapi);
				// Return to automatic detection on the next configure().
			}
		}
	}

	void say(const std::string& utf8, bool interrupt) override
	{
		if(!lib_) {
			return;
		}
		avoid_silent_engines();
		std::wstring w = widen(utf8);
		say_(w.c_str(), interrupt ? 1 : 0);
	}

	void stop() override
	{
		if(lib_) {
			stop_();
		}
	}

	std::string current_engine() override
	{
		if(!lib_) {
			return std::string();
		}
		int cur = getv_(SP_ENGINE);
		auto list = engines();
		if(cur >= 0 && static_cast<std::size_t>(cur) < list.size()) {
			return list[cur];
		}
		return "none detected";
	}

private:
	HMODULE lib_ = nullptr;
	say_fn say_ = nullptr;
	stop_fn stop_ = nullptr;
	getv_fn getv_ = nullptr;
	setv_fn setv_ = nullptr;
	gets_fn gets_ = nullptr;
	std::string error_;
	bool sapi_fallback_ = true;
	int forced_ = -1;
	bool com_ok_ = false;
};

#else

/** speech-dispatcher through the spd-say command line tool (Linux/BSD convenience). */
class spd_backend : public backend
{
public:
	void say(const std::string& utf8, bool interrupt) override
	{
		if(interrupt) {
			run({"spd-say", "-C"});
		}
		run({"spd-say", "--", utf8});
	}

	void stop() override { run({"spd-say", "-C"}); }
	std::string current_engine() override { return "speech-dispatcher"; }

	static bool available()
	{
		return std::system("command -v spd-say >/dev/null 2>&1") == 0;
	}

private:
	static void run(const std::vector<std::string>& args)
	{
		pid_t pid = fork();
		if(pid == 0) {
			std::vector<char*> argv;
			for(const auto& a : args) {
				argv.push_back(const_cast<char*>(a.c_str()));
			}
			argv.push_back(nullptr);
			execvp(argv[0], argv.data());
			_exit(127);
		} else if(pid > 0) {
			int status = 0;
			waitpid(pid, &status, 0);
		}
	}
};

#endif

struct worker_state
{
	std::mutex mutex;
	std::condition_variable cv;
	std::condition_variable idle_cv;
	std::deque<request> queue;
	bool busy = false;
	std::thread thread;
	bool running = false;
	backend_status status;
	std::vector<std::string> engines;
	bool sapi_fallback = true;
	std::string engine = "auto";
};

worker_state& state()
{
	static worker_state s;
	return s;
}

std::unique_ptr<backend> make_backend(const std::string& id, const std::string& log_path, bool sapi_fallback,
	const std::string& engine, backend_status& st)
{
	st = backend_status();
	if(id == "none") {
		st.backend = "none";
		return std::make_unique<null_backend>();
	}
	if(id == "log" || id == "stdout") {
		st.backend = id;
		st.library_loaded = true;
		st.detail = id == "log" ? log_path : std::string();
		return std::make_unique<stream_backend>(id == "log" ? log_path : std::string());
	}
#ifdef _WIN32
	auto us = std::make_unique<universal_speech_backend>(sapi_fallback, engine);
	st.backend = "universal_speech";
	st.library_loaded = us->loaded();
	st.detail = us->error();
	if(us->loaded()) {
		return us;
	}
	return std::make_unique<null_backend>();
#else
	(void)sapi_fallback;
	(void)engine;
	if(spd_backend::available()) {
		st.backend = "spd";
		st.library_loaded = true;
		return std::make_unique<spd_backend>();
	}
	st.backend = "none";
	st.detail = "no speech backend available on this platform (spd-say not found)";
	return std::make_unique<null_backend>();
#endif
}

void worker_main(std::string id, std::string log_path, bool sapi_fallback, std::string engine)
{
	worker_state& s = state();
	backend_status st;
	std::unique_ptr<backend> be = make_backend(id, log_path, sapi_fallback, engine, st);
	{
		std::lock_guard lock(s.mutex);
		st.engine = be->current_engine();
		s.status = st;
		s.engines = be->engines();
	}

	for(;;) {
		request req;
		{
			std::unique_lock lock(s.mutex);
			s.busy = false;
			s.idle_cv.notify_all();
			s.cv.wait(lock, [&s] { return !s.queue.empty(); });
			req = std::move(s.queue.front());
			s.queue.pop_front();
			s.busy = true;
		}

		switch(req.type) {
		case request_type::quit:
			be.reset();
			{
				std::lock_guard lock(s.mutex);
				s.busy = false;
				s.idle_cv.notify_all();
			}
			return;
		case request_type::stop:
			be->stop();
			break;
		case request_type::configure: {
			bool fb;
			std::string eng;
			{
				std::lock_guard lock(s.mutex);
				fb = s.sapi_fallback;
				eng = s.engine;
			}
			be->configure(fb, eng);
			break;
		}
		case request_type::say:
			be->say(req.text, req.interrupt);
			break;
		}

		std::string eng = be->current_engine();
		std::lock_guard lock(s.mutex);
		s.status.engine = eng;
	}
}

void push(request&& r)
{
	worker_state& s = state();
	{
		std::lock_guard lock(s.mutex);
		if(!s.running) {
			return;
		}
		if(r.type == request_type::stop || (r.type == request_type::say && r.interrupt)) {
			// Drop pending utterances: they are superseded.
			s.queue.erase(std::remove_if(s.queue.begin(), s.queue.end(),
				[](const request& q) { return q.type == request_type::say; }), s.queue.end());
		}
		s.queue.push_back(std::move(r));
	}
	s.cv.notify_one();
}

} // anonymous namespace

void start(const std::string& backend, const std::string& log_path, bool sapi_fallback, const std::string& engine)
{
	stop_worker();
	worker_state& s = state();
	{
		std::lock_guard lock(s.mutex);
		s.queue.clear();
		s.running = true;
		s.busy = true;
		s.sapi_fallback = sapi_fallback;
		s.engine = engine;
		s.status = backend_status();
		s.status.backend = backend;
	}
	s.thread = std::thread(worker_main, backend, log_path, sapi_fallback, engine);
}

void stop_worker()
{
	worker_state& s = state();
	{
		std::lock_guard lock(s.mutex);
		if(!s.running) {
			return;
		}
		s.queue.clear();
		s.queue.push_back(request{request_type::quit, std::string(), false});
		s.running = false;
	}
	s.cv.notify_one();
	if(s.thread.joinable()) {
		s.thread.join();
	}
}

bool running()
{
	worker_state& s = state();
	std::lock_guard lock(s.mutex);
	return s.running;
}

void say(const std::string& text, bool interrupt)
{
	if(text.empty()) {
		return;
	}
	push(request{request_type::say, text, interrupt});
}

void silence()
{
	push(request{request_type::stop, std::string(), true});
}

void configure(bool sapi_fallback, const std::string& engine)
{
	{
		worker_state& s = state();
		std::lock_guard lock(s.mutex);
		s.sapi_fallback = sapi_fallback;
		s.engine = engine;
	}
	push(request{request_type::configure, std::string(), false});
}

backend_status status()
{
	worker_state& s = state();
	std::lock_guard lock(s.mutex);
	return s.status;
}

std::vector<std::string> engine_names()
{
	worker_state& s = state();
	std::lock_guard lock(s.mutex);
	return s.engines;
}

bool wait_idle(unsigned timeout_ms)
{
	worker_state& s = state();
	std::unique_lock lock(s.mutex);
	return s.idle_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
		[&s] { return !s.running || (s.queue.empty() && !s.busy); });
}

} // namespace accessibility::speech
