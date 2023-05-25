#include <Ice/Ice.h>
#include <cstdio>
#include "debug_protocol.h"

using namespace std;
using namespace DosboxDebugger;

class DebugHostImpl : public DebugHost
{
public:
	void Connect(const ::Ice::Current& current) override { printf("-> Connect\n"); }
	void Detach(const ::Ice::Current& current) override { printf("-> Detach\n"); }
	void Exit(const ::Ice::Current& current) override { printf("-> Exit\n"); }
	void Continue(const ::Ice::Current& current) override { printf("-> Continue\n"); } 
	Registers Break(const ::Ice::Current& current) override { printf("-> Break\n"); return Registers(); } 
	Registers StepOver(const ::Ice::Current& current) override { printf("-> StepOver\n"); return Registers(); }
	Registers StepIn(const ::Ice::Current& current) override { printf("-> StepIn\n"); return Registers(); }
	Registers StepOut(const ::Ice::Current& current) override { printf("-> StepOut\n"); return Registers(); }
	Registers RunToCall(const ::Ice::Current& current) override { printf("-> RunToCall\n"); return Registers(); }
	Registers RunToAddress(Address address, const ::Ice::Current& current) override { printf("-> RunToAddress\n"); return Registers(); }
	Registers GetState(const ::Ice::Current& current) override { printf("-> GetState\n"); return Registers(); }

	AssemblySequence Disassemble(Address address, int length, const ::Ice::Current& current) override
	{
        printf("-> Disassemble(%x:%x, %d)\n", address.segment, address.offset, length);
		return AssemblySequence();
	}

	ByteSequence GetMemory(Address address, int length, const ::Ice::Current& current) override
	{
        printf("-> GetMemory(%x:%x, %d)\n", address.segment, address.offset, length);
		return ByteSequence();
	}

	BreakpointSequence ListBreakpoints(const ::Ice::Current& current) override
	{
        printf("-> ListBreakpoints\n");
		return BreakpointSequence();
	}

	void SetBreakpoint(Breakpoint breakpoint, const ::Ice::Current& current) override
	{
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
        printf("-> DelBreakpoint(%d)\n", bpNumber);
	}

	void SetReg(Register reg, int value, const ::Ice::Current& current) override
	{
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
		default: ;
		}

        printf("-> SetReg(%s, %x)\n", regName, value);
	}
};

/*
int main(int argc, char* argv[])
{
	try
	{
		Ice::CommunicatorHolder ich(argc, argv);
		auto adapter = ich->createObjectAdapterWithEndpoints("DebugHostAdapter", "default -p 7243");
		auto servant = make_shared<DebugHostImpl>();
		adapter->add(servant, Ice::stringToIdentity("DebugHost"));
		adapter->activate();

		ich->waitForShutdown();
	}
	catch (const std::exception& e)
	{
		cerr << e.what() << endl;
		return 1;
	}

	return 0;
}
*/
