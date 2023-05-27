namespace DosboxDebugger;

public delegate string GetArg();
public delegate void DebugCommand(Debugger d);
public delegate DebugCommand CommandBuilder(GetArg getArg);

public class Command
{
    public Command(string[] names, string description, CommandBuilder buildCommand)
    {
        Names = names ?? throw new ArgumentNullException(nameof(names));
        Description = description;
        BuildCommand = buildCommand ?? throw new ArgumentNullException(nameof(buildCommand));
    }

    public string[] Names { get; }
    public string Description { get; }
    public CommandBuilder BuildCommand { get; }
}