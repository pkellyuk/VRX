using System.Diagnostics;

namespace VRX.Desktop;

// What the main button on the game card does now (its label says so). Expert follows the
// selection and its Depth | Geometry mode; Easy is always Attach / Play for a running window
// or display, as before.
public enum MainAction
{
    None,                   // nothing usable: disabled, its tooltip says why
    Busy,                   // a session is starting, playing or stopping, or the chooser is open: disabled
    AttachPlay,             // attach to the running game (estimated depth), or Easy's Attach / Play
    PlayStream,             // attach to the geometry stream of a run VRX started
    PlayDisplay,            // play a whole display
    StartGame,              // start the stopped game normally, then attach with estimated depth
    StartWithGeometry,      // start the stopped game with true geometry (warning first)
    RestartWithGeometry,    // restart the running game with true geometry (warning and restart dialog)
}

// Why the main button is disabled with nothing usable (Expert only; Easy shows no reason).
public enum MainBlock
{
    None,
    NoSelection,            // nothing chosen (or a display that is not there)
    CannotStart,            // a stopped game VRX cannot start
}

public static class MainButton
{
    // The main button's action, with no side effects.
    // hasSelection: a game or display is selected and its settings are loaded.
    // isDisplay: the selection is a whole display. isRunning: its window (or display) is there.
    // streamReady: VRX started this very process with true geometry and its stream is ready
    // (GeometryLaunch.ExistingSession, as DecideAttach uses it). geometryMode: the Depth |
    // Geometry switch shows Geometry. canStart: VRX can start the game (an existing executable,
    // or a Steam game bound to one). easy: Easy mode. busy: a session is starting, running or
    // stopping, or the chooser is gathering games.
    public static MainAction Decide(bool hasSelection, bool isDisplay, bool isRunning, bool streamReady, bool geometryMode, bool canStart, bool easy, bool busy)
    {
        Debug.WriteLine($"[Main button] Decide enter: selection {hasSelection}, display {isDisplay}, running {isRunning}, stream {streamReady}, geometry {geometryMode}, can start {canStart}, easy {easy}, busy {busy}");
        if (busy)
        {
            Debug.WriteLine("[Main button] Decide exit: Busy");
            return MainAction.Busy;
        }
        if (!hasSelection)
        {
            Debug.WriteLine("[Main button] Decide exit: None (no selection)");
            return MainAction.None;
        }
        if (easy)
        {
            // Easy as before: Attach / Play for a running window or display, nothing else.
            var easyAction = isRunning ? MainAction.AttachPlay : MainAction.None;
            Debug.WriteLine($"[Main button] Decide exit: {easyAction} (Easy)");
            return easyAction;
        }
        if (isDisplay)
        {
            var displayAction = isRunning ? MainAction.PlayDisplay : MainAction.None;
            Debug.WriteLine($"[Main button] Decide exit: {displayAction} (display)");
            return displayAction;
        }
        bool geometry = geometryMode && canStart;
        if (isRunning)
        {
            var runningAction = streamReady ? MainAction.PlayStream : geometry ? MainAction.RestartWithGeometry : MainAction.AttachPlay;
            Debug.WriteLine($"[Main button] Decide exit: {runningAction} (running)");
            return runningAction;
        }
        if (!canStart)
        {
            Debug.WriteLine("[Main button] Decide exit: None (not running, VRX cannot start it)");
            return MainAction.None;
        }
        var stoppedAction = geometry ? MainAction.StartWithGeometry : MainAction.StartGame;
        Debug.WriteLine($"[Main button] Decide exit: {stoppedAction} (not running)");
        return stoppedAction;
    }

    // Why Decide gave None (for the disabled button's tooltip); MainBlock.None otherwise.
    public static MainBlock Why(MainAction action, bool hasSelection, bool isDisplay, bool isRunning)
    {
        if (action != MainAction.None) return MainBlock.None;
        if (!hasSelection || isDisplay || isRunning) return MainBlock.NoSelection;
        return MainBlock.CannotStart;
    }
}
