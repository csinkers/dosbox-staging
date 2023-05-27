using System.Numerics;
using System.Text;
using ImGuiNET;

namespace DosboxDebugger;

class CommandWindow : IImGuiWindow
{
    readonly Debugger _debugger;
    readonly LogHistory _history;
    readonly byte[] _inputBuffer = new byte[512]; // TODO: Initial size
    bool _autoScroll = true;
    bool _scrollToBottom = true;
    bool _focus;
    bool _open = true;

    public CommandWindow(Debugger debugger, LogHistory history)
    {
        _debugger = debugger ?? throw new ArgumentNullException(nameof(debugger));
        _history = history ?? throw new ArgumentNullException(nameof(history));
    }

    public void Open() => _open = true;

    public void Draw()
    {
        if (!_open)
            return;

        ImGui.Begin("Command", ref _open);
        ImGui.SetWindowPos(Vector2.Zero, ImGuiCond.FirstUseEver);

        // Reserve enough left-over height for 1 separator + 1 input text
        float footerHeightToReserve = ImGui.GetStyle().ItemSpacing.Y + ImGui.GetFrameHeightWithSpacing();
        ImGui.BeginChild(
            "ScrollingRegion",
            new Vector2(0, -footerHeightToReserve),
            false,
            ImGuiWindowFlags.HorizontalScrollbar);

        ImGui.PushStyleVar(ImGuiStyleVar.ItemSpacing, new Vector2(4, 1)); // Tighten spacing

        _history.Access(0, (_, logs) =>
        {
            foreach (var log in logs)
            {
                ImGui.PushStyleColor(ImGuiCol.Text, log.Color);
                ImGui.TextUnformatted(log.Line);
                ImGui.PopStyleColor();
            }
        });

        if (_scrollToBottom || (_autoScroll && ImGui.GetScrollY() >= ImGui.GetScrollMaxY()))
            ImGui.SetScrollHereY(1.0f);
        _scrollToBottom = false;

        ImGui.PopStyleVar();
        ImGui.EndChild();
        ImGui.Separator();

        if (_focus)
        {
            ImGui.SetKeyboardFocusHere(0);
            _focus = false;
        }

        bool reclaimFocus = false;
        var inputTextFlags = ImGuiInputTextFlags.EnterReturnsTrue;
        if (ImGui.InputText("", _inputBuffer, (uint)_inputBuffer.Length, inputTextFlags))
        {
            var command = Encoding.ASCII.GetString(_inputBuffer);
            int index = command.IndexOf((char)0, StringComparison.Ordinal);
            command = command[..index];

            for (int i = 0; i < command.Length; i++)
                _inputBuffer[i] = 0;

            CommandParser.RunCommand(command, _debugger);
            reclaimFocus = true;
        }

        ImGui.SetItemDefaultFocus();
        if (reclaimFocus)
            ImGui.SetKeyboardFocusHere(-1); // Auto focus previous widget

        ImGui.SameLine();
        ImGui.Checkbox("Scroll", ref _autoScroll);

        ImGui.End();
    }
}