using ImGuiNET;
using Exception = System.Exception;

namespace DosboxDebugger;

static class Program
{
    public static int Main()
    {
        try
        {
            using var ice = new IceSession();
            using var ui = new Ui();

            var history = new LogHistory();
            var debugger = new Debugger(ice, history);
            var commandWindow = new CommandWindow(debugger, history);
            var registersWindow = new RegistersWindow(debugger);

            ui.AddWindow(commandWindow.Draw);
            ui.AddWindow(registersWindow.Draw);

            ui.AddMenu(() =>
            {
                if (ImGui.BeginMenu("Windows"))
                {
                    if (ImGui.MenuItem("Command")) commandWindow.Open();
                    if (ImGui.MenuItem("Registers")) registersWindow.Open();
                    ImGui.EndMenu();
                }

            });
            ui.Run();
/*
            string? line;
            while ((line = Console.ReadLine())?.ToUpperInvariant() != "EXIT")
                if (!string.IsNullOrEmpty(line))
                    CommandParser.RunCommand(line, debugger);
*/
            return 0;
        }
        catch (Exception e)
        {
            Console.Error.WriteLine(e);
            return 1;
        }
    }
}