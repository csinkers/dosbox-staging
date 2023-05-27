using Exception = System.Exception;

namespace DosboxDebugger;

static class Program
{

    public static int Main()
    {
        try
        {
            using var ice = new IceSession();
            var debugger = new Debugger(ice);

            string? line;
            while ((line = Console.ReadLine())?.ToUpperInvariant() != "EXIT")
                if (!string.IsNullOrEmpty(line))
                    CommandParser.RunCommand(line, debugger);

            return 0;
        }
        catch (Exception e)
        {
            Console.Error.WriteLine(e);
            return 1;
        }
    }
}

