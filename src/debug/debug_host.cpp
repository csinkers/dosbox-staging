#include <Ice/Ice.h>
#include <condition_variable>
#include <cstdio>
#include <list>
#include <mutex>
#include <thread>

#include "debug_internal.h"
#include "debug_protocol.h"

using namespace std;
using namespace DosboxDebugger;

static void GetRegisters(Registers& result)
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

class Clients {
	mutex m_;
	vector<std::shared_ptr<DebugClientPrx>> clients_;

	public:
	void Add(std::shared_ptr<DebugClientPrx> proxy)
	{
		unique_lock ul(m_);
		const auto& it = std::find_if(clients_.begin(),
		                              clients_.end(),
		                              [&proxy](auto& x) {
			                              return Ice::targetEqualTo(x, proxy);
		                              });

		if (it == clients_.end()) {
			clients_.push_back(proxy);
		}
	}

	void Remove(std::shared_ptr<DebugClientPrx> proxy)
	{
		unique_lock ul(m_);
		const auto& it = std::remove_if(clients_.begin(),
		                                clients_.end(),
		                                [&proxy](auto x) {
			                                return x.get() == proxy.get();
		                                });

		if (it != clients_.end()) {
			clients_.erase(it);
		}
	}

	void ForEach(std::function<void(std::shared_ptr<DebugClientPrx>)> func)
	{
		unique_lock ul(m_);
		for (auto& it : clients_) {
			func(it);
		}
	}
} g_clients;

class DebugHostImpl : public DebugHost {
	public:
	void Connect(std::shared_ptr<DebugClientPrx> proxy,
	             const Ice::Current& current) override
	{
		g_clients.Add(proxy);
	}

	void Continue(const ::Ice::Current& current) override
	{
		Do([] {
			printf("-> Continue\n");
			DEBUG_Run(1, false);
		});
	}

	Registers Break(const ::Ice::Current& current) override
	{
		Registers result;
		Do([&result] {
			printf("-> Break\n");
			DOSBOX_SetLoop(&DEBUG_Loop);
			GetRegisters(result);
		});
		return result;
	}

	Registers StepIn(const ::Ice::Current& current) override
	{
		Registers result;
		Do([&result] {
			printf("-> StepIn\n");
			DEBUG_Run(1, true);
			GetRegisters(result);
		});
		return result;
	}

	Registers StepMultiple(int cycles, const ::Ice::Current& current) override
	{
		Registers result;
		Do([&cycles, &result] {
			printf("-> StepMultiple(%d)\n", cycles);
			DEBUG_Run(cycles, true);
			GetRegisters(result);
		});
		return result;
	}

	void RunToAddress(Address address, const ::Ice::Current& current) override
	{
		Do([&address] {
			printf("-> RunToAddress(%x:%x)\n", (int)address.segment, address.offset);
			if (!CBreakpoint::FindPhysBreakpoint(address.segment, address.offset, true)) {
				CBreakpoint::AddBreakpoint(address.segment, address.offset, true);
			}
			DEBUG_Run(1, false);
		});
	}

	Registers GetState(const ::Ice::Current& current) override
	{
		Registers result;
		Do([&result] { GetRegisters(result); });
		return result;
	}

	AssemblySequence Disassemble(Address address, int length,
	                             const ::Ice::Current& current) override
	{
		auto result = AssemblySequence();
		Do([&address, &length] {
			printf("-> Disassemble(%x:%x, %d)\n", address.segment, address.offset, length);
		});
		return result;
	}

	ByteSequence GetMemory(Address address, int length,
	                       const ::Ice::Current& current) override
	{
		auto result = ByteSequence(length);
		Do([&address, &length, &result] {
			printf("-> GetMemory(%x:%x, %d)\n", address.segment, address.offset, length);
			const auto uoff = (uint32_t)address.offset;
			const auto ulen = (uint32_t)length;
			for (uint32_t x = 0; x < ulen; x++) {
				const auto physAddr = GetAddress(address.segment, uoff + x);
				if (mem_readb_checked(physAddr, &result[x])) {
					result[x] = 0;
				}
			}
		});
		return result;
	}

	void SetMemory(Address address, ByteSequence bytes,
	               const Ice::Current& current) override
	{
		Do([&address, &bytes] {
			printf("-> SetMemory(%x:%x, %llu)\n",
			       address.segment,
			       address.offset,
			       bytes.size());
			const auto uoff = (uint32_t)address.offset;
			for (Bitu x = 0; x < bytes.size(); x++) {
				const auto physAddr = GetAddress(address.segment, uoff + x);
				mem_writeb_checked(physAddr, bytes[x]);
			}
		});
	}

	BreakpointSequence ListBreakpoints(const ::Ice::Current& current) override
	{
		BreakpointSequence result;
		Do([&result] {
			for (auto& it = CBreakpoint::begin(); it != CBreakpoint::end(); ++it) {
				result.push_back(Breakpoint());
				auto& bp = result.back();

				bp.address.segment = (*it)->GetSegment();
				bp.address.offset  = (*it)->GetOffset();
				bp.ah              = 0;
				bp.al              = 0;

				switch ((*it)->GetType()) {
				case BKPNT_PHYSICAL: bp.type = BreakpointType::Normal; break;

				case BKPNT_INTERRUPT:
					if ((*it)->GetValue() == BPINT_ALL) {
						bp.type = BreakpointType::Interrupt;
					} else if ((*it)->GetOther() == BPINT_ALL) {
						bp.type = BreakpointType::InterruptWithAH;
						bp.ah   = (*it)->GetValue();
					} else {
						bp.type = BreakpointType::InterruptWithAX;
						bp.ah   = (*it)->GetValue();
						bp.al   = (*it)->GetOther();
					}
					break;

				case BKPNT_MEMORY: bp.type = BreakpointType::Read; break;

				case BKPNT_UNKNOWN:
				case BKPNT_MEMORY_PROT:
				case BKPNT_MEMORY_LINEAR:
					bp.type = BreakpointType::Unknown;
					break;
				}
			}
			printf("-> ListBreakpoints\n");
		});
		return result;
	}

	void SetBreakpoint(Breakpoint breakpoint, const ::Ice::Current& current) override
	{
		Do([&breakpoint] {
			const char* typeName = "Unk";
			switch (breakpoint.type) {
			case BreakpointType::Normal:
				typeName = "Normal";
				CBreakpoint::AddBreakpoint(breakpoint.address.segment,
				                           breakpoint.address.offset,
				                           false);
				break;

			case BreakpointType::Read:
				typeName = "Read";
				CBreakpoint::AddMemBreakpoint(breakpoint.address.segment,
				                              breakpoint.address.offset);
				break;

			case BreakpointType::Write: typeName = "Write"; break;

			case BreakpointType::Interrupt:
				typeName = "Interrupt";
				CBreakpoint::AddIntBreakpoint((uint8_t)breakpoint.address.offset,
				                              BPINT_ALL,
				                              BPINT_ALL,
				                              false);
				break;

			case BreakpointType::InterruptWithAH:
				typeName = "IntAH";
				CBreakpoint::AddIntBreakpoint((uint8_t)breakpoint.address.offset,
				                              breakpoint.ah,
				                              BPINT_ALL,
				                              false);
				break;

			case BreakpointType::InterruptWithAX:
				typeName = "IntAX";
				CBreakpoint::AddIntBreakpoint((uint8_t)breakpoint.address.offset,
				                              breakpoint.ah,
				                              breakpoint.al,
				                              false);
				break;
			}

			printf("-> SetBreakpoint(%x:%x, %s, %d, %d)\n",
			       breakpoint.address.segment,
			       breakpoint.address.offset,
			       typeName,
			       (int)breakpoint.ah,
			       (int)breakpoint.al);
		});
	}

	void DelBreakpoint(Address address, const ::Ice::Current& current) override
	{
		Do([&address] {
			printf("-> DelBreakpoint(%x:%x)\n", address.segment, address.offset);
			CBreakpoint::DeleteBreakpoint(address.segment, address.offset);
		});
	}

	void SetReg(Register reg, int value, const ::Ice::Current& current) override
	{
		Do([&reg, &value] {
			const char* regName = "";
			switch (reg) {
			case Register::Flags: regName = "Flags"; break;
			case Register::EAX: regName = "EAX"; break;
			case Register::EBX: regName = "EBX"; break;
			case Register::ECX: regName = "ECX"; break;
			case Register::EDX: regName = "EDX"; break;
			case Register::ESI: regName = "ESI"; break;
			case Register::EDI: regName = "EDI"; break;
			case Register::EBP: regName = "EBP"; break;
			case Register::ESP: regName = "ESP"; break;
			case Register::EIP: regName = "EIP"; break;
			case Register::ES: regName = "ES"; break;
			case Register::CS: regName = "CS"; break;
			case Register::SS: regName = "SS"; break;
			case Register::DS: regName = "DS"; break;
			case Register::FS: regName = "FS"; break;
			case Register::GS: regName = "GS"; break;
			}

			printf("-> SetReg(%s, %x)\n", regName, value);
		});
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

static void AlertClients()
{
	printf("Hit BP\n");
	Registers state;
	GetRegisters(state);
	g_clients.ForEach([&state](auto client) {
		printf("Alerting client\n");
		client->StoppedAsync(
		    state,
		    [] { printf("Client alerted OK\n"); },
		    [client](exception_ptr) {
			    printf("Client error - removing proxy\n");
			    g_clients.Remove(client);
		    });
	});
}

static LoopHandler* lastLoop;
void DEBUG_PollWork()
{
	auto loop = DOSBOX_GetLoop();
	if (loop != lastLoop) {
		if (loop == DEBUG_Loop) {
			AlertClients();
		}
	}

	g_queue.process();
	// Don't need to send an alert if the debugger changed the state
	lastLoop = DOSBOX_GetLoop();
}
