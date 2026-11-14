using System.IO;
using System.Text.Json;
using System.Windows;

namespace VRX.Desktop;

// Local English gesture matching. The vocabulary grows from words the user taps;
// no keystrokes or gestures are sent to a prediction service.
internal sealed class SwipePredictor
{
    private static readonly string[] Rows = ["qwertyuiop", "asdfghjkl", "zxcvbnm"];
    private readonly Dictionary<string, int> learned = new(StringComparer.Ordinal);
    private readonly Dictionary<string, Point[]> shapes = new(StringComparer.Ordinal);
    private readonly string? path;
    private readonly List<string> words;
    internal SwipePredictor(string? path = null)
    {
        this.path = path;
        words = Vocabulary.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries).Distinct().ToList();
        try
        {
            if (path != null && File.Exists(path))
                foreach (var pair in JsonSerializer.Deserialize<Dictionary<string, int>>(File.ReadAllText(path)) ?? [])
                    if (Valid(pair.Key)) { learned[pair.Key] = pair.Value; if (!words.Contains(pair.Key)) words.Add(pair.Key); }
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException) { }
        foreach (string word in words) shapes[word] = Shape(word);
    }
    internal static Point KeyPoint(char letter)
    {
        for (int row = 0; row < Rows.Length; row++)
        {
            int col = Rows[row].IndexOf(char.ToLowerInvariant(letter));
            if (col >= 0) return new Point(col + (row == 0 ? 0 : row == 1 ? .5 : 1.5), row * (5.0 / 6));
        }
        return new Point();
    }
    private static bool Valid(string word) => word.Length is >= 2 and <= 24 && word.All(c => c is >= 'a' and <= 'z');
    internal bool IsKnown(string word) => shapes.ContainsKey(word.ToLowerInvariant());
    internal void Learn(string word)
    {
        word = word.ToLowerInvariant(); if (!Valid(word)) return;
        learned[word] = learned.GetValueOrDefault(word) + 1;
        if (!shapes.ContainsKey(word)) { words.Add(word); shapes[word] = Shape(word); }
    }
    internal void Save()
    {
        if (path == null || learned.Count == 0) return;
        try { Directory.CreateDirectory(Path.GetDirectoryName(path)!); File.WriteAllText(path, JsonSerializer.Serialize(learned)); }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
    }
    internal string[] Prefix(string prefix) => words.Where(w => w.StartsWith(prefix, StringComparison.OrdinalIgnoreCase) && w.Length > prefix.Length)
        .OrderByDescending(w => learned.GetValueOrDefault(w)).Take(3).ToArray();
    internal string[] Predict(IReadOnlyList<Point> trace)
    {
        if (trace.Count < 2) return [];
        var path = Resample(trace);
        return words.Select((word, rank) => (word, rank))
            .Where(x => (KeyPoint(x.word[0]) - path[0]).Length < 1.6 && (KeyPoint(x.word[^1]) - path[^1]).Length < 1.6)
            .Select(x => (x.word, score: Distance(path, shapes[x.word]) + .018 * Math.Log(x.rank + 2) - .04 * Math.Log(learned.GetValueOrDefault(x.word) + 1)))
            .OrderBy(x => x.score).Take(3).Select(x => x.word).ToArray();
    }
    internal static Point[] Shape(string word) => Resample(word.Where((c, i) => i == 0 || word[i - 1] != c).Select(KeyPoint).ToArray());
    private static Point[] Resample(IReadOnlyList<Point> points)
    {
        if (points.Count == 0) return new Point[32];
        var lengths = new double[points.Count];
        for (int i = 1; i < points.Count; i++) lengths[i] = lengths[i - 1] + (points[i] - points[i - 1]).Length;
        var result = new Point[32]; int segment = 1;
        for (int i = 0; i < result.Length; i++)
        {
            double at = lengths[^1] * i / (result.Length - 1);
            while (segment < points.Count - 1 && lengths[segment] < at) segment++;
            if (points.Count < 2 || lengths[^1] < .0001) { result[i] = points[0]; continue; }
            double t = (at - lengths[segment - 1]) / Math.Max(.0001, lengths[segment] - lengths[segment - 1]);
            result[i] = points[segment - 1] + (points[segment] - points[segment - 1]) * t;
        }
        return result;
    }
    private static double Distance(Point[] a, Point[] b)
    {
        var previous = Enumerable.Repeat(double.PositiveInfinity, b.Length + 1).ToArray(); previous[0] = 0;
        for (int i = 1; i <= a.Length; i++)
        {
            var current = Enumerable.Repeat(double.PositiveInfinity, b.Length + 1).ToArray();
            for (int j = Math.Max(1, i - 6); j <= Math.Min(b.Length, i + 6); j++)
                current[j] = (a[i - 1] - b[j - 1]).LengthSquared + Math.Min(previous[j], Math.Min(previous[j - 1], current[j - 1]));
            previous = current;
        }
        return previous[^1] / a.Length;
    }
    internal static void SmokeTest()
    {
        var predictor = new SwipePredictor();
        foreach (string word in new[] { "hello", "world", "browser", "keyboard", "please", "thanks", "screen" })
            if (!predictor.Predict(Shape(word)).Contains(word)) throw new Exception($"Swipe prediction missed {word}");
        predictor.Learn("vrxcontroller");
        if (!predictor.Predict(Shape("vrxcontroller")).Contains("vrxcontroller")) throw new Exception("Keyboard did not learn a new word");
        if (!predictor.Prefix("brows").Contains("browser")) throw new Exception("Tap prediction did not complete a word");
    }
    private const string Vocabulary = """
    the to and of a in is it you that he was for on are with as his they at be this have from or one had by word but not what all were we when your can said there use an each which she do how their if will up other about out many then them these so some her would make like him into time has look two more write go see number no way could people my than first water been call who oil its now find long down day did get come made may part hello world browser keyboard please thanks screen
    i me our us yes yeah yep nope okay ok well also just very really only still even much good great nice right left back forward next last new old big small little over under again here where why when want need know think take give work help try let say tell ask put keep mean feel leave bring begin seem hear turn show play move live believe hold happen must provide sit stand lose pay meet include continue set learn change lead understand watch follow stop create speak read allow add spend grow open walk win offer remember love consider appear buy wait serve die send expect build stay fall cut reach kill remain suggest raise pass sell require report decide pull return explain hope develop carry break receive agree support hit produce eat cover catch draw choose cause point listen realize place close involve increase start game windows window menu button image pictures snapshot folder images photo save delete cancel click grip trigger stick controller hand hands floating throw resize type swipe prediction english text address search google internet website youtube video videos music movie games settings user mouse enter space shift tab escape home end desktop computer vr headset pico steam steamvr vrx control fullscreen launch application program software hardware virtual reality snap capture camera clipboard copy paste upload download zoom scroll select reset default profile session window browser browsers screenshot screenshots
    above across act action active activity actually address advice afraid after afternoon age ago air airport almost alone along already although always amazing america american among amount animal another answer any anyone anything apartment app apple apply area arm around arrive art article artist attention audio author available away awesome awful baby bad bag ball bank base basic bathroom battery beach beautiful became because become bed before began behind being best better between beyond billion bird birthday black blood blue board boat body book border born both box boy branch bread bright brother brown building business busy buy cake calendar camera camp car card care career careful case cat center central century certain certainly chair chance character charge check child children choice city class clean clear clip clock clothes cloud club code coffee cold color colour comfortable common community company compare complete completely condition conference connect connection contact content context conversation cool corner correct cost country couple course court cousin cover crazy cream culture cup current customer daily dark data date daughter dead deal dear death deep degree delicious department design detail different difficult dinner direction discussion display doctor dog door double doubt draft drag dream drive driver drop during early earth east easy edge edit education effect effort either else email empty enable energy enjoy enough entire environment especially evening event ever every everybody everyone everything evidence exactly example excellent except experience expert export extra eye eyes face fact family far fast father favorite favourite feature feet field file film final finally finger finish fire fish five flat floor focus food foot football force forest forget form four free friend front full fun future general girl glass global goal god gold gone government green ground group guess guy hair half happy hard head health heart heavy high history hot hour house human hundred idea important improve inside instead interest issue job join keep key kid kind kitchen knew language large late later laugh law layer least less letter level library light line link list local location log login longer lot low machine main major man manager map mark market match matter maybe meeting member memory message middle might minute missing model moment monday money month morning most mother motion mountain mouth mr mrs multiple name nation national near nearly necessary never night none north note nothing notice november object ocean october off office often once online onto operation option order original others outside own page paint panel paper parent party password past path peace pen per perfect perhaps person phone pink plan planet platform player pocket police poor popular position possible power practice pretty probably problem process project public purple push quality question quick quiet quite radio rain range ready real reason recent record red refresh region register regular relationship release remote remove render request research resolution rest result review rich ring road rock room round row rule run safe same saturday saw school science sea second section security select sense september serious service seven several share sharp shirt short should side sign simple since single sister site six size sky sleep slow smart smile smooth snow social something sometimes son soon sorry sound south special speed spring square staff stage star state station step stock store story straight street strong student stuff such summer sunday sure sweet switch table talk task tea team tech technology test thank thing third those though thought thousand three through throw thumb thus today together tomorrow too top touch town track train travel tree trip trouble true trust twice typing unable until update usual value various view voice volume vote wake wall warm warning was water wave wear weather week welcome west whatever whether white whole whose wife wild wish within without woman women wonder wonderful wood word worker working worry would wrong year yellow yesterday young zero
    account accept access accident according achievement acid actual adapt additional advance advantage adventure advertising affect agency ahead aim alive alternative angry announce annual anybody anyway anywhere apart apology approach appropriate april argument army arrange arrival august automatic balance bar basic battle beginning behave belief belong benefit beside best beyond bike bill bit bitter block boat boil boss bottom bowl brain brave breakfast breath breathe bridge brief broad broken budget burn cable calculate calm capable capital captain career carpet cash castle category cause ceiling celebrate chain challenge champion chapter charity cheap chemical china christian christmas church circle citizen claim classic clever client climate climb clothing coach collect college comfortable command comment commercial communication competition complex concern confident confirm conflict confused congratulations connect consequence conservative consist constant construct contain contrast convenient cookie coordinate core count couple crash creative credit crime critical crowded crystal culture curious dance danger dangerous database deadline december decision decorate decrease defend definitely definition delivery demonstrate depth describe description desert deserve desire desk destroy determine difference difficulty direct directory disappear discover distance district divide document domestic done download dramatic drawer drawing dress drink drink drive dry due dull duty dynamic earn eastern economy editor efficient either electric electronic emergency emotion emphasize employee employer employment encourage ending engine engineer engineering enough entry equal equipment error escape essential estate estimate europe european exact examination exercise exist exit expand expensive experiment export expression extreme failure faith familiar famous fancy farm farmer fashion fault fear female festival fiction fifteen fifty fight figure fill finance financial fine firmly fit fitness fix flag flight flower fly flying fold folk foreign forever formal former forth fortune forty foundation frame france fresh friendship fruit fuel function funny furniture gain garage garden gas gather gear gender giant gift glad goat grab graduate grand grass grey grocery guard guest guide habit handle hang happiness hardly hate healthy heat heaven height held hers herself hidden highlight hill historical holiday honest honor horse hospital hotel household huge hungry husband ice icon identity ignore ill imagine impact implement impossible income indeed independent individual information initial injury inner input instance instruction insurance intelligent international interview introduction invite island item january japan john journal journey judge juice july june jump justice knew knife knowledge lake laptop latter laugh launch leader leadership leaf lean legal lemon length lesson likely limit literature load loan locked lock logical lonely loud lovely luck lunch magazine maintain male management manner march marriage master material math maximum measure medicine mental mention menu merchant mess metal method microphone military minimum minister minor mirror mistake mixed mobile modern monitor motor movement murder museum mutual mystery narrative narrow native natural nature nearby negative neither nervous network newspaper normal northern nose november obvious obtain occasion occur odd official operating opinion orange ordinary organization otherwise output overall owner package painting parking particular partner patient pattern pause payment percent performance permission personal perspective phase photograph physical pick pilot pitch placement plastic plate pleasant pleasure plenty pointer poetry political politics pool position positive possibility potato potential powerful practical prefer prepare present president press pressure prevent previous price primary principle print private progress promise proper protect proud prove purchase purpose quarter queen quickly race railway random rate rather raw react reality recognize recommendation reduce reference reflect refuse regarding regardless relate relative relax relevant reliable religion remember repeat replace reply represent republic reputation responsible restaurant retire reward rice ride rise risk river robot roof rough route royal rugby salad salary salt sample sand satisfy saturday sauce schedule scheme score scotland season seat secondary secret secretary secure seed seek senior separate sequence series session severe sexual shadow shall shape sheep shelf shell ship shock shoot shopping shout signal silence similar simply simultaneously singer slightly solid solution somewhere sort soul source southern speech spirit sport spot spread staff standard statement statistics steady steal steel steep stone storage storm strange strategy strength stress stretch strike string structure struggle subject successful suddenly suffer sufficient suit suitable supply suppose surface surround survive sweet symbol system tape taste technical technique temperature temple tend term territory theme theory therefore thick thin think throat throughout tidy tip tired toast token tone tool total totally tough toward towards toy trade traditional traffic transfer transparent transport treat treatment trial trick triangle trigger truth tune twelve twenty twice typical ugly ultimately unable undo uniform union unique unit universe university unlock upper urgent useful usual valley variable vegetable version victim village visible vision visit visitor visual vitamin volume wait waiter warm wash waste wealthy wedding weekend weight western wet wheel whenever whereas wherever whisper width window wise woke woman wonderful worse worst worth wound wrap writer written yesterday yield yourself youth zone
    """;
}
