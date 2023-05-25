#include <Ice/Ice.h>
#include <cstdio>
#include <thread>
#include <list>
#include <mutex>
#include <condition_variable>

#include "debug_protocol.h"
#include "cpu.h"
#include "regs.h"

using namespace std;
using namespace DosboxDebugger;

class WorkQueue {
	public:
	void process()
	{
		unique_lock lock(mutex_);

		while (!callbacks_.empty()) {
			CallbackEntry entry = callbacks_.front();
			callbacks_.pop_front();
			entry();
		}
	}

	void add(function<void()> response)
	{
		const unique_lock lock(mutex_);
		callbacks_.emplace_back(std::move(response));
	}

	private:
	using CallbackEntry = std::function<void()>;

	std::mutex mutex_;
	std::list<CallbackEntry> callbacks_;
};

static WorkQueue g_queue;

class ManualResetEvent {
	public:
	ManualResetEvent() : state_(false) {}
	void set()
	{
		unique_lock ul(m_);
		state_ = true;
		cv_.notify_one();
	}

	void reset()
	{
		unique_lock ul(m_);
		state_ = false;
	}

	void wait()
	{
		unique_lock ul(m_);
		while (!state_) {
			cv_.wait(ul);
		}
	}

	private:
	bool state_;
	mutex m_;
	condition_variable cv_;
};

class DebugHostImpl : public DebugHost {
	public:
	void Connect(const ::Ice::Current& current) override
	{
		Do([] {
		});
	}

	void Detach(const ::Ice::Current& current) override
	{
		Do([] {
		});
		printf("-> Detach\n");
	}

	void Exit(const ::Ice::Current& current) override
	{
		Do([] {
		});
		printf("-> Exit\n");
	}

	void Continue(const ::Ice::Current& current) override
	{
		Do([] {
		});
		printf("-> Continue\n");
	} 

	Registers Break(const ::Ice::Current& current) override
	{
		Registers result;
		Do([&result] {
			GetRegisters(result);
			printf("-> Break\n");
		});
		return result;
	} 

	Registers StepOver(const ::Ice::Current& current) override
	{
		Registers result;
		Do([&result] {
			GetRegisters(result);
			printf("-> StepOver\n");
		});
		return result;
	}

	Registers StepIn(const ::Ice::Current& current) override
	{
		Registers result;
		Do([&result] {
			GetRegisters(result);
			printf("-> StepIn\n");
		});
		return result;
	}

	Registers StepOut(const ::Ice::Current& current) override
	{
		Registers result;
		Do([&result] {
			GetRegisters(result);
			printf("-> StepOut\n");
		});
		return result;
	}

	Registers RunToCall(const ::Ice::Current& current) override
	{
		Registers result;
		Do([&result] {
			GetRegisters(result);
			printf("-> RunToCall\n");
		});
		return result;
	}

	Registers RunToAddress(Address address, const ::Ice::Current& current) override
	{
		Registers result;
		Do([&address, &result] {
			GetRegisters(result);
			printf("-> RunToAddress(%x:%x)\n", (int)address.segment, address.offset);
		});
		return result;
	}

	Registers GetState(const ::Ice::Current& current) override
	{
		Registers result;
		Do([&result] {
			GetRegisters(result);
		});
		return result;
	}

	AssemblySequence Disassemble(Address address, int length, const ::Ice::Current& current) override
	{
		Do([] {
		});
        printf("-> Disassemble(%x:%x, %d)\n", address.segment, address.offset, length);
		return AssemblySequence();
	}

	ByteSequence GetMemory(Address address, int length, const ::Ice::Current& current) override
	{
		Do([] {
		});
        printf("-> GetMemory(%x:%x, %d)\n", address.segment, address.offset, length);
		return ByteSequence();
	}

	BreakpointSequence ListBreakpoints(const ::Ice::Current& current) override
	{
		Do([] {
		});
        printf("-> ListBreakpoints\n");
		return BreakpointSequence();
	}

	void SetBreakpoint(Breakpoint breakpoint, const ::Ice::Current& current) override
	{
		Do([] {
		});
		const char *typeName = "Unk";
		switch(breakpoint.type)
		{
		case BreakpointType::Normal:          typeName = "Normal"; break;
		case BreakpointType::Read:            typeName = "Read"; break;
		case BreakpointType::Write:           typeName = "Write"; break;
		case BreakpointType::Interrupt:       typeName = "Interrupt"; break;
		case BreakpointType::InterruptWithAH: typeName = "IntAH"; break;
		case BreakpointType::InterruptWithAX: typeName = "IntAX"; break;
		}

        printf("-> SetBreakpoint(%d, %x:%x, %s, %d, %d)\n",
			breakpoint.number,
			breakpoint.address.segment, breakpoint.address.offset, 
			typeName,
			(int)breakpoint.ah,
			(int)breakpoint.al);
	}

	void DelBreakpoint(int bpNumber, const ::Ice::Current& current) override
	{
		Do([] {
		});
        printf("-> DelBreakpoint(%d)\n", bpNumber);
	}

	void SetReg(Register reg, int value, const ::Ice::Current& current) override
	{
		Do([] {
		});
		const char *regName = "";
		switch(reg)
		{
		case Register::Flags: regName = "Flags"; break;
		case Register::EAX:   regName = "EAX"; break;
		case Register::EBX:   regName = "EBX"; break;
		case Register::ECX:   regName = "ECX"; break;
		case Register::EDX:   regName = "EDX"; break;
		case Register::ESI:   regName = "ESI"; break;
		case Register::EDI:   regName = "EDI"; break;
		case Register::EBP:   regName = "EBP"; break;
		case Register::ESP:   regName = "ESP"; break;
		case Register::EIP:   regName = "EIP"; break;
		case Register::ES:    regName = "ES"; break;
		case Register::CS:    regName = "CS"; break;
		case Register::SS:    regName = "SS"; break;
		case Register::DS:    regName = "DS"; break;
		case Register::FS:    regName = "FS"; break;
		case Register::GS:    regName = "GS"; break;
		}

        printf("-> SetReg(%s, %x)\n", regName, value);
	}

private:
	static void Do(std::function<void()> func)
	{
		ManualResetEvent mre;
		g_queue.add([&mre, &func] {
			func();
			mre.set();
		});

		mre.wait();
	}

	static void GetRegisters(Registers &result)
	{
		result.flags = (int)reg_flags;
		result.eax   = (int)reg_eax;
		result.ebx   = (int)reg_ebx;
		result.ecx   = (int)reg_ecx;
		result.edx   = (int)reg_edx;
		result.esi   = (int)reg_esi;
		result.edi   = (int)reg_edi;
		result.ebp   = (int)reg_ebp;
		result.esp   = (int)reg_esp;
		result.eip   = (int)reg_eip;

		result.cs = Segs.val[cs];
		result.ds = Segs.val[ds];
		result.es = Segs.val[es];
		result.ss = Segs.val[ss];
		result.fs = Segs.val[fs];
		result.gs = Segs.val[gs];
	}
};

static std::unique_ptr<std::thread> g_debugThread;
static shared_ptr<Ice::Communicator> communicator;

static void ServerThread()
{
	try {
		Ice::CommunicatorHolder ich(Ice::InitializationData(), ICE_INT_VERSION);
		communicator = ich.communicator();
		const auto adapter = ich->createObjectAdapterWithEndpoints("DebugHostAdapter",
		                                                                "default -p 7243");
		const auto servant = make_shared<DebugHostImpl>();
		adapter->add(servant, Ice::stringToIdentity("DebugHost"));
		adapter->activate();
		ich->waitForShutdown();
	} catch (const std::exception& e) {
		cerr << e.what() << endl;
	}
}

void DEBUG_StartHost()
{
	if (g_debugThread) {
		return;
	}

	g_debugThread = std::make_unique<std::thread>(&ServerThread);
}

void DEBUG_StopHost()
{
	if (!g_debugThread) {
		return;
	}

	communicator->shutdown();
	g_debugThread->join();
}

void DEBUG_PollWork()
{
	g_queue.process();
}
