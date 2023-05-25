using System.Globalization;
using System.Text;

namespace DosboxDebugger;

public delegate string GetArg();
public delegate void DebugCommand(DebugHostPrx host);
public delegate DebugCommand CommandBuilder(GetArg getArg);

public class Command
{
    public Command(string[] names, CommandBuilder buildCommand)
    {
        Names = names ?? throw new ArgumentNullException(nameof(names));
        BuildCommand = buildCommand ?? throw new ArgumentNullException(nameof(buildCommand));
    }

    public string[] Names { get; }
    public CommandBuilder BuildCommand { get; }
}

static class Program
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

    static void PrintAsm(AssemblyLine[] lines)
    {
        foreach (var line in lines)
            Console.WriteLine($"{line.address.segment:X}:{line.address.offset:X8} {line.line}");
    }

    static void PrintBytes(byte[] bytes)
    {
        const string HexChars = "0123456789ABCDEF";
        var result = new StringBuilder(bytes.Length * 2);
        for (var i = 0; i < bytes.Length; i++)
        {
            if (i % 16 == 0)
                result.AppendLine();

            var b = bytes[i];
            result.Append(HexChars[b >> 4]);
            result.Append(HexChars[b & 0xf]);
            result.Append(i % 2 == 0 ? ' ' : '-');
        }

        Console.WriteLine(result.ToString());
    }

    static void PrintBps(Breakpoint[] breakpoints)
    {
        foreach (var bp in breakpoints)
            Console.WriteLine($"{bp.number} {bp.address.segment:X}:{bp.address.offset:X8} {bp.type} {bp.ah:X2} {bp.al:X2}");
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

    static void PrintReg(Registers reg)
    {
        var flags = "Z C O";
        Console.WriteLine($"EAX {reg.eax:X8} ESI {reg.esi:X8}");
        Console.WriteLine($"EBX {reg.ebx:X8} EDI {reg.edi:X8}");
        Console.WriteLine($"ECX {reg.ecx:X8} EBP {reg.ebp:X8}");
        Console.WriteLine($"EDX {reg.edx:X8} ESP {reg.esp:X8}");
        Console.WriteLine($"EIP {reg.eip:X8} {flags}");
    }

    static readonly Dictionary<string, Command> Commands = new Command[]
    {
        new(new[] { "Connect" }, _ => host => host.Connect()),
        new(new[] { "Detach" }, _ => host => host.Detach()),
        new(new[] { "Exit", "q" }, _ => host => host.Exit()),
        new(new[] { "Continue", "go", "g" }, _ => host => host.Continue()),
        new(new[] { "Break", "b" }, _ => host => PrintReg(host.Break())),
        new(new[] { "StepOver", "p" }, _ => host => PrintReg(host.StepOver())),
        new(new[] { "StepIn", "n" }, _ => host => PrintReg(host.StepIn())),
        new(new[] { "StepOut", "go" }, _ => host => PrintReg(host.StepOut())),
        new(new[] { "RunToCall", "gc" }, _ => host => PrintReg(host.RunToCall())),

        new(new[] { "RunToAddress", "ga" }, getArg => host =>
        {
            var address = ParseAddress(getArg());
            PrintReg(host.RunToAddress(address));
        }),

        new(new[] { "GetState", "r" }, _ => host => PrintReg(host.GetState())),
        new(new[] { "Disassemble", "u" }, getArg => host =>
        {
            var address = ParseAddress(getArg());
            var length = ParseVal(getArg());
            PrintAsm(host.Disassemble(address, length));
        }),

        new(new[] { "GetMemory", "d" }, getArg => host =>
        {
            var address = ParseAddress(getArg());
            var length = ParseVal(getArg());
            PrintBytes(host.GetMemory(address, length));
        }),

        new(new[] { "ListBreakpoints", "bps" }, _ => host => PrintBps(host.ListBreakpoints())),
        new(new[] { "SetBreakpoint", "bp" }, getArg => host =>
        {
            var num = int.Parse(getArg());
            var address = ParseAddress(getArg());
            var type = ParseBpType(getArg());
            var bp = new Breakpoint(num, address, type, 0, 0);
            host.SetBreakpoint(bp);
        }),

        new(new[] { "DelBreakpoint", "bd" }, getArg => host =>
        {
            int num = int.Parse(getArg());
            host.DelBreakpoint(num);
        }),

        new(new[] { "SetReg", "reg" }, getArg => host =>
        {
            Register reg = ParseReg(getArg());
            int value = ParseVal(getArg());
            host.SetReg(reg, value);
        })
    }.SelectMany(x => x.Names.Select(name => (name, x)))
     .ToDictionary(x => x.name, x => x.x);

    static void RunCommand(string line, DebugHostPrx debugHost)
    {
        try
        {
            var parts = line.Split(' ');
            if (parts.Length == 0)
                return;

            var name = parts[0].ToUpperInvariant();
            if (Commands.TryGetValue(name, out var command))
            {
                int curArg = 1;
                var func = command.BuildCommand(() => curArg >= parts.Length ? "" : parts[curArg++]);
                func(debugHost);
            }
        }
        catch (Exception ex)
        {
            Console.WriteLine("Parse error: " + ex.Message);
        }
    }

    public static int Main()
    {
        try
        {
            var emptyArgs = Array.Empty<string>();
            using Ice.Communicator communicator = Ice.Util.initialize(ref emptyArgs);
            Ice.ObjectPrx? obj = communicator.stringToProxy("DebugHost:default -h localhost -p 7243");

            DebugHostPrx debugHost = DebugHostPrxHelper.checkedCast(obj);
            if (debugHost == null)
                throw new ApplicationException("Invalid proxy");

            string? line;
            while ((line = Console.ReadLine())?.ToUpperInvariant() != "EXIT")
                if (!string.IsNullOrEmpty(line))
                    RunCommand(line, debugHost);

            return 0;
        }
        catch (Exception e)
        {
            Console.Error.WriteLine(e);
            return 1;
        }
    }
}

