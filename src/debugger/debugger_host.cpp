#include "debugger_internal.h"

#include <algorithm>
#include <future>

#if C_DEBUGGER && C_LIZARD_DEBUGGER

#include "debugger_inc.h"
#include <condition_variable>
#include <cstdio>
#include <list>
#include <mutex>
#include <thread>
#include <SDL_net.h>
#include "LizardComms.g.h"

using namespace LizardComms;

static bool IsStopped() { return DOSBOX_GetLoop() == DEBUG_Loop; }

static void GetRegisters(LRegisters1& result)
{
	result.is_stopped = IsStopped();
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
		std::unique_lock lock(mutex_);

		while (!callbacks_.empty()) {
			CallbackEntry entry = callbacks_.front();
			callbacks_.pop_front();
			entry();
		}
	}

	void add(std::function<void()> response)
	{
		const std::unique_lock lock(mutex_);
		callbacks_.emplace_back(std::move(response));
	}

	private:
	using CallbackEntry = std::function<void()>;

	std::mutex mutex_;
	std::list<CallbackEntry> callbacks_;
};

template<typename T>
class ClientList
{
	std::mutex m_;
	std::vector<std::shared_ptr<T>> clients_;

public:
	void Add(const std::shared_ptr<T>& duplex)
	{
		std::unique_lock ul(m_);
		clients_.push_back(duplex);
	}

	void Remove(const std::shared_ptr<T>& duplex)
	{
		std::unique_lock ul(m_);
		const auto& it =
			std::ranges::remove_if(
				clients_,
				[&duplex](const auto& x) { return x.get() == duplex.get(); }
			).begin();

		if (it != clients_.end()) {
			clients_.erase(it);
		}
	}

	void ForEach(const std::function<void(T&)>& func)
	{
		std::unique_lock ul(m_);
		for (const auto& it : clients_) {
			func(*it);
		}
	}
};

class SdlNetSocket : public ISocket
{
	TCPsocket m_socket;

public:
	explicit SdlNetSocket(TCPsocket socket) : m_socket(socket) {}
	ISocket* Accept() override
	{
		if (m_socket == nullptr)
			throw CommsError("Socket is closed");

		TCPsocket socket = SDLNet_TCP_Accept(m_socket);
		if (socket == nullptr)
			return nullptr;

		return new SdlNetSocket(socket);
	}

	void Send(std::span<const uint8_t> buffer) override
	{
		if (m_socket == nullptr)
			throw CommsError("Socket is closed");

		int size = static_cast<int>(buffer.size());
		int len = SDLNet_TCP_Send(m_socket, buffer.data(), size);
		if (len < size)
			throw CommsError(std::format("SDLNet_TCP_Send: {}", SDLNet_GetError()));
	}

	void Receive(std::span<uint8_t> buffer) override
	{
		if (m_socket == nullptr)
			throw CommsError("Socket is closed");

		uint32_t size = static_cast<uint32_t>(buffer.size());
		uint32_t totalReceived = 0;
		while (totalReceived < size)
		{
			int remaining = static_cast<int>(size - totalReceived);
			int bytesReceived = SDLNet_TCP_Recv(m_socket, buffer.data(), remaining);
			if (bytesReceived <= 0)
				throw CommsError(std::format("SDLNet_TCP_Recv: {}", SDLNet_GetError()));

			totalReceived += bytesReceived;
		}
	}

	void GetPeerAddress(uint32_t &ip, uint16_t& port) const override
	{
		if (m_socket == nullptr)
		{
			ip = 0;
			port = 0;
			return;
		}
			
		IPaddress* remoteip = SDLNet_TCP_GetPeerAddress(m_socket);
		if (!remoteip)
			throw CommsError(std::format("SDLNet_TCP_GetPeerAddress: {}", SDLNet_GetError()));

		ip = SDL_SwapBE32(remoteip->host);
		port = remoteip->port;
	}

	std::string GetPeerAddress() const override
	{
		uint32_t ip;
		uint16_t port;
		GetPeerAddress(ip, port);
		return std::format("{}.{}.{}.{}:{}", 
			ip >> 24,
			(ip >> 16) & 0xff,
			(ip >> 8) & 0xff,
			ip & 0xff,
			port);
	}

	void Close() override
	{
		if (m_socket)
		{
			SDLNet_TCP_Close(m_socket);
			m_socket = nullptr;
		}
	}

	~SdlNetSocket() override { SdlNetSocket::Close(); }
};

ISocket* CreateSdlNetServerSocket(uint16_t port)
{
	IPaddress ip;
	if (SDLNet_ResolveHost(&ip, nullptr, port) == -1)
		throw CommsError(std::format("SDLNet_ResolveHost: {}", SDLNet_GetError()));

	TCPsocket serverSocket = SDLNet_TCP_Open(&ip);
	if (!serverSocket)
		throw CommsError(std::format("SDLNet_TCP_Open: {}", SDLNet_GetError()));

	return new SdlNetSocket(serverSocket);
}

class ClientConnection : public ILizardClient1 {
	std::shared_ptr<IDuplex> duplex_;
	std::unique_ptr<ILizardClient1> serializer_;

public:
	explicit ClientConnection(const std::shared_ptr<IDuplex>& duplex)
	        : duplex_(duplex),
	          serializer_(MakeLizardClient1Serializer([&](std::vector<uint8_t>& buffer) {
		          duplex_->SendAndReplaceBufferWithResponse(buffer);
	          }))
	{
	}

	~ClientConnection() override = default;
        void Stopped(LRegisters1& state) override
	{
		serializer_->Stopped(state);
	}

	void Disconnect()
	{
		duplex_->Disconnect();
	}
};

class ConsoleLogger final : public ILogger
{
	LogLevel m_level;

	static void Log(std::string_view level, std::string_view message)
	{
		printf("%.*s %.*s\n",
			(int)level.size(),
			level.data(),
			static_cast<int>(message.size()),
			message.data());
	}

public:
	explicit ConsoleLogger(LogLevel level) : m_level(level) {}
	~ConsoleLogger() override = default;
	void Debug(std::string_view message) override { if (m_level <= LizardComms::Debug) Log("[DEBUG]", message); }
	void Info(std::string_view message)  override { if (m_level <= LizardComms::Info)  Log("[INFO] ", message); }
	void Warn(std::string_view message)  override { if (m_level <= LizardComms::Warn)  Log("[WARN] ", message); }
	void Error(std::string_view message) override { if (m_level <= LizardComms::Error) Log("[ERROR]", message); }
	void Fatal(std::string_view message) override { if (m_level <= LizardComms::Fatal) Log("[FATAL]", message); }
};

static std::shared_ptr<ILogger> log_(new ConsoleLogger(Debug));
static WorkQueue pendingRequests_;
static ClientList<ClientConnection> clients_;
static ManualResetEvent done_;
static std::unique_ptr<std::thread> g_debugThread;

class LizardImpl : public ILizard1 {
public:
	void Continue() override
	{
		Do([] {
			printf("-> Continue\n");
			ResumeExecution();
		});
	}

	LRegisters1 Break() override
	{
		LRegisters1 result = {};
		Do([&result] {
			printf("-> Break\n");
			DOSBOX_SetLoop(&DEBUG_Loop);
			GetRegisters(result);
		});
		return result;
	}

	LRegisters1 StepIn() override
	{
		LRegisters1 result = {};
		Do([&result] {
			printf("-> StepIn\n");
			CPU_Cycles = 1;
			(*cpudecoder)();
			GetRegisters(result);
		});
		return result;
	}

	LRegisters1 StepOut() override
	{
		// TODO
		LRegisters1 result = {};
		Do([&result] {
			printf("-> StepOut\n");
			CPU_Cycles = 1;
			(*cpudecoder)();
			GetRegisters(result);
		});
		return result;
	}

	LRegisters1 StepOver() override
	{
		LRegisters1 result = {};
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

	LRegisters1 StepMultiple(uint32_t cycles) override
	{
		LRegisters1 result = {};
		Do([&cycles, &result] {
			printf("-> StepMultiple(%d)\n", cycles);
			CPU_Cycles = cycles;
			(*cpudecoder)();
			GetRegisters(result);
		});
		return result;
	}

	void RunToAddress(LAddress1& address) override
	{
		Do([&address] {
			printf("-> RunToAddress(%x:%x)\n", (int)address.segment, address.offset);
			if (!CBreakpoint::FindPhysBreakpoint(address.segment, address.offset, true)) {
				CBreakpoint::AddBreakpoint(address.segment, address.offset, true);
			}
			ResumeExecution();
		});
	}

	LRegisters1 GetState() override
	{
		LRegisters1 result = {};
		Do([&result] { GetRegisters(result); });
		return result;
	}

	std::vector<LAssemblyLine1> Disassemble(LAddress1& address, uint32_t length) override
	{
		auto result = std::vector<LAssemblyLine1>();
		Do([&address, &length, &result] {
			printf("-> Disassemble(%x:%x, %d)\n", address.segment, address.offset, length);

			const PhysPt start = GetAddress(address.segment, address.offset);
			PhysPt cur = start;

			for (uint32_t i = 0; i < length; i++) {
				char buffer[200];
				const Bitu size = DasmI386(buffer, cur, cur, cpu.code.big);

				LAssemblyLine1 line;
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

	std::vector<uint8_t> GetMemory(LAddress1& address, uint32_t length) override
	{
		auto result = std::vector<uint8_t>(length);
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

	void SetMemory(LAddress1& address, std::vector<uint8_t>& bytes) override
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

	uint32_t GetMaxNonEmptyAddress(uint16_t seg) override
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

	std::vector<LAddress1> SearchMemory(
		LAddress1& start,
		uint32_t length,
		std::vector<uint8_t>& pattern,
		uint32_t advance) override
	{
		std::vector<LAddress1> results;
		if (pattern.empty())
			return results;

		if (advance == 0)
			advance = (int)pattern.size();

		if (length == 0xffffffff) {
			const auto maxAddr = (PhysPt)GetMaxNonEmptyAddress(start.segment);
			length = (int)(maxAddr - start.offset);
		}

		Descriptor desc;
		if (!cpu.gdt.GetDescriptor(start.segment, desc)) 
			return results;

		const PhysPt maxPhysAddr = desc.GetBase() + desc.GetLimit();
		PhysPt p = desc.GetBase() + start.offset;
		if (p > maxPhysAddr)
			return results;

		PhysPt endAddr = std::min(p + length, maxPhysAddr);

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
						LAddress1 result = {};
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
						LAddress1 result = {};
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

	std::vector<LBreakpoint1> ListBreakpoints() override
	{
		std::vector<LBreakpoint1> result;
		Do([&result] {
			for (auto it = CBreakpoint::begin(); it != CBreakpoint::end(); ++it) {
				result.emplace_back();
				auto& bp = result.back();

				bp.id              = (*it)->GetId();
				bp.address.segment = (short)(*it)->GetSegment();
				bp.address.offset  = (int)(*it)->GetOffset();
				bp.ah              = 0;
				bp.al              = 0;
				bp.is_enabled      = (*it)->IsEnabled();

				switch ((*it)->GetType()) {
				case BKPNT_PHYSICAL:
					bp.type = (*it)->GetOnce()
					            ? LBreakpointType1::Ephemeral
					            : LBreakpointType1::Normal;
					break;

				case BKPNT_INTERRUPT:
					if ((*it)->GetValue() == BPINT_ALL) {
						bp.type = LBreakpointType1::Interrupt;
					} else if ((*it)->GetOther() == BPINT_ALL) {
						bp.type = LBreakpointType1::InterruptWithAH;
						bp.ah   = (unsigned char)(*it)->GetValue();
					} else {
						bp.type = LBreakpointType1::InterruptWithAX;
						bp.ah   = (unsigned char)(*it)->GetValue();
						bp.al   = (unsigned char)(*it)->GetOther();
					}
					break;

				case BKPNT_MEMORY: bp.type = LBreakpointType1::Read; break;
				case BKPNT_MEMORY_READ: // TODO
					bp.type = LBreakpointType1::Unknown;
					break;

				case BKPNT_UNKNOWN:
				case BKPNT_MEMORY_PROT:
				case BKPNT_MEMORY_LINEAR:
					bp.type = LBreakpointType1::Unknown;
					break;
				}
			}
		});
		return result;
	}

	void SetBreakpoint(LBreakpoint1& breakpoint) override
	{
		Do([&breakpoint] {
			const char* typeName = "Unk";
			const CBreakpoint *bp = nullptr;
			switch (breakpoint.type) {
			case LBreakpointType1::Normal:
				typeName = "Normal";
				bp = CBreakpoint::AddBreakpoint(breakpoint.address.segment,
				                                breakpoint.address.offset,
				                                false);
				break;

			case LBreakpointType1::Ephemeral:
				typeName = "Ephemeral";
				bp = CBreakpoint::AddBreakpoint(breakpoint.address.segment,
				                           breakpoint.address.offset,
				                           true);
				break;

			case LBreakpointType1::Read:
				typeName = "Read";
				bp = CBreakpoint::AddMemBreakpoint(breakpoint.address.segment,
				                              breakpoint.address.offset);
				break;

			case LBreakpointType1::Write: typeName = "Write"; break;

			case LBreakpointType1::Interrupt:
				typeName = "Interrupt";
				bp = CBreakpoint::AddIntBreakpoint((uint8_t)breakpoint.address.offset,
				                              BPINT_ALL,
				                              BPINT_ALL,
				                              false);
				break;

			case LBreakpointType1::InterruptWithAH:
				typeName = "IntAH";
				bp = CBreakpoint::AddIntBreakpoint((uint8_t)breakpoint.address.offset,
				                              breakpoint.ah,
				                              BPINT_ALL,
				                              false);
				break;

			case LBreakpointType1::InterruptWithAX:
				typeName = "IntAX";
				bp = CBreakpoint::AddIntBreakpoint((uint8_t)breakpoint.address.offset,
				                              breakpoint.ah,
				                              breakpoint.al,
				                              false);
				break;

			case LBreakpointType1::Unknown:
				break;
			}

			if (bp != nullptr && !breakpoint.is_enabled)
				CBreakpoint::EnableBreakpoint(bp->GetId(), false);

			printf("-> SetBreakpoint(%x:%x, %s, %d, %d, %s)\n",
			       breakpoint.address.segment,
			       breakpoint.address.offset,
			       typeName,
			       (int)breakpoint.ah,
			       (int)breakpoint.al,
			       breakpoint.is_enabled ? "enabled" : "disabled");
		});
	}

	void EnableBreakpoint(uint32_t id, bool enabled) override
	{
		Do([id, enabled] {
			printf("-> EnableBreakpoint(%d, %s)\n", id, enabled ? "true" : "false");
			CBreakpoint::EnableBreakpoint(id, enabled);
		});
	}

	void DeleteBreakpoint(uint32_t id) override
	{
		Do([id] {
			printf("-> DelBreakpoint(%d)\n", (int)id);
			CBreakpoint::DeleteBreakpoint((int)id);
		});
	}

	void SetRegister(LRegister1 reg, uint32_t value) override
	{
		Do([&reg, &value] {
			const char* regName = "";
			switch (reg) {
			case LRegister1::Flags: regName = "Flags"; break;
			case LRegister1::EAX: regName = "EAX"; break;
			case LRegister1::EBX: regName = "EBX"; break;
			case LRegister1::ECX: regName = "ECX"; break;
			case LRegister1::EDX: regName = "EDX"; break;
			case LRegister1::ESI: regName = "ESI"; break;
			case LRegister1::EDI: regName = "EDI"; break;
			case LRegister1::EBP: regName = "EBP"; break;
			case LRegister1::ESP: regName = "ESP"; break;
			case LRegister1::EIP: regName = "EIP"; break;
			case LRegister1::ES: regName = "ES"; break;
			case LRegister1::CS: regName = "CS"; break;
			case LRegister1::SS: regName = "SS"; break;
			case LRegister1::DS: regName = "DS"; break;
			case LRegister1::FS: regName = "FS"; break;
			case LRegister1::GS: regName = "GS"; break;
			}

			printf("-> SetReg(%s, %x)\n", regName, value);
		});
	}

	void AddDescriptor(std::vector<LDescriptor1>& results, Descriptor& desc) const
	{
		if (desc.Type() & 0x04) { // Gate
			LDescriptor1 result;
			result.type     = (LDescriptorType1)desc.Type();
			result.offset   = desc.GetOffset();
			result.selector = desc.GetSelector();
			result.dpl      = desc.DPL();
			result.is_big   = !!desc.Big();
			results.push_back(result);
		} else { // Segment
			LDescriptor1 result;
			result.type     = (LDescriptorType1)desc.Type();
			result.offset   = desc.GetBase();
			result.selector = desc.GetLimit();
			result.dpl      = desc.DPL();
			result.is_big   = desc.Big();
			results.push_back(result);
		}
	}

	std::vector<LDescriptor1> GetGdt() override
	{
		Descriptor desc;
		const Bitu length = cpu.gdt.GetLimit();
		PhysPt address    = cpu.gdt.GetBase();
		const auto max    = (PhysPt)(address + length);

		std::vector<LDescriptor1> results;
		while (address < max) {
			desc.Load(address);
			AddDescriptor(results, desc);
			address += 8;
		}
		return results;
	}

	std::vector<LDescriptor1> GetLdt() override
	{
		std::vector<LDescriptor1> results;
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
		pendingRequests_.add([&mre, &func] {
			func();
			mre.set();
		});

		mre.wait();
	}
};

void RunServerLoopV1(std::unique_ptr<ISocket> socket, const std::shared_ptr<ILogger>& log)
{
	LizardImpl behavior;
	std::unique_ptr<IDeserializer> deserializer(MakeLizard1Deserializer(behavior));

	std::shared_ptr<IDuplex> duplex(
		CreateDuplex(
			std::move(socket),
			log,
			[&deserializer](std::vector<uint8_t>& buffer)
			{
				deserializer->HandleMessage(buffer);
			}));

	std::shared_ptr<ClientConnection> clientConn = std::make_shared<ClientConnection>(duplex);
	clients_.Add(clientConn);
	duplex->WaitForExit();
	clients_.Remove(clientConn);
}

void AcceptorLoop(std::unique_ptr<ISocket> serverSocket)
{
	while (!done_.get())
	{
		std::unique_ptr<ISocket> socket(serverSocket->Accept());
		if (!socket)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			continue;
		}

		log_->Info(std::format("Accepted a connection from {}", socket->GetPeerAddress()));

		constexpr uint8_t maxServerVersion = 1;
		uint8_t version = ServerHandshake(*socket, maxServerVersion);
		switch (version)
		{
		case 1:
			RunServerLoopV1(std::move(socket), log_); // Server is responsible for closing the client socket
			break;

		default:
			throw CommsError(std::format("Unsupported minimum comms version {}", version));
		}
	}

	log_->Info("Server stopped");
}

void AlertClients()
{
	LRegisters1 state = {};
	GetRegisters(state);
	clients_.ForEach([&state](ClientConnection& client) {
		client.Stopped(state);
	});
}

void DEBUG_StartHost()
{
	if (g_debugThread) {
		return;
	}

	if (SDLNet_Init() == -1) {
		log_->Error(std::format("Failed to open socket - SDLNet_Init returned {}", SDLNet_GetError()));
		return;
	}

	uint16_t port = 7243; // TODO: Config
	log_->Info("Starting server...");
	std::unique_ptr<ISocket> serverSocket(CreateSdlNetServerSocket(port));
	g_debugThread = std::make_unique<std::thread>(&AcceptorLoop, std::move(serverSocket));
}

void DEBUG_StopHost()
{
	if (!g_debugThread) {
		return;
	}

	done_.set();
	clients_.ForEach([](ClientConnection& client) { client.Disconnect(); });
	g_debugThread->join();
	g_debugThread.reset();
	SDLNet_Quit();
}

static LoopHandler* lastLoop;
void DEBUG_PollWork()
{
	const auto loop = DOSBOX_GetLoop();
	if (loop != lastLoop && loop == DEBUG_Loop) {
		AlertClients();
	}

	pendingRequests_.process();

	// Don't need to send an alert if the debugger changed the state
	lastLoop = DOSBOX_GetLoop();
}
#endif
