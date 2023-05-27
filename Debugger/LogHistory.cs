namespace DosboxDebugger;

public class LogHistory : ITracer
{
    const int MaxHistory = 1000;
    readonly object _syncRoot = new();
    readonly Queue<LogEntry> _history = new();

    public void Add(string line) => Add(line, Severity.Debug);
    public void Add(string line, Severity severity)
    {
        lock (_syncRoot)
        {
            _history.Enqueue(new(severity, line));
            if (_history.Count > MaxHistory)
                _history.Dequeue();
        }
    }

    public void Access<T>(T context, Action<T, IReadOnlyCollection<LogEntry>> operation)
    {
        if (operation == null) throw new ArgumentNullException(nameof(operation));
        lock (_syncRoot)
            operation(context, _history);
    }

    public void Clear()
    {
        lock (_syncRoot)
            _history.Clear();
    }

    public void Debug(string message) => Add(message, Severity.Debug);
    public void Info(string message) => Add(message, Severity.Info);
    public void Warn(string message) => Add(message, Severity.Warn);
    public void Error(string message) => Add(message, Severity.Error);
}