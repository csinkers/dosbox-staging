namespace DosboxDebugger;

public delegate void StoppedDelegate(Registers state);

public class Debugger
{
    Registers _registers;
    public ITracer Log { get; }
    public DebugHostPrx Host { get; }
    public DebugClientPrx DebugClientPrx { get; }
    public Registers OldRegisters { get; private set; }
    public Registers Registers
    {
        get => _registers;
        set { OldRegisters = _registers; _registers = value; }
    }

    public MemoryCache Memory { get; } = new();

    public Debugger(IceSession ice, ITracer log)
    {
        Log = log ?? throw new ArgumentNullException(nameof(log));
        ice.Client.StoppedEvent += OnStopped;
        Host = ice.DebugHost;
        DebugClientPrx = ice.ClientProxy;
    }

    void OnStopped(Registers state)
    {
        Console.WriteLine(" -> Stopped");
    }
}
