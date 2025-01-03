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

static bool IsStopped() { return DOSBOX_GetLoop() == DEBUG_Loop; }

static void GetRegisters(DosboxDebugger::Registers& result)
{
	result.stopped = IsStopped();
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

	result.cs = (short)Segs.val[cs];
	result.ds = (short)Segs.val[ds];
	result.es = (short)Segs.val[es];
	result.ss = (short)Segs.val[ss];
	result.fs = (short)Segs.val[fs];
	result.gs = (short)Segs.val[gs];
}

static void ResumeExecution() { // Based on DEBUG_Run
	CPU_Cycles = 1;
	(*cpudecoder)();

	// ensure all breakpoints are activated
	CBreakpoint::ActivateBreakpoints();
	DOSBOX_SetNormalLoop();
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
			ResumeExecution();
		});
	}

	DosboxDebugger::Registers Break(const ::Ice::Current& current) override
	{
		DosboxDebugger::Registers result = {};
		Do([&result] {
			printf("-> Break\n");
			DOSBOX_SetLoop(&DEBUG_Loop);
			GetRegisters(result);
		});
		return result;
	}

	DosboxDebugger::Registers StepIn(const ::Ice::Current& current) override
	{
		DosboxDebugger::Registers result = {};
		Do([&result] {
			printf("-> StepIn\n");
			CPU_Cycles = 1;
			(*cpudecoder)();
			GetRegisters(result);
		});
		return result;
	}

	DosboxDebugger::Registers StepOver(const ::Ice::Current& current) override
	{
		DosboxDebugger::Registers result = {};
		Do([&result] { 
			printf("-> StepOver\n");

			char dline[200];
			const PhysPt start = GetAddress(SegValue(cs), reg_eip);
			const Bitu size = DasmI386(dline, start, reg_eip, cpu.code.big);

			if (strstr(dline, "call") || strstr(dline, "int") ||
			    strstr(dline, "loop") || strstr(dline, "rep")) {
				const auto nextIP = (uint32_t)(reg_eip + size);

				// Don't add a temporary breakpoint if there's already one there
				if (!CBreakpoint::FindPhysBreakpoint(SegValue(cs), nextIP, true)) {
					CBreakpoint::AddBreakpoint(SegValue(cs), nextIP, true);
				}

				ResumeExecution();
			} else {
				CPU_Cycles = 1;
				(*cpudecoder)();
			}

			GetRegisters(result);
		});
		return result;
	}

	DosboxDebugger::Registers StepMultiple(int cycles, const ::Ice::Current& current) override
	{
		DosboxDebugger::Registers result = {};
		Do([&cycles, &result] {
			printf("-> StepMultiple(%d)\n", cycles);
			CPU_Cycles = cycles;
			(*cpudecoder)();
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
			ResumeExecution();
		});
	}

	DosboxDebugger::Registers GetState(const ::Ice::Current& current) override
	{
		DosboxDebugger::Registers result = {};
		Do([&result] { GetRegisters(result); });
		return result;
	}

	DosboxDebugger::AssemblySequence Disassemble(DosboxDebugger::Address address, int length,
	                             const ::Ice::Current& current) override
	{
		auto result = DosboxDebugger::AssemblySequence();
		Do([&address, &length, &result] {
			printf("-> Disassemble(%x:%x, %d)\n", address.segment, address.offset, length);

			const PhysPt start = GetAddress(address.segment, address.offset);
			PhysPt cur = start;

			for (int i = 0; i < length; i++) {
				char buffer[200];
				const Bitu size = DasmI386(buffer, cur, cur, cpu.code.big);

				DosboxDebugger::AssemblyLine line;
				line.address.segment = address.segment;
				line.address.offset = (int)(address.offset + (cur - start));
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
			// printf("-> GetMemory(%x:%x, %d)\n", address.segment, address.offset, length);
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

	int GetMaxNonEmptyAddress(short seg, const Ice::Current& current) override
	{
		Descriptor desc;
		if (!cpu.gdt.GetDescriptor(seg, desc)) 
			return 0;

		const PhysPt minPage               = desc.GetBase() >> 12;
		const PhysPt maxPhysAddrForSegment = desc.GetBase() + desc.GetLimit();
		const PhysPt maxPage               = maxPhysAddrForSegment >> 12;

		for (PhysPt i = maxPage; i >= minPage; i--) {
			if (paging.tlb.read[i] != nullptr) {
				const PhysPt maxPhysAddrForPage = (i << 12) + 0xfff;
				const PhysPt segmentRelative = maxPhysAddrForPage - desc.GetBase();
				return (int)segmentRelative;
			}

			if (i == 0) {
				break;
			}
		}

		return 0;
	}

	DosboxDebugger::Addresses SearchMemory(
		DosboxDebugger::Address start,
		int length,
		DosboxDebugger::ByteSequence pattern,
		int advance,
		const Ice::Current& current) override
	{
		DosboxDebugger::Addresses results;
		if (pattern.empty())
			return results;

		if (advance == 0)
			advance = (int)pattern.size();

		if (length == -1) {
			const auto maxAddr = (PhysPt)GetMaxNonEmptyAddress(start.segment, current);
			length = (int)(maxAddr - start.offset);
		}

		Descriptor desc;
		if (!cpu.gdt.GetDescriptor(start.segment, desc)) 
			return results;

		const PhysPt maxPhysAddr = desc.GetBase() + desc.GetLimit();
		PhysPt p = desc.GetBase() + start.offset;
		if (p > maxPhysAddr)
			return results;

		PhysPt endAddr = p + length;
		if (endAddr > maxPhysAddr)
			endAddr = maxPhysAddr;

		while (p < endAddr) {
			const PhysPt pageNum = p >> 12;
			const auto pEnd = (PhysPt)(p + pattern.size() - 1);
			HostPt page = paging.tlb.read[pageNum];
			HostPt endPage = paging.tlb.read[pEnd >> 12];

			if (page == nullptr) {
				while (p >> 12 == pageNum) {
					p += advance;
				}
				continue;
			}

			if (page == endPage) {
				for (PhysPt i = 0; i < pattern.size(); i++) {
					const uint8_t val = page[p + i];
					if (pattern[i] != val)
						break;

					if (i == pattern.size() - 1)
					{
						DosboxDebugger::Address result = {};
						result.segment = start.segment;
						result.offset  = (int)(p - desc.GetBase());
						results.push_back(result);
					}
				}
			} else {
				for (PhysPt i = 0; i < pattern.size(); i++) {
					uint8_t val;
					mem_readb_checked(p + i, &val);
					if (pattern[i] != val)
						break;

					if (i == pattern.size() - 1)
					{
						DosboxDebugger::Address result = {};
						result.segment = start.segment;
						result.offset  = (int)(p - desc.GetBase());
						results.push_back(result);
					}
				}
			}

			p += advance;
		}
		
		return results;
	}

	DosboxDebugger::BreakpointSequence ListBreakpoints(const ::Ice::Current& current) override
	{
		DosboxDebugger::BreakpointSequence result;
		Do([&result] {
			for (auto it = CBreakpoint::begin(); it != CBreakpoint::end(); ++it) {
				result.push_back(DosboxDebugger::Breakpoint());
				auto& bp = result.back();

				bp.id              = (*it)->GetId();
				bp.address.segment = (short)(*it)->GetSegment();
				bp.address.offset  = (int)(*it)->GetOffset();
				bp.ah              = 0;
				bp.al              = 0;
				bp.enabled         = (*it)->IsEnabled();

				switch ((*it)->GetType()) {
				case BKPNT_PHYSICAL:
					bp.type = (*it)->GetOnce()
					            ? DosboxDebugger::BreakpointType::Ephemeral
					            : DosboxDebugger::BreakpointType::Normal;
					break;

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
		});
		return result;
	}

	void SetBreakpoint(DosboxDebugger::Breakpoint breakpoint, const ::Ice::Current& current) override
	{
		Do([&breakpoint] {
			const char* typeName = "Unk";
			const CBreakpoint *bp = nullptr;
			switch (breakpoint.type) {
			case DosboxDebugger::BreakpointType::Normal:
				typeName = "Normal";
				bp = CBreakpoint::AddBreakpoint(breakpoint.address.segment,
				                                breakpoint.address.offset,
				                                false);
				break;

			case DosboxDebugger::BreakpointType::Ephemeral:
				typeName = "Ephemeral";
				bp = CBreakpoint::AddBreakpoint(breakpoint.address.segment,
				                           breakpoint.address.offset,
				                           true);
				break;

			case DosboxDebugger::BreakpointType::Read:
				typeName = "Read";
				bp = CBreakpoint::AddMemBreakpoint(breakpoint.address.segment,
				                              breakpoint.address.offset);
				break;

			case DosboxDebugger::BreakpointType::Write: typeName = "Write"; break;

			case DosboxDebugger::BreakpointType::Interrupt:
				typeName = "Interrupt";
				bp = CBreakpoint::AddIntBreakpoint((uint8_t)breakpoint.address.offset,
				                              BPINT_ALL,
				                              BPINT_ALL,
				                              false);
				break;

			case DosboxDebugger::BreakpointType::InterruptWithAH:
				typeName = "IntAH";
				bp = CBreakpoint::AddIntBreakpoint((uint8_t)breakpoint.address.offset,
				                              breakpoint.ah,
				                              BPINT_ALL,
				                              false);
				break;

			case DosboxDebugger::BreakpointType::InterruptWithAX:
				typeName = "IntAX";
				bp = CBreakpoint::AddIntBreakpoint((uint8_t)breakpoint.address.offset,
				                              breakpoint.ah,
				                              breakpoint.al,
				                              false);
				break;

			case DosboxDebugger::BreakpointType::Unknown:
				break;
			}

			if (bp != nullptr && !breakpoint.enabled)
				CBreakpoint::EnableBreakpoint(bp->GetId(), false);

			printf("-> SetBreakpoint(%x:%x, %s, %d, %d, %s)\n",
			       breakpoint.address.segment,
			       breakpoint.address.offset,
			       typeName,
			       (int)breakpoint.ah,
			       (int)breakpoint.al,
			       breakpoint.enabled ? "enabled" : "disabled");
		});
	}

	void EnableBreakpoint(int id, bool enabled, const ::Ice::Current& current) override
	{
		Do([id, enabled] {
			printf("-> EnableBreakpoint(%d, %s)\n", id, enabled ? "true" : "false");
			CBreakpoint::EnableBreakpoint(id, enabled);
		});
	}

	void DelBreakpoint(int id, const ::Ice::Current& current) override
	{
		Do([id] {
			printf("-> DelBreakpoint(%d)\n", id);
			CBreakpoint::DeleteBreakpoint(id);
		});
	}

	void SetRegister(DosboxDebugger::Register reg, int value, const ::Ice::Current& current) override
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

	void AddDescriptor(DosboxDebugger::Descriptors& results, Descriptor& desc) const
	{
		if (desc.Type() & 0x04) { // Gate
			const auto result    = std::make_shared<DosboxDebugger::GateDescriptor>();
			result->type   = (DosboxDebugger::SegmentType)desc.Type();
			result->offset = (int)desc.GetOffset();
			result->selector = (short)desc.GetSelector();
			result->dpl      = desc.DPL();
			result->big      = !!desc.Big();
			results.push_back(result);
		} else { // Segment
			const auto result = std::make_shared<DosboxDebugger::SegmentDescriptor>();
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
		const Bitu length = cpu.gdt.GetLimit();
		PhysPt address    = cpu.gdt.GetBase();
		const auto max    = (PhysPt)(address + length);

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
		const Bitu ldtSelector = cpu.gdt.SLDT();

		if (!cpu.gdt.GetDescriptor(ldtSelector, desc)) {
			return results;
		}

		const Bitu length = desc.GetLimit();
		PhysPt address    = desc.GetBase();
		const PhysPt max  = (PhysPt)(address + length);
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
		const Ice::PropertiesPtr properties = Ice::createProperties();
		properties->setProperty("Ice.MessageSizeMax", "2097152");

		Ice::InitializationData initData = Ice::InitializationData();
		initData.properties = properties;

		const Ice::CommunicatorHolder ich(initData, ICE_INT_VERSION);
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
	DosboxDebugger::Registers state = {};
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
	const auto loop = DOSBOX_GetLoop();
	if (loop != lastLoop && loop == DEBUG_Loop) {
		AlertClients();
	}

	g_queue.process();

	// Don't need to send an alert if the debugger changed the state
	lastLoop = DOSBOX_GetLoop();
}
