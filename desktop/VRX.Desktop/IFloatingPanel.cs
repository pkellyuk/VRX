namespace VRX.Desktop;

internal interface IFloatingPanel : IDisposable
{
    bool Closed { get; }
    bool Dirty { get; }
    byte[] Render();
    void Published();
    void Pointer(int kind, float x, float y, float value = 0);
}

internal readonly record struct KeyboardEdit(string Text, int Key = 0, int Backspaces = 0);
internal interface IFloatingTextTarget
{
    void Edit(KeyboardEdit edit);
}
