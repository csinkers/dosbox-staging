#include <Ice/Ice.h>
#include <condition_variable>
#include <cstdio>
#include <list>
#include <mutex>
#include <thread>

#include "debug_inc.h"
#include "debug_internal.h"
#include "debug_protocol.h"

using namespace std;

static void GetRegisters(DosboxDebugger::Registers& result)
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
	vector<std::shared_ptr<DosboxDebugger::DebugClientPrx>> clients_;

	public:
	void Add(std::shared_ptr<DosboxDebugger::DebugClientPrx> proxy)
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

	void Remove(std::shared_ptr<DosboxDebugger::DebugClientPrx> proxy)
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

	void ForEach(const std::function<void(std::shared_ptr<DosboxDebugger::DebugClientPrx>)> &func)
	{
		unique_lock ul(m_);
		for (const auto& it : clients_) {
			func(it);
		}
	}
} g_clients;

class DebugHostImpl : public DosboxDebugger::DebugHost {
	public:
	void Connect(std::shared_ptr<DosboxDebugger::DebugClientPrx> proxy,
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

	DosboxDebugger::Registers Break(const ::Ice::Current& current) override
	{
		DosboxDebugger::Registers result;
		Do([&result] {
			printf("-> Break\n");
			DOSBOX_SetLoop(&DEBUG_Loop);
			GetRegisters(result);
		});
		return result;
	}

	DosboxDebugger::Registers StepIn(const ::Ice::Current& current) override
	{
		DosboxDebugger::Registers result;
		Do([&result] {
			printf("-> StepIn\n");
			DEBUG_Run(1, true);
			GetRegisters(result);
		});
		return result;
	}

	DosboxDebugger::Registers StepMultiple(int cycles, const ::Ice::Current& current) override
	{
		DosboxDebugger::Registers result;
		Do([&cycles, &result] {
			printf("-> StepMultiple(%d)\n", cycles);
			DEBUG_Run(cycles, true);
			GetRegisters(result);
		});
		return result;
	}

	void RunToAddress(DosboxDebugger::Address address, const ::Ice::Current& current) override
	{
		Do([&address] {
			printf("-> RunToAddress(%x:%x)\n", (int)address.segment, address.offset);
			if (!CBreakpoint::FindPhysBreakpoint(address.segment, address.offset, true)) {
				CBreakpoint::AddBreakpoint(address.segment, address.offset, true);
			}
			DEBUG_Run(1, false);
		});
	}

	DosboxDebugger::Registers GetState(const ::Ice::Current& current) override
	{
		DosboxDebugger::Registers result;
		Do([&result] { GetRegisters(result); });
		return result;
	}

	DosboxDebugger::AssemblySequence Disassemble(DosboxDebugger::Address address, int length,
	                             const ::Ice::Current& current) override
	{
		auto result = DosboxDebugger::AssemblySequence();
		Do([&address, &length, &result] {
			printf("-> Disassemble(%x:%x, %d)\n", address.segment, address.offset, length);

			char buffer[200];
			PhysPt start = GetAddress(address.segment, address.offset);
			PhysPt cur = start;
			for (int i = 0; i < length; i++) {
				Bitu size = DasmI386(buffer, cur, cur, cpu.code.big);

				DosboxDebugger::AssemblyLine line;
				line.address.segment = address.segment;
				line.address.offset = address.offset + (cur - start);
				line.line = buffer;

				for (Bitu c = 0; c < size; c++) {
					uint8_t value;
					if (mem_readb_checked((PhysPt)(cur + c), &value)) {
						value = 0;
					}
					line.bytes.push_back(value);
				}

				result.push_back(line);
				cur = (PhysPt)(cur + size);
			}
		});
		return result;
	}

	DosboxDebugger::ByteSequence GetMemory(DosboxDebugger::Address address, int length,
	                       const ::Ice::Current& current) override
	{
		auto result = DosboxDebugger::ByteSequence(length);
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

	void SetMemory(DosboxDebugger::Address address, DosboxDebugger::ByteSequence bytes,
	               const Ice::Current& current) override
	{
		Do([&address, &bytes] {
			printf("-> SetMemory(%x:%x, %llu)\n",
			       address.segment,
			       address.offset,
			       bytes.size());
			const auto uoff = (uint32_t)address.offset;
			for (Bitu x = 0; x < bytes.size(); x++) {
				const auto physAddr = GetAddress(address.segment, (uint32_t)(uoff + x));
				mem_writeb_checked(physAddr, bytes[x]);
			}
		});
	}

	DosboxDebugger::BreakpointSequence ListBreakpoints(const ::Ice::Current& current) override
	{
		DosboxDebugger::BreakpointSequence result;
		Do([&result] {
			for (auto& it = CBreakpoint::begin(); it != CBreakpoint::end(); ++it) {
				result.push_back(DosboxDebugger::Breakpoint());
				auto& bp = result.back();

				bp.address.segment = (*it)->GetSegment();
				bp.address.offset  = (*it)->GetOffset();
				bp.ah              = 0;
				bp.al              = 0;

				switch ((*it)->GetType()) {
				case BKPNT_PHYSICAL: bp.type = DosboxDebugger::BreakpointType::Normal; break;

				case BKPNT_INTERRUPT:
					if ((*it)->GetValue() == BPINT_ALL) {
						bp.type = DosboxDebugger::BreakpointType::Interrupt;
					} else if ((*it)->GetOther() == BPINT_ALL) {
						bp.type = DosboxDebugger::BreakpointType::InterruptWithAH;
						bp.ah   = (unsigned char)(*it)->GetValue();
					} else {
						bp.type = DosboxDebugger::BreakpointType::InterruptWithAX;
						bp.ah   = (unsigned char)(*it)->GetValue();
						bp.al   = (unsigned char)(*it)->GetOther();
					}
					break;

				case BKPNT_MEMORY: bp.type = DosboxDebugger::BreakpointType::Read; break;

				case BKPNT_UNKNOWN:
				case BKPNT_MEMORY_PROT:
				case BKPNT_MEMORY_LINEAR:
					bp.type = DosboxDebugger::BreakpointType::Unknown;
					break;
				}
			}
			printf("-> ListBreakpoints\n");
		});
		return result;
	}

	void SetBreakpoint(DosboxDebugger::Breakpoint breakpoint, const ::Ice::Current& current) override
	{
		Do([&breakpoint] {
			const char* typeName = "Unk";
			switch (breakpoint.type) {
			case DosboxDebugger::BreakpointType::Normal:
				typeName = "Normal";
				CBreakpoint::AddBreakpoint(breakpoint.address.segment,
				                           breakpoint.address.offset,
				                           false);
				break;

			case DosboxDebugger::BreakpointType::Read:
				typeName = "Read";
				CBreakpoint::AddMemBreakpoint(breakpoint.address.segment,
				                              breakpoint.address.offset);
				break;

			case DosboxDebugger::BreakpointType::Write: typeName = "Write"; break;

			case DosboxDebugger::BreakpointType::Interrupt:
				typeName = "Interrupt";
				CBreakpoint::AddIntBreakpoint((uint8_t)breakpoint.address.offset,
				                              BPINT_ALL,
				                              BPINT_ALL,
				                              false);
				break;

			case DosboxDebugger::BreakpointType::InterruptWithAH:
				typeName = "IntAH";
				CBreakpoint::AddIntBreakpoint((uint8_t)breakpoint.address.offset,
				                              breakpoint.ah,
				                              BPINT_ALL,
				                              false);
				break;

			case DosboxDebugger::BreakpointType::InterruptWithAX:
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

	void DelBreakpoint(DosboxDebugger::Address address, const ::Ice::Current& current) override
	{
		Do([&address] {
			printf("-> DelBreakpoint(%x:%x)\n", address.segment, address.offset);
			CBreakpoint::DeleteBreakpoint(address.segment, address.offset);
		});
	}

	void SetReg(DosboxDebugger::Register reg, int value, const ::Ice::Current& current) override
	{
		Do([&reg, &value] {
			const char* regName = "";
			switch (reg) {
			case DosboxDebugger::Register::Flags: regName = "Flags"; break;
			case DosboxDebugger::Register::EAX: regName = "EAX"; break;
            case DosboxDebugger::Register::EBX: regName = "EBX"; break;
			case DosboxDebugger::Register::ECX: regName = "ECX"; break;
			case DosboxDebugger::Register::EDX: regName = "EDX"; break;
			case DosboxDebugger::Register::ESI: regName = "ESI"; break;
			case DosboxDebugger::Register::EDI: regName = "EDI"; break;
			case DosboxDebugger::Register::EBP: regName = "EBP"; break;
			case DosboxDebugger::Register::ESP: regName = "ESP"; break;
			case DosboxDebugger::Register::EIP: regName = "EIP"; break;
			case DosboxDebugger::Register::ES: regName = "ES"; break;
			case DosboxDebugger::Register::CS: regName = "CS"; break;
			case DosboxDebugger::Register::SS: regName = "SS"; break;
			case DosboxDebugger::Register::DS: regName = "DS"; break;
			case DosboxDebugger::Register::FS: regName = "FS"; break;
			case DosboxDebugger::Register::GS: regName = "GS"; break;
			}

			printf("-> SetReg(%s, %x)\n", regName, value);
		});
	}

	void AddDescriptor(DosboxDebugger::Descriptors& results, Descriptor& desc)
	{
		if (desc.Type() & 0x04) { // Gate
			auto result    = std::make_shared<DosboxDebugger::GateDescriptor>();
			result->type   = (DosboxDebugger::SegmentType)desc.Type();
			result->offset = (int)desc.GetOffset();
			result->selector = (int)desc.GetSelector();
			result->dpl      = desc.DPL();
			result->big      = !!desc.Big();
			results.push_back(result);
		} else { // Segment
			auto result = std::make_shared<DosboxDebugger::SegmentDescriptor>();
			result->type  = (DosboxDebugger::SegmentType)desc.Type();
			result->base  = (int)desc.GetBase();
			result->limit = (int)desc.GetLimit();
			result->dpl   = desc.DPL();
			result->big   = desc.Big();
			results.push_back(result);
		}
	}

	DosboxDebugger::Descriptors GetGdt(const Ice::Current& current) override
	{
		Descriptor desc;
		Bitu length    = cpu.gdt.GetLimit();
		PhysPt address = cpu.gdt.GetBase();
		PhysPt max     = (PhysPt)(address + length);

		DosboxDebugger::Descriptors results;
		while (address < max) {
			desc.Load(address);
			AddDescriptor(results, desc);
			address += 8;
		}
		return results;
	}

	DosboxDebugger::Descriptors GetLdt(const Ice::Current& current) override
	{
		DosboxDebugger::Descriptors results;
		Descriptor desc;
		Bitu ldtSelector = cpu.gdt.SLDT();

		if (!cpu.gdt.GetDescriptor(ldtSelector, desc)) {
			return results;
		}

		Bitu length    = desc.GetLimit();
		PhysPt address = desc.GetBase();
		PhysPt max     = (PhysPt)(address + length);
		while (address < max) {
			desc.Load(address);
			AddDescriptor(results, desc);
			address += 8;
		}
		return results;
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

static void AlertClients()
{
	DosboxDebugger::Registers state;
	GetRegisters(state);
	g_clients.ForEach([&state](auto client) {
		client->StoppedAsync(
		    state,
		    [] {},
		    [client](const exception_ptr&) { g_clients.Remove(client); });
	});
}

static std::unique_ptr<std::thread> g_debugThread;
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
	g_debugThread.reset();
}

static LoopHandler* lastLoop;
void DEBUG_PollWork()
{
	auto loop = DOSBOX_GetLoop();
	if (loop != lastLoop && loop == DEBUG_Loop) {
		AlertClients();
	}

	g_queue.process();

	// Don't need to send an alert if the debugger changed the state
	lastLoop = DOSBOX_GetLoop();
}
