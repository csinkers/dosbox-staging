using System.Globalization;
using System.Text;

namespace DosboxDebugger;

static class CommandParser
{
    static uint ParseOffset(string s, Debugger d, out int segmentHint)
    {
        segmentHint = 0;
        if (!uint.TryParse(s, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var offset))
        {
            var upper = s.ToUpperInvariant();
            switch (upper)
            {
                case "EAX": segmentHint = d.Registers.ds; return (uint)d.Registers.eax;
                case "EBX": segmentHint = d.Registers.ds; return (uint)d.Registers.ebx;
                case "ECX": segmentHint = d.Registers.ds; return (uint)d.Registers.ecx;
                case "EDX": segmentHint = d.Registers.ds; return (uint)d.Registers.edx;
                case "ESI": segmentHint = d.Registers.ds; return (uint)d.Registers.esi;
                case "EDI": segmentHint = d.Registers.ds; return (uint)d.Registers.edi;
                case "EBP": segmentHint = d.Registers.ss; return (uint)d.Registers.ebp;
                case "ESP": segmentHint = d.Registers.ss; return (uint)d.Registers.esp;
                case "EIP": segmentHint = d.Registers.cs; return (uint)d.Registers.eip;
            }

            if (!d.TryFindSymbol(s, out offset))
                throw new FormatException($"Could not resolve an address for \"{s}\"");
        }

        return offset;
    }

    static Address ParseAddress(string s, Debugger d, bool code)
    {
        int index = s.IndexOf(':');
        uint offset;
        int segment;

        if (index == -1)
        {
            offset = ParseOffset(s, d, out segment);
            if (segment == 0)
                segment = code ? d.Registers.cs : d.Registers.ds;
        }
        else
        {
            var part = s[..index];
            if (!int.TryParse(part, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out segment))
            {
                segment = part.ToUpperInvariant() switch
                {
                    "CS" => d.Registers.cs,
                    "DS" => d.Registers.ds,
                    "SS" => d.Registers.ss,
                    "ES" => d.Registers.es,
                    "FS" => d.Registers.fs,
                    "GS" => d.Registers.gs,
                    _ => throw new FormatException($"Invalid segment \"{part}\"")
                };
            }

            offset = ParseOffset(s[(index+1)..], d, out _);
        }

        var signedOffset = unchecked((int)offset);
        return new Address(unchecked((short)segment), signedOffset);
    }

    static int ParseVal(string s)
    {
        if (s.StartsWith("0x"))
            return int.Parse(s[2..], NumberStyles.HexNumber);

        if (s.StartsWith("0"))
            return int.Parse(s[1..], NumberStyles.HexNumber);

        return int.Parse(s);
    }

    static void PrintAsm(AssemblyLine[] lines, ITracer log)
    {
        foreach (var line in lines)
            log.Debug($"{line.address.segment:X}:{line.address.offset:X8} {line.line}");
    }

    static void PrintBytes(Address address, byte[] bytes, ITracer log)
    {
        const string hexChars = "0123456789ABCDEF";
        var result = new StringBuilder(bytes.Length * 2);
        var chars = new StringBuilder(16);
        for (var i = 0; i < bytes.Length; i++)
        {
            if (i % 16 == 0)
            {
                result.Append(chars);
                chars.Clear();
                result.AppendLine();
                result.Append($"{address.segment:X}:{address.offset + i:X8}: ");
            }

            var b = bytes[i];
            result.Append(hexChars[b >> 4]);
            result.Append(hexChars[b & 0xf]);
            result.Append(i % 2 == 0 ? '-' : ' ');
            char c = (char)b;
            if (c < 0x20 || c > 0x7f) c = '.';
            chars.Append(c);
        }

        if (chars.Length > 0)
            result.Append(chars);

        log.Debug(result.ToString());
    }

    static void PrintBps(Breakpoint[] breakpoints, ITracer log)
    {
        foreach (var bp in breakpoints)
            log.Debug($"{bp.address.segment:X}:{bp.address.offset:X8} {bp.type} {bp.ah:X2} {bp.al:X2}");
    }

    static void PrintDescriptors(Descriptor[] descriptors, bool ldt, ITracer log)
    {
        for(int i = 0; i < descriptors.Length; i++)
        {
            var descriptor = descriptors[i];
            switch (descriptor.type)
            {
                case SegmentType.SysInvalid:
                    break;

                case SegmentType.Sys286CallGate:
                case SegmentType.SysTaskGate:
                case SegmentType.Sys286IntGate:
                case SegmentType.Sys286TrapGate:
                case SegmentType.Sys386CallGate:
                case SegmentType.Sys386IntGate:
                case SegmentType.Sys386TrapGate:
                    var gate = (GateDescriptor)descriptor;
                    log.Debug($"{i:X4} {gate.type} {(gate.big ? "32" : "16")} {gate.selector:X4}: {gate.offset:X8} R{gate.dpl}");
                    break;

                default:
                    var seg = (SegmentDescriptor)descriptor;
                    ushort selector = (ushort)((i << 3) | seg.dpl);
                    if (ldt)
                        selector |= 4;
                    log.Debug($"{i:X4}={selector:X4} {seg.type} {(seg.big ? "32" : "16")} {seg.@base:X8} {seg.limit:X8} R{seg.dpl}");
                    break;
            }
        }
    }

    static Register ParseReg(string s) =>
        s.ToUpperInvariant() switch
        {
            "Flags" => Register.Flags,
            "EAX" => Register.EAX,
            "EBX" => Register.EBX,
            "ECX" => Register.ECX,
            "EDX" => Register.EDX,
            "ESI" => Register.ESI,
            "EDI" => Register.EDI,
            "EBP" => Register.EBP,
            "ESP" => Register.ESP,
            "EIP" => Register.EIP,
            "ES" => Register.ES,
            "CS" => Register.CS,
            "SS" => Register.SS,
            "DS" => Register.DS,
            "FS" => Register.FS,
            "GS" => Register.GS,
            _ => throw new FormatException($"Unexpected register \"{s}\"")
        };

    static BreakpointType ParseBpType(string s) =>
        s.ToUpperInvariant() switch
        {
            "NORMAL" => BreakpointType.Normal,
            "N" => BreakpointType.Normal,
            "X" => BreakpointType.Normal,
            "READ" => BreakpointType.Read,
            "R" => BreakpointType.Read,
            "WRITE" => BreakpointType.Write,
            "W" => BreakpointType.Write,
            "INTERRUPT" => BreakpointType.Interrupt,
            "INT" => BreakpointType.Interrupt,
            "INTERRUPTWITHAH" => BreakpointType.InterruptWithAH,
            "INTAH" => BreakpointType.InterruptWithAH,
            "INTAL" => BreakpointType.InterruptWithAX,
            _ => throw new FormatException($"Unexpected breakpoint type \"{s}\"")
        };

    static void UpdateRegisters(Registers reg, Debugger d)
    {
        d.Registers = reg;
        var flagsSb = new StringBuilder();
        var flags = (CpuFlags)reg.flags;
        if ((flags & CpuFlags.CF) != 0) flagsSb.Append(" C");
        if ((flags & CpuFlags.ZF) != 0) flagsSb.Append(" Z");
        if ((flags & CpuFlags.SF) != 0) flagsSb.Append(" S");
        if ((flags & CpuFlags.OF) != 0) flagsSb.Append(" O");
        if ((flags & CpuFlags.AF) != 0) flagsSb.Append(" A");
        if ((flags & CpuFlags.PF) != 0) flagsSb.Append(" P");

        if ((flags & CpuFlags.DF) != 0) flagsSb.Append(" D");
        if ((flags & CpuFlags.IF) != 0) flagsSb.Append(" I");
        if ((flags & CpuFlags.TF) != 0) flagsSb.Append(" T");

        d.Log.Debug($"EAX {reg.eax:X8} ESI {reg.esi:X8} DS {reg.ds:X4} ES {reg.es:X4}");
        d.Log.Debug($"EBX {reg.ebx:X8} EDI {reg.edi:X8} FS {reg.fs:X4} GS {reg.gs:X4}");
        d.Log.Debug($"ECX {reg.ecx:X8} EBP {reg.ebp:X8}");
        d.Log.Debug($"EDX {reg.edx:X8} ESP {reg.esp:X8} SS {reg.ss:X4}");
        d.Log.Debug($"CS {reg.cs:X4} EIP {reg.eip:X8}{flagsSb}");
    }

    static readonly Dictionary<string, Command> Commands = new Command[]
        {
            new(new[] {"help", "?"}, "Show help", (_,  d) =>
            {
                var commands = Commands.Values.Distinct().OrderBy(x => x.Names[0]).ToList();
                int maxLength = 0;
                foreach (var cmd in commands)
                {
                    var length = cmd.Names.Sum(x => x.Length) + cmd.Names.Length - 1;
                    if (length > maxLength)
                        maxLength = length;
                }

                foreach (var cmd in commands)
                {
                    var names = string.Join(" ", cmd.Names);
                    var pad = new string(' ', maxLength - names.Length);
                    d.Log.Debug($"{names}{pad}: {cmd.Description}");
                }
            }),
            new(new []  { "clear", "cls" }, "Clear the log history", (_,  d) => d.Log.Clear()),

            new(new[] { "Connect", "!" }, "Register the callback proxy for 'breakpoint hit' alerts",
                (_,  d) => d.Host.Connect(d.DebugClientPrx)),
            new(new[] { "Continue", "g" }, "Resume execution", (_,  d) => d.Host.Continue()),

            // TODO
            new(new[] { "Break", "b" }, "Pause execution", (_,  d) => UpdateRegisters(d.Host.Break(), d)),
            new(new[] { "StepOver", "p" }, "Steps to the next instruction, ignoring function calls / interrupts etc", (_,  _) => { }),
            new(new[] { "StepIn", "n" }, "Steps to the next instruction, including into function calls etc", (_,  d) => UpdateRegisters(d.Host.StepIn(), d)),
            new(new[] { "StepMultiple", "gn" }, "Runs the CPU for the given number of cycles", (getArg, d) =>
            {
                var n = ParseVal(getArg());
                UpdateRegisters(d.Host.StepMultiple(n), d);
            }),
            new(new[] { "StepOut", "go" }, "Run until the current function returns", (_,  _) => { }),
            new(new[] { "RunToCall", "gc" }, "Run until the next 'call' instruction is encountered", (_,  _) => { }),

            new(new[] { "RunToAddress", "ga" }, "Run until the given address is reached", (getArg,  d) =>
            {
                var address = ParseAddress(getArg(), d, true);
                d.Host.RunToAddress(address);
            }),

            new(new[] { "GetState", "r" }, "Get the current CPU state", (_,  d) => UpdateRegisters(d.Host.GetState(), d)),
            new(new[] { "Disassemble", "u" }, "Disassemble instructions at the given address", (getArg,  d) =>
            {
                var addressArg = getArg();
                var address = addressArg == "" 
                    ? new Address(d.Registers.cs, d.Registers.eip)
                    : ParseAddress(addressArg, d, true);

                var lengthArg = getArg();
                var length = addressArg == "" || lengthArg == ""
                    ? 10
                    : ParseVal(lengthArg);

                PrintAsm(d.Host.Disassemble(address, length), d.Log);
            }),

            new(new[] { "GetMemory", "d" }, "Gets the contents of memory at the given address", (getArg,  d) =>
            {
                var address = ParseAddress(getArg(), d, false);
                var lengthArg = getArg();
                var length = lengthArg == ""
                    ? 64
                    : ParseVal(lengthArg);

                PrintBytes(address, d.Host.GetMemory(address, length), d.Log);
            }),
            new(new[] { "SetMemory", "e" }, "Changes the contents of memory at the given address", (getArg,  d) =>
            {
                var address = ParseAddress(getArg(), d, false);
                var value = ParseVal(getArg());
                var bytes = BitConverter.GetBytes(value);
                d.Host.SetMemory(address, bytes);
            }),

            new(new[] { "ListBreakpoints", "bps", "bl" }, "Retrieves the current breakpoint list", (_,  d) => PrintBps(d.Host.ListBreakpoints(), d.Log)),
            new(new[] { "SetBreakpoint", "bp" }, "<address> [type] [ah] [al] - Sets or updates a breakpoint", (getArg,  d) =>
            {
                var address = ParseAddress(getArg(), d, true);
                var s = getArg();
                var type = s == "" ? BreakpointType.Normal : ParseBpType(getArg());

                s = getArg();
                byte ah = s == "" ? (byte)0 : (byte)ParseVal(s);

                s = getArg();
                byte al = s == "" ? (byte)0 : (byte)ParseVal(s);

                var bp = new Breakpoint(address, type, ah, al);
                d.Host.SetBreakpoint(bp);
            }),

            new(new[] { "DelBreakpoint", "bd" }, "Removes the breakpoint at the given address", (getArg,  d) =>
            {
                var addr = ParseAddress(getArg(), d, true);
                d.Host.DelBreakpoint(addr);
            }),

            new(new[] { "SetReg", "reg" }, "Updates the contents of a CPU register", (getArg,  d) =>
            {
                Register reg = ParseReg(getArg());
                int value = ParseVal(getArg());
                d.Host.SetReg(reg, value);
            }),

            new(new[] { "GetGDT", "gdt"}, "Retrieves the Global Descriptor Table", (getArg, d) =>
            {
                PrintDescriptors(d.Host.GetGdt(), false, d.Log);
            }),

            new(new[] { "GetLDT", "ldt"}, "Retrieves the Local Descriptor Table", (getArg, d) =>
            {
                PrintDescriptors(d.Host.GetLdt(), true, d.Log);
            })
        }.SelectMany(x => x.Names.Select(name => (name, x)))
        .ToDictionary(x => x.name.ToUpperInvariant(), x => x.x);

    public static void RunCommand(string line, Debugger d)
    {
        try
        {
            if (string.IsNullOrWhiteSpace(line))
                return;

            var parts = line.Split(' ');
            var name = parts[0].ToUpperInvariant();

            if (Commands.TryGetValue(name, out var command))
            {
                int curArg = 1;
                command.Func(() => curArg >= parts.Length ? "" : parts[curArg++], d);
            }
            else d.Log.Error($"Unknown command \"{parts[0]}\"");
        }
        catch (Exception ex)
        {
            d.Log.Error("Parse error: " + ex.Message);
        }
    }
}