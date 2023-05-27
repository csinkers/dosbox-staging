using Ice;

namespace DosboxDebugger;

public class IceSession : IDisposable
{
    readonly Communicator _communicator;
    public DebugHostPrx DebugHost { get; }
    public DebugClientI Client { get; }
    public DebugClientPrx ClientProxy { get; }

    public IceSession()
    {
        var emptyArgs = Array.Empty<string>();
        _communicator = Ice.Util.initialize(ref emptyArgs);
        DebugHost = DebugHostPrxHelper.checkedCast(
            _communicator.stringToProxy("DebugHost:default -h localhost -p 7243")
                .ice_twoway()
                .ice_secure(false));

        if (DebugHost == null)
            throw new ApplicationException("Invalid proxy");

        var adapter = _communicator.createObjectAdapterWithEndpoints("Callback.Client", "default -h localhost");
        Client = new DebugClientI();
        adapter.add(Client, Ice.Util.stringToIdentity("debugClient"));
        adapter.activate();

        ClientProxy = DebugClientPrxHelper.uncheckedCast(
            adapter.createProxy(Ice.Util.stringToIdentity("debugClient")));

        if (ClientProxy == null)
            throw new ApplicationException("Could not build client");
    }

    public void Dispose() => _communicator.Dispose();
}