using System.IO;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;

namespace VRX.Desktop;

internal sealed class BrowserBookmarks
{
    internal sealed record Entry(string Address, string Title);
    private readonly string file;
    private List<Entry> entries = [];
    internal IReadOnlyList<Entry> Entries => entries;

    internal BrowserBookmarks(string file)
    {
        this.file = file;
        try
        {
            if (File.Exists(file) && new FileInfo(file).Length <= 2 * 1024 * 1024)
                entries = (JsonSerializer.Deserialize<List<Entry>>(File.ReadAllText(file)) ?? [])
                    .OfType<Entry>().Where(entry => Normalise(entry.Address) != null)
                    .DistinctBy(entry => entry.Address).Take(200).ToList();
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException)
        { System.Diagnostics.Debug.WriteLine($"[Browser bookmarks] {ex.Message}"); }
    }

    internal static string? Normalise(string? address) =>
        address is { Length: > 0 and <= 4096 } &&
        Uri.TryCreate(address, UriKind.Absolute, out var uri) && uri.Scheme is "https" or "http" &&
        uri.UserInfo.Length == 0 ? uri.AbsoluteUri : null;

    internal bool Contains(string? address) => Normalise(address) is string url && entries.Any(entry => entry.Address == url);

    internal void Add(string address, string? title)
    {
        if (Normalise(address) is not string url || Contains(url)) return;
        if (entries.Count >= 200) throw new IOException("Remove a bookmark before saving another one.");
        string name = string.IsNullOrWhiteSpace(title) ? new Uri(url).Host : title.Trim();
        Save([new(url, name.Length > 160 ? name[..160] : name), .. entries]);
    }

    internal void Remove(string address) => Save(entries.Where(entry => entry.Address != address).ToList());

    private void Save(List<Entry> updated)
    {
        ProfileStore.AtomicWrite(file, JsonSerializer.Serialize(updated));
        entries = updated;
    }

    internal static void SmokeTest(string output)
    {
        string path = Path.Combine(output, $"bookmarks-{Guid.NewGuid():N}.json");
        try
        {
            const string address = "https://game.example/help";
            var store = new BrowserBookmarks(path);
            store.Add(address, "Game help");
            store.Add(address, "Repeated");
            store.Add("file:///private.html", "Local file");
            var opened = new BrowserBookmarks(path);
            if (opened.Entries.Count != 1 || !opened.Contains(address) || opened.Entries[0].Title != "Game help")
                throw new IOException("Bookmarks did not persist or rejected a valid web page.");
            opened.Remove(address);
            if (new BrowserBookmarks(path).Entries.Count != 0) throw new IOException("A removed bookmark returned.");
        }
        finally { File.Delete(path); }
    }
}

internal sealed partial class FloatingBrowser
{
    private Button bookmarksButton = null!;
    private string? pageTitle;
    private readonly Border bookmarkOverlay = new() { Width = 475, CornerRadius = new(12),
        Background = Brushes.DarkSlateGray, Padding = new(14), Visibility = Visibility.Collapsed };
    private readonly StackPanel bookmarkList = new();
    private readonly ScrollViewer bookmarkScroll = new() { MaxHeight = 285, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
    private readonly HashSet<Button> bookmarkButtons = [];

    private void InitializeBookmarks()
    {
        bookmarkScroll.Content = bookmarkList;
        bookmarkOverlay.Child = bookmarkScroll;
        Canvas.SetRight(bookmarkOverlay, 10);
        Canvas.SetTop(bookmarkOverlay, 4);
        viewport.Children.Insert(2, bookmarkOverlay);
    }

    private void ToggleBookmarks()
    {
        if (bookmarkOverlay.Visibility == Visibility.Visible) { CloseBookmarks(); return; }
        CancelMouse();
        ClearContext();
        PopulateBookmarks();
        bookmarkScroll.ScrollToTop();
        bookmarkOverlay.Visibility = Visibility.Visible;
        Changed();
    }

    private void UpdateBookmarkStar() =>
        bookmarksButton.Content = FloatingBrowserRuntime.Bookmarks.Contains(LoadedAddress) ? "★" : "☆";

    private void CloseBookmarks()
    {
        if (bookmarkOverlay.Visibility == Visibility.Collapsed) return;
        bookmarkOverlay.Visibility = Visibility.Collapsed;
        ClearBookmarkRows();
        Changed();
    }

    private void ClearBookmarkRows()
    {
        foreach (var button in bookmarkButtons) actions.Remove(button);
        bookmarkButtons.Clear();
        bookmarkList.Children.Clear();
    }

    private void PopulateBookmarks()
    {
        ClearBookmarkRows();
        var store = FloatingBrowserRuntime.Bookmarks;
        Button EntryButton(string title, double width, Action action)
        {
            var button = Button(title, width, action);
            bookmarkButtons.Add(button);
            return button;
        }
        void Save(Action change)
        {
            try { change(); }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
            { status.Text = ex.Message; }
            UpdateBookmarkStar();
            PopulateBookmarks();
        }
        var add = EntryButton(Loc.Get("BrowserBookmarkSave"), double.NaN, () =>
        {
            if (LoadedAddress is string url) Save(() => store.Add(url, pageTitle));
        });
        add.IsEnabled = BrowserBookmarks.Normalise(LoadedAddress) != null && !store.Contains(LoadedAddress);
        bookmarkList.Children.Add(add);
        if (store.Entries.Count == 0)
            bookmarkList.Children.Add(new TextBlock { Text = Loc.Get("BrowserBookmarksEmpty"), Foreground = Brushes.White, Margin = new(8) });
        foreach (var item in store.Entries)
        {
            var row = new DockPanel { Margin = new(0, 4, 0, 0) };
            var remove = EntryButton("×", 48, () => Save(() => store.Remove(item.Address)));
            remove.ToolTip = Loc.Get("BrowserBookmarkRemove");
            System.Windows.Automation.AutomationProperties.SetName(remove, Loc.Format("BrowserBookmarkRemoveNamed", item.Title));
            DockPanel.SetDock(remove, Dock.Right);
            row.Children.Add(remove);
            var open = EntryButton(item.Title, double.NaN, () =>
            {
                CloseBookmarks();
                addressFocus = addressSelected = false;
                browser.Load(item.Address);
            });
            open.Content = new TextBlock { Text = item.Title, TextTrimming = TextTrimming.CharacterEllipsis };
            open.ToolTip = item.Address;
            open.HorizontalContentAlignment = HorizontalAlignment.Left;
            row.Children.Add(open);
            bookmarkList.Children.Add(row);
        }
    }
}
