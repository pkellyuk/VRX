namespace VRX.Desktop;

internal interface IFloatingPanel : IDisposable
{
    bool Closed { get; }
    bool Dirty { get; }
    byte[] Render();
    void Published();
    void Pointer(int kind, float x, float y, float value = 0);
}

[Flags]
internal enum KeyboardModifiers { None = 0, Control = 1, Alt = 2, Shift = 4, Windows = 8 }
internal readonly record struct KeyboardEdit(string Text, int Key = 0, int Backspaces = 0, KeyboardModifiers Modifiers = KeyboardModifiers.None);
internal interface IFloatingTextTarget
{
    void Edit(KeyboardEdit edit);
}
