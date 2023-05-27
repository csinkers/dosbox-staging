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
		Unknown,
		Normal,
		Read,
		Write,
		Interrupt,
		InterruptWithAH,
		InterruptWithAX
	}

	struct Breakpoint
	{
		Address address;
		BreakpointType type; 
		byte ah;
		byte al;
	}

	struct AssemblyLine { Address address; string line; }
	sequence<AssemblyLine> AssemblySequence;
	sequence<Breakpoint> BreakpointSequence;

    interface DebugClient
    {
		void Stopped(Registers state);
    }

    interface DebugHost
    {
		void Connect(DebugClient* proxy);
		void Continue();
		Registers Break();
		Registers StepIn();
		Registers StepMultiple(int cycles);
		void RunToAddress(Address address);
		Registers GetState();

		AssemblySequence Disassemble(Address address, int length);
		ByteSequence GetMemory(Address address, int length);
		void SetMemory(Address address, ByteSequence bytes);

		BreakpointSequence ListBreakpoints();
		void SetBreakpoint(Breakpoint breakpoint);
		void DelBreakpoint(Address address);

		void SetReg(Register reg, int value);
    }
}