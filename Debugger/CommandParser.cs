using System.Globalization;
using System.Text;

namespace DosboxDebugger;

static class CommandParser
{
    static Address ParseAddress(string s)
    {
        int index = s.IndexOf(':');
        if (index == -1)
            throw new FormatException("Expected : in address");

        var segment = unchecked((short)ushort.Parse(s[..index], NumberStyles.HexNumber));
        var offset = unchecked((int)uint.Parse(s[(index + 1)..], NumberStyles.HexNumber));
        return new Address(segment, offset);
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

    static void PrintBytes(byte[] bytes, ITracer log)
    {
        const string hexChars = "0123456789ABCDEF";
        var result = new StringBuilder(bytes.Length * 2);
        for (var i = 0; i < bytes.Length; i++)
        {
            if (i % 16 == 0)
                result.AppendLine();

            var b = bytes[i];
            result.Append(hexChars[b >> 4]);
            result.Append(hexChars[b & 0xf]);
            result.Append(i % 2 == 0 ? ' ' : '-');
        }

        log.Debug(result.ToString());
    }

    static void PrintBps(Breakpoint[] breakpoints, ITracer log)
    {
        foreach (var bp in breakpoints)
            log.Debug($"{bp.address.segment:X}:{bp.address.offset:X8} {bp.type} {bp.ah:X2} {bp.al:X2}");
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
            new(new[] {"help", "?"}, "Show help", _ => d =>
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
            new(new []  { "clear", "cls" }, "Clear the log history", _ => d => d.Log.Clear()),

            new(new[] { "Connect", "!" }, "Register the callback proxy for 'breakpoint hit' alerts",
                _ => d => d.Host.Connect(d.DebugClientPrx)),
            new(new[] { "Continue", "g" }, "Resume execution", _ => d => d.Host.Continue()),

            // TODO
            new(new[] { "Break", "b" }, "Pause execution", _ => d => UpdateRegisters(d.Host.Break(), d)),
            new(new[] { "StepOver", "p" }, "Steps to the next instruction, ignoring function calls / interrupts etc", _ => d => { }),
            new(new[] { "StepIn", "n" }, "Steps to the next instruction, including into function calls etc", _ => d => UpdateRegisters(d.Host.StepIn(), d)),
            new(new[] { "StepMultiple", "gn" }, "Runs the CPU for the given number of cycles", getArg => d =>
            {
                var n = ParseVal(getArg());
                UpdateRegisters(d.Host.StepMultiple(n), d);
            }),
            new(new[] { "StepOut", "go" }, "Run until the current function returns", _ => d => { }),
            new(new[] { "RunToCall", "gc" }, "Run until the next 'call' instruction is encountered", _ => d => { }),

            new(new[] { "RunToAddress", "ga" }, "Run until the given address is reached", getArg => d =>
            {
                var address = ParseAddress(getArg());
                d.Host.RunToAddress(address);
            }),

            new(new[] { "GetState", "r" }, "Get the current CPU state", _ => d => UpdateRegisters(d.Host.GetState(), d)),
            new(new[] { "Disassemble", "u" }, "Disassemble instructions at the given address", getArg => d =>
            {
                var address = ParseAddress(getArg());
                var length = ParseVal(getArg());
                PrintAsm(d.Host.Disassemble(address, length), d.Log);
            }),

            new(new[] { "GetMemory", "d" }, "Gets the contents of memory at the given address", getArg => d =>
            {
                var address = ParseAddress(getArg());
                var length = ParseVal(getArg());
                PrintBytes(d.Host.GetMemory(address, length), d.Log);
            }),
            new(new[] { "SetMemory", "e" }, "Changes the contents of memory at the given address", getArg => d =>
            {
                var address = ParseAddress(getArg());
                var value = ParseVal(getArg());
                var bytes = BitConverter.GetBytes(value);
                d.Host.SetMemory(address, bytes);
            }),

            new(new[] { "ListBreakpoints", "bps", "bl" }, "Retrieves the current breakpoint list", _ => d => PrintBps(d.Host.ListBreakpoints(), d.Log)),
            new(new[] { "SetBreakpoint", "bp" }, "<address> [type] [ah] [al] - Sets or updates a breakpoint", getArg => d =>
            {
                var address = ParseAddress(getArg());
                var s = getArg();
                var type = s == "" ? BreakpointType.Normal : ParseBpType(getArg());

                s = getArg();
                byte ah = s == "" ? (byte)0 : (byte)ParseVal(s);

                s = getArg();
                byte al = s == "" ? (byte)0 : (byte)ParseVal(s);

                var bp = new Breakpoint(address, type, ah, al);
                d.Host.SetBreakpoint(bp);
            }),

            new(new[] { "DelBreakpoint", "bd" }, "Removes the breakpoint at the given address", getArg => d =>
            {
                var addr = ParseAddress(getArg());
                d.Host.DelBreakpoint(addr);
            }),

            new(new[] { "SetReg", "reg" }, "Updates the contents of a CPU register", getArg => d =>
            {
                Register reg = ParseReg(getArg());
                int value = ParseVal(getArg());
                d.Host.SetReg(reg, value);
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
                var func = command.BuildCommand(() => curArg >= parts.Length ? "" : parts[curArg++]);
                func(d);
            }
            else d.Log.Error($"Unknown command \"{parts[0]}\"");
        }
        catch (Exception ex)
        {
            d.Log.Error("Parse error: " + ex.Message);
        }
    }
}