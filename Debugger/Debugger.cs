namespace DosboxDebugger;

public delegate void StoppedDelegate(Registers state);

public class Debugger
{
    public DebugHostPrx Host { get; }
    public DebugClientPrx DebugClientPrx { get; }

    public Debugger(IceSession ice)
    {
        ice.Client.StoppedEvent += OnStopped;
        Host = ice.DebugHost;
        DebugClientPrx = ice.ClientProxy;
    }

    void OnStopped(Registers state)
    {
        Console.WriteLine(" -> Stopped");
    }
}
