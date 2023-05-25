module DosboxDebugger
{
	struct Address { short segment; int offset; }
	sequence<byte> ByteSequence;

	enum Register { Flags,
		EAX, EBX, ECX, EDX,
		ESI, EDI,
		EBP, ESP, EIP,
		ES, CS, SS, DS, FS, GS 
	}

	struct Registers {
		// CPU state? FPU?
		int flags;

		int eax; int ebx; int ecx; int edx;
		int esi; int edi;
		int ebp; int esp; int eip;

		short es; short cs; short ss;
		short ds; short fs; short gs;
	}

	enum BreakpointType 
	{
		Normal,
		Read,
		Write,
		Interrupt,
		InterruptWithAH,
		InterruptWithAX
	}

	struct Breakpoint
	{
		int number; // -1 = create new
		Address address;
		BreakpointType type; 
		byte ah;
		byte al;
	}

	struct AssemblyLine { Address address; string line; }
	sequence<AssemblyLine> AssemblySequence;
	sequence<Breakpoint> BreakpointSequence;

    interface DebugHost
    {
		void Connect();
		void Detach();
		void Exit();

		void Continue();
		Registers Break();
		Registers StepOver();
		Registers StepIn();
		Registers StepOut();
		Registers RunToCall();
		Registers RunToAddress(Address address);
		Registers GetState();

		AssemblySequence Disassemble(Address address, int length);
		ByteSequence GetMemory(Address address, int length);

		BreakpointSequence ListBreakpoints();
		void SetBreakpoint(Breakpoint breakpoint);
		void DelBreakpoint(int bpNumber);

		void SetReg(Register reg, int value);
    }

    interface DebugClient
    {
		void Running();
		void Stopped(Registers state);
    }
}