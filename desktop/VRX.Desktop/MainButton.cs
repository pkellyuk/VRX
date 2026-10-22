using System.Diagnostics;

namespace VRX.Desktop;

// What the main button on the game card does now (its label says so). Expert follows the
// selection and the app-wide Depth | Geometry mode; Easy is always Attach / Play for a running
// window or display, as before.
public enum MainAction
{
    None,                   // nothing usable: disabled, its tooltip says why (MainBlock)
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
    CannotStart,            // a game VRX cannot start (and, on Depth, not running)
    DisplayNeedsDepth,      // Geometry mode, a whole display: it only plays with estimated depth
    NotAGame,               // Geometry mode, a program on the non-game list (GameHistory.IsNotAGame)
}

// Everything the main button depends on, as plain values (so the decision can be tested).
// HasSelection: a game or display is selected and its settings are loaded. IsDisplay: the
// selection is a whole display. IsRunning: its window (or display) is there. StreamReady: VRX
// started this very process with true geometry and its stream is ready
// (GeometryLaunch.ExistingSession, as DecideAttach uses it). GeometryMode: the app-wide Depth |
// Geometry switch is on Geometry (AppSettings.StereoMode). CanStart: VRX can start the game (an
// existing executable, or a Steam game bound to one). NotAGame: the program is on the non-game
// list (Teams, browsers, editors...). Easy: Easy mode (always estimated depth). Busy: a session
// is starting, running or stopping, or the chooser is gathering games.
public readonly record struct MainState(bool HasSelection, bool IsDisplay, bool IsRunning, bool StreamReady, bool GeometryMode,
    bool CanStart, bool NotAGame, bool Easy, bool Busy);

public static class MainButton
{
    // The main button's action, with no side effects; `block` says why when it is None.
    // Depth: Start / Attach / Play / Play (a VRX geometry stream) / Play display, as before true
    // geometry existed. Geometry: Start (true geometry) for a stopped game VRX can start, Play
    // for VRX's own geometry run, Restart for a game running normally; never a display or a
    // program that is not a game.
    public static MainAction Decide(MainState state, out MainBlock block)
    {
        Debug.WriteLine($"[Main button] Decide enter: {state}");
        block = MainBlock.None;
        if (state.Busy)
        {
            Debug.WriteLine("[Main button] Decide exit: Busy");
            return MainAction.Busy;
        }
        if (!state.HasSelection)
        {
            block = MainBlock.NoSelection;
            Debug.WriteLine("[Main button] Decide exit: None (no selection)");
            return MainAction.None;
        }
        if (state.Easy)
        {
            // Easy as before: Attach / Play for a running window or display, nothing else.
            var easyAction = state.IsRunning ? MainAction.AttachPlay : MainAction.None;
            Debug.WriteLine($"[Main button] Decide exit: {easyAction} (Easy)");
            return easyAction;
        }
        var action = state.GeometryMode ? DecideGeometry(state, out block) : DecideDepth(state, out block);
        Debug.WriteLine($"[Main button] Decide exit: {action}, block {block} ({(state.GeometryMode ? "Geometry" : "Depth")})");
        return action;
    }

    // Depth mode, Expert: everything plays with estimated depth, except Play for the stream of
    // a run VRX started with true geometry.
    private static MainAction DecideDepth(MainState state, out MainBlock block)
    {
        block = MainBlock.None;
        if (state.IsDisplay)
        {
            if (state.IsRunning) return MainAction.PlayDisplay;
            block = MainBlock.NoSelection;
            return MainAction.None;
        }
        if (state.IsRunning) return state.StreamReady ? MainAction.PlayStream : MainAction.AttachPlay;
        if (state.CanStart) return MainAction.StartGame;
        block = MainBlock.CannotStart;
        return MainAction.None;
    }

    // Geometry mode, Expert: only games VRX starts (or started) with true geometry.
    private static MainAction DecideGeometry(MainState state, out MainBlock block)
    {
        block = MainBlock.None;
        if (state.IsDisplay)
        {
            block = MainBlock.DisplayNeedsDepth;
            return MainAction.None;
        }
        if (state.NotAGame)
        {
            block = MainBlock.NotAGame;
            return MainAction.None;
        }
        if (state.IsRunning && state.StreamReady) return MainAction.PlayStream;
        if (!state.CanStart)
        {
            block = MainBlock.CannotStart;
            return MainAction.None;
        }
        return state.IsRunning ? MainAction.RestartWithGeometry : MainAction.StartWithGeometry;
    }
}
