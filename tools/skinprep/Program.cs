// skinprep: copy skin character models into every level, so any skin works anywhere.
//
//   skinprep <game folder> [--skins TAYLOR,SAMUELS,...] [--levels HAB_AIRPORT,...] [--dry] [--restore]
//
// For each skin (a DisplayModel:<NAME> composite) it finds a level that ships the model, then copies the composite
// and everything it renders with (models, materials, textures, shaders, collision) into each level that lacks it.
// Each level is written to a staging folder, re-loaded and checked, and only then copied over the level, after the
// original files are backed up to <game>\coop_backup\levels\<LEVEL>\ (once; --restore puts them back).
//
// The import, closure and validation code is adapted from AI-MP's RipleyCharacterInstaller ("AI-MP — originally
// created by Dropper7856", https://github.com/Dropper7856/AIMP), under the AI-MP Non-Commercial Attribution
// License (third_party/AIMP-LICENSE.txt). Changes: any DisplayModel and any source level instead of the Nostromo
// crew from one DLC level; a plain backup/restore instead of AI-MP's transaction journal; no NPC weapon scripts.
using System.Security.Cryptography;
using System.Text.Json;
using CATHODE;
using CATHODE.Scripting;
using CATHODE.Scripting.Internal;
using CathodeLib;
using CathodeLib.ObjectExtensions;

static class P
{
    static readonly string[] DefaultSkins = { "TAYLOR", "SAMUELS", "AXEL", "RICARDO", "MARLOW" };

    static int Main(string[] a)
    {
        if (a.Length < 1) { Console.WriteLine("usage: skinprep <game folder> [--skins A,B] [--levels L1,L2] [--dry] [--restore]"); return 1; }
        var game = Path.GetFullPath(a[0]);
        string? Opt(string k) { var i = Array.IndexOf(a, k); return i >= 0 && i + 1 < a.Length ? a[i + 1] : null; }
        var skins = (Opt("--skins")?.Split(',') ?? DefaultSkins).Select(s => s.Trim().ToUpperInvariant()).ToArray();
        var only = Opt("--levels")?.Split(',').Select(s => s.Trim().ToUpperInvariant()).ToHashSet();
        bool dry = a.Contains("--dry"), restore = a.Contains("--restore"), roundtrip = a.Contains("--roundtrip");   // roundtrip: load and save, add nothing (diagnosis)
        if (System.Diagnostics.Process.GetProcessesByName("AI").Length > 0) { Console.WriteLine("Close the game first."); return 2; }

        var env = Path.Combine(game, "DATA", "ENV");
        var levels = Levels(env).Where(l => only == null || only.Contains(Key(env, l))).ToList();
        if (restore) { foreach (var l in levels) Restore(game, env, l); if (only == null) File.Delete(Path.Combine(game, "coop_skins.txt")); return 0; }

        var global = new Global(Path.Combine(env, "GLOBAL"), new PAK2(Path.Combine(game, "DATA", "GLOBAL", "ANIMATION.PAK")));
        // where each skin lives (from a scan of every level's scripts; the same model in every level that has it)
        var known = new Dictionary<string, string> {
            ["TAYLOR"] = "SCI_HUB", ["SAMUELS"] = "SCI_HUB", ["RICARDO"] = "SCI_HUB", ["WAIT"] = "SCI_HUB", ["AXEL"] = "HAB_AIRPORT",
            ["MARLOW"] = "HAB_SHOPPINGCENTRE", ["VERLAINE"] = "BSP_TORRENS", ["CONNOR"] = "BSP_TORRENS", ["DR_KUHLMAN"] = "SCI_HOSPITALUPPER",
            ["SPACESUIT_HEYST"] = "BSP_LV426_PT01", ["SPACESUIT_MEEKS"] = "BSP_LV426_PT01" };
        var sourceOf = new Dictionary<string, string>();
        foreach (var s in skins) if (known.TryGetValue(s, out var lv)) sourceOf[s] = Path.Combine(env, "PRODUCTION", lv);
        foreach (var s in skins)
            Console.WriteLine(sourceOf.TryGetValue(s, out var src) ? $"skin {s}: from {Key(env, src)}" : $"skin {s}: no level has it, skipped");

        var sources = new Dictionary<string, Assets>();
        Assets Src(string l) => sources.TryGetValue(l, out var x) ? x : sources[l] = new Assets(l, global);
        int changed = 0, failed = 0;
        var have = new SortedDictionary<string, List<string>>();   // level -> skins it ships after this run (coop_skins.txt)
        foreach (var l in levels)
        {
            var name = Key(env, l);
            try
            {
                var dest = LoadChecked(l, global, out var warn);
                if (warn != null) { Console.WriteLine($"{name}: skipped, CathodeLib can't read all of its scripts ({warn})"); continue; }
                var before = Counts(dest.Commands);
                var snap = dest.Commands.Entries.SelectMany(c => c.GetEntities().Select(e => (k: c.shortGUID.AsUInt32.ToString("x8") + "/" + e.shortGUID.AsUInt32.ToString("x8"), v: string.Join(",", e.parameters.Select(x => x.name.ToString()).OrderBy(x => x)))))
                    .GroupBy(t => t.k).ToDictionary(g => g.Key, g => g.First().v);
                var missing = skins.Where(s => sourceOf.ContainsKey(s) && !Has(dest.Commands, "DisplayModel:" + s)).ToList();
                var present = skins.Where(x => Has(dest.Commands, "DisplayModel:" + x)).ToList();
                if (missing.Count == 0 && !roundtrip) { Console.WriteLine($"{name}: has every skin"); have[name] = present; continue; }
                if (roundtrip) missing.Clear();
                if (dry) { Console.WriteLine($"{name}: would add {string.Join(", ", missing)}"); continue; }
                foreach (var s in missing)
                {
                    var src = Src(sourceOf[s]);
                    var root = src.Commands.GetComposite("DisplayModel:" + s)!;
                    var closure = Closure(src.Commands, new[] { root });
                    Validate(src, closure);
                    Import(src, dest, closure);
                }
                var stage = Path.Combine(game, "coop_backup", "stage", name);
                if (Directory.Exists(stage)) Directory.Delete(stage, true);
                dest.SaveTo(stage);
                var check = LoadChecked(stage, global, out var warn2);   // re-load what we wrote
                if (warn2 != null) throw new InvalidDataException("written scripts don't load cleanly: " + warn2);
                var after = Counts(check.Commands);
                // Existing composites must come back the same. The one change CathodeLib makes on every save is writing a
                // function's resources out as a "resource" parameter too; that alone is allowed.
                foreach (var kv in before)
                {
                    if (!after.TryGetValue(kv.Key, out var n)) throw new InvalidDataException($"composite {kv.Key} lost when written");
                    if (n == kv.Value) continue;
                    var g = Convert.ToUInt32(kv.Key, 16);
                    if (Shape(kv.Value) != Shape(n)) throw new InvalidDataException($"composite {kv.Key} changed when written ({kv.Value} -> {n})");
                    foreach (var c1 in check.Commands.Entries.Where(x => x.shortGUID.AsUInt32 == g))
                        foreach (var e1 in c1.GetEntities())
                        {
                            var key = kv.Key + "/" + e1.shortGUID.AsUInt32.ToString("x8");
                            var now1 = e1.parameters.Select(x => x.name.ToString()).ToList();
                            var was = snap.TryGetValue(key, out var w) ? w.Split(',', StringSplitOptions.RemoveEmptyEntries).ToList() : null;
                            if (was == null) throw new InvalidDataException($"new entity in {c1.name}");
                            var added = now1.Where(x => !was.Contains(x)).ToList(); var lost = was.Where(x => !now1.Contains(x)).ToList();
                            if (lost.Count > 0 || added.Any(x => x != "resource"))
                                throw new InvalidDataException($"{c1.name}: parameters changed when written (+{string.Join(",", added)} -{string.Join(",", lost)})");
                        }
                }
                Publish(game, env, l, stage);
                Directory.Delete(stage, true);
                Console.WriteLine($"{name}: added {string.Join(", ", missing)}");
                have[name] = present.Concat(missing).ToList();
                changed++;
            }
            catch (Exception e) { Console.WriteLine($"{name}: FAILED, level left unchanged: {e.Message}"); failed++; }
        }
        if (!dry && only == null)
        {
            File.WriteAllLines(Path.Combine(game, "coop_skins.txt"), have.Select(kv => kv.Key + " " + string.Join(" ", kv.Value)));
            Console.WriteLine("wrote coop_skins.txt");
        }
        Console.WriteLine($"done: {changed} levels changed, {failed} failed");
        return failed == 0 ? 0 : 3;
    }

    static string Shape(string counts) => counts[..counts.LastIndexOf('/')];   // everything but the parameter total
    static bool Has(Commands c, string name) => c.Entries.Any(x => string.Equals(x.name, name, StringComparison.OrdinalIgnoreCase));
    // Load a level, capturing CathodeLib's console warnings: any "Skipping" means part of the scripts would be lost on save.
    static Assets LoadChecked(string root, Global global, out string? warning)
    {
        var old = Console.Out; var sw = new StringWriter(); Console.SetOut(sw);
        Assets a;
        try { a = new Assets(root, global); } finally { Console.SetOut(old); }
        var bad = sw.ToString().Split(Environment.NewLine.ToCharArray()).FirstOrDefault(x => x.Contains("Skipping") || x.Contains("ERROR") || x.Contains("Failed"));
        warning = bad?.Trim();
        return a;
    }
    // Per composite: functions, variables, proxies, aliases, links, parameters. Must not change for existing composites.
    static Dictionary<string, string> Counts(Commands c) => c.Entries.GroupBy(x => x.shortGUID.AsUInt32).ToDictionary(g => g.Key.ToString("x8"), g => {
        int n = g.Count(), f = g.Sum(x => x.functions.Count), v = g.Sum(x => x.variables.Count), px = g.Sum(x => x.proxies.Count), al = g.Sum(x => x.aliases.Count);
        int links = g.Sum(x => x.GetEntities().Sum(e => e.childLinks.Count)), pars = g.Sum(x => x.GetEntities().Sum(e => e.parameters.Count));
        return $"{n}x {f}/{v}/{px}/{al}/{links}/{pars}"; });

    // ---------- levels ----------
    static IEnumerable<string> Levels(string env)
    {
        var prod = Path.Combine(env, "PRODUCTION");
        foreach (var d in Directory.GetDirectories(prod).Concat(Directory.Exists(Path.Combine(prod, "DLC")) ? Directory.GetDirectories(Path.Combine(prod, "DLC")) : Array.Empty<string>()))
        {
            var n = Path.GetFileName(d).ToUpperInvariant();
            if (n == "DLC" || n == "FRONTEND" || n.EndsWith("_PATCH")) continue;
            if (File.Exists(Path.Combine(d, "WORLD", "COMMANDS.PAK")) && File.Exists(Path.Combine(d, "RENDERABLE", "LEVEL_MODELS.PAK"))) yield return d;
        }
    }
    static string World(string level) => Path.Combine(level, "WORLD");
    static string Key(string env, string level) => Path.GetRelativePath(Path.Combine(env, "PRODUCTION"), level).Replace('\\', '_').ToUpperInvariant();

    // ---------- closure and checks (adapted from AI-MP) ----------
    static List<Composite> Closure(Commands commands, IEnumerable<Composite> roots)
    {
        var seen = new HashSet<uint>(); var result = new List<Composite>(); var pending = new Stack<Composite>(roots.Reverse());
        while (pending.TryPop(out var comp))
        {
            if (!seen.Add(comp.shortGUID.AsUInt32)) continue;
            result.Add(comp);
            foreach (var f in comp.functions)
            {
                if (f.function.IsFunctionType) continue;
                pending.Push(commands.GetComposite(f.function) ?? throw new InvalidDataException($"dependency {f.function} missing from {comp.name}"));
            }
        }
        return result;
    }

    static IEnumerable<ResourceReference> ResourcesOf(Entity e)
    {
        if (e is FunctionEntity fn && fn.resources != null) foreach (var r in fn.resources) if (r != null) yield return r;
        foreach (var p in e.parameters)
            if (p.content is cResource { value: not null } res) foreach (var r in res.value) if (r != null) yield return r;
    }

    static void Validate(Assets assets, IEnumerable<Composite> comps)
    {
        int renderables = 0;
        foreach (var comp in comps)
            foreach (var r in comp.GetEntities().SelectMany(ResourcesOf))
                switch (r.resource_type)
                {
                    case ResourceType.RENDERABLE_INSTANCE:
                        if (r.RenderableInstance is not { Count: > 0 }) throw new InvalidDataException($"unresolved renderable in {comp.name}");
                        foreach (var el in r.RenderableInstance) CheckElement(assets, el, new HashSet<RenderableElements.Element>(System.Collections.Generic.ReferenceEqualityComparer.Instance));
                        renderables++; break;
                    case ResourceType.ANIMATED_MODEL when r.AnimatedModel == null:
                    case ResourceType.COLLISION_MAPPING when r.CollisionMapping == null:
                        throw new InvalidDataException($"unresolved {r.resource_type} in {comp.name}");
                    case ResourceType.DYNAMIC_PHYSICS_SYSTEM:
                    case ResourceType.EXCLUSIVE_MASTER_STATE_RESOURCE:
                        throw new InvalidDataException($"{r.resource_type} can't be copied safely");
                }
        if (renderables == 0) throw new InvalidDataException("no renderable model");
    }
    static void CheckElement(Assets a, RenderableElements.Element el, HashSet<RenderableElements.Element> seen)
    {
        if (!seen.Add(el)) return;
        if (el.Model == null || el.Material == null || a.Models.FindModel(el.Model) == null || !a.Materials.Entries.Any(m => ReferenceEquals(m, el.Material)))
            throw new InvalidDataException("unresolved model or material");
        if (el.Material.Shader == null || !a.Shaders.Entries.Any(s => ReferenceEquals(s, el.Material.Shader))) throw new InvalidDataException("unresolved shader");
        foreach (var t in el.Material.TextureReferences)
            if (t.Location != TexturePtr.Source.NONE && t.Texture == null) throw new InvalidDataException("unresolved texture");
        foreach (var lod in el.LODs) CheckElement(a, lod, seen);
    }

    static void Import(Assets src, Assets dst, IReadOnlyList<Composite> closure)
    {
        var imp = new Importer(src, dst);
        foreach (var comp in closure)
        {
            var byName = dst.Commands.GetComposite(comp.name); var byGuid = dst.Commands.GetComposite(comp.shortGUID);
            if ((byName != null && byName.shortGUID != comp.shortGUID) || (byGuid != null && !string.Equals(byGuid.name, comp.name, StringComparison.OrdinalIgnoreCase)))
                throw new InvalidDataException($"composite conflict: {comp.name}");
            if (byGuid != null) continue;   // already there (shared archetype)
            var copy = comp.Copy();
            foreach (var e in comp.GetEntities())
            {
                var ce = copy.GetEntityByID(e.shortGUID) ?? throw new InvalidDataException("entity clone incomplete");
                var a = ResourcesOf(e).ToArray(); var b = ResourcesOf(ce).ToArray();
                if (a.Length != b.Length) throw new InvalidDataException("resource clone incomplete");
                for (int i = 0; i < a.Length; i++) imp.Resource(a[i], b[i]);
            }
            dst.Commands.Entries.Add(copy);
        }
    }

    sealed class Importer(Assets src, Assets dst)
    {
        readonly Dictionary<Models.CS2, Models.CS2> models = new(System.Collections.Generic.ReferenceEqualityComparer.Instance);
        readonly Dictionary<Materials.Material, Materials.Material> mats = new(System.Collections.Generic.ReferenceEqualityComparer.Instance);
        readonly Dictionary<List<RenderableElements.Element>, List<RenderableElements.Element>> rends = new(System.Collections.Generic.ReferenceEqualityComparer.Instance);

        public void Resource(ResourceReference o, ResourceReference c)
        {
            switch (o.resource_type)
            {
                case ResourceType.RENDERABLE_INSTANCE: c.RenderableInstance = Elements(o.RenderableInstance); break;
                case ResourceType.ANIMATED_MODEL:
                    c.AnimatedModel = dst.Animations.ImportEntry(o.AnimatedModel);
                    c.AnimatedModel.ResourceIndex = dst.Animations.Entries.IndexOf(c.AnimatedModel); break;
                case ResourceType.COLLISION_MAPPING: c.CollisionMapping = dst.CollisionMaps.ImportEntry(o.CollisionMapping); break;
            }
        }
        List<RenderableElements.Element> Elements(List<RenderableElements.Element> o)
        {
            if (rends.TryGetValue(o, out var cached)) return cached;
            var result = new List<RenderableElements.Element>();
            foreach (var el in o)
            {
                if (el.ModelLocation != PakLocation.LEVEL || el.MaterialLocation != PakLocation.LEVEL) throw new InvalidDataException("global model, can't copy as a level asset");
                var copy = el.Copy();
                var model = src.Models.FindModel(el.Model) ?? throw new InvalidDataException("model missing");
                var im = Model(model); bool found = false;
                for (int ci = 0; ci < model.Components.Count && !found; ci++)
                    for (int li = 0; li < model.Components[ci].LODs.Count && !found; li++)
                        for (int si = 0; si < model.Components[ci].LODs[li].Submeshes.Count && !found; si++)
                            if (ReferenceEquals(model.Components[ci].LODs[li].Submeshes[si], el.Model)) { copy.Model = im.Components[ci].LODs[li].Submeshes[si]; found = true; }
                if (!found) throw new InvalidDataException("submesh not mapped");
                copy.Material = Material(el.Material);
                result.Add(copy); dst.Renderables.Entries.Add(copy);
            }
            for (int i = 0; i < o.Count; i++) result[i].LODs = Elements(o[i].LODs);
            rends.Add(o, result);
            return result;
        }
        Models.CS2 Model(Models.CS2 o)
        {
            if (models.TryGetValue(o, out var cached)) return cached;
            var copy = o.Copy();
            for (int ci = 0; ci < o.Components.Count; ci++)
                for (int li = 0; li < o.Components[ci].LODs.Count; li++)
                    for (int si = 0; si < o.Components[ci].LODs[li].Submeshes.Count; si++)
                    {
                        var f = o.Components[ci].LODs[li].Submeshes[si]; var t = copy.Components[ci].LODs[li].Submeshes[si];
                        t.Material = f.Material == null ? null! : Material(f.Material);
                        t.WeightedCollision = dst.Collisions.ImportEntry(f.WeightedCollision);
                        t.MorphAnimSet = dst.Morphs.ImportEntry(f.MorphAnimSet);
                    }
            var existing = dst.Models.Entries.FirstOrDefault(m => string.Equals(m.Name, o.Name, StringComparison.OrdinalIgnoreCase));
            if (existing != null)
            {
                if (Hash(existing) != Hash(copy)) throw new InvalidDataException($"a different model named {o.Name} is already in the level");
                models.Add(o, existing); return existing;
            }
            dst.Models.Entries.Add(copy); models.Add(o, copy); return copy;
        }
        Materials.Material Material(Materials.Material o)
        {
            if (mats.TryGetValue(o, out var cached)) return cached;
            var copy = o.Copy();
            for (int i = 0; i < o.TextureReferences.Count; i++)
            {
                var f = o.TextureReferences[i];
                copy.TextureReferences[i].Texture = f.Location == TexturePtr.Source.GLOBAL ? f.Texture : dst.Textures.ImportEntry(f.Texture);
            }
            copy.Shader = dst.Shaders.ImportEntry(o.Shader);
            var existing = dst.Materials.Entries.FirstOrDefault(m => m == copy);
            if (existing != null) copy = existing; else dst.Materials.Entries.Add(copy);
            mats.Add(o, copy); return copy;
        }
        static string Hash(Models.CS2 m) => Convert.ToHexString(SHA256.HashData(JsonSerializer.SerializeToUtf8Bytes(m,
            new JsonSerializerOptions { IncludeFields = true, NumberHandling = System.Text.Json.Serialization.JsonNumberHandling.AllowNamedFloatingPointLiterals })));
    }

    // ---------- level files (adapted from AI-MP) ----------
    sealed class Assets
    {
        public readonly string WorldRoot;
        public readonly Textures Textures; public readonly Shaders Shaders; public readonly Collisions Collisions; public readonly MorphTargets Morphs;
        public readonly Materials Materials; public readonly Models Models; public readonly RenderableElements Renderables;
        public readonly MaterialMappings Mappings; public readonly CollisionMaps CollisionMaps; public readonly EnvironmentAnimations Animations;
        public readonly Commands Commands;
        public readonly CathodeFile[] All;
        // root: a level folder (or a staging folder with RENDERABLE and WORLD); worldOverride for staging
        public Assets(string root, Global global, string? realWorld = null)
        {
            var r = Path.Combine(root, "RENDERABLE"); var w = Path.Combine(root, "WORLD"); WorldRoot = w;
            if (Directory.EnumerateFiles(r, "*.FZIP").Any()) throw new InvalidDataException("compressed level format not supported");
            Textures = new(Path.Combine(r, "LEVEL_TEXTURES.ALL.PAK"));
            Shaders = new(Path.Combine(r, "LEVEL_SHADERS_DX11.PAK"));
            Collisions = new(Path.Combine(w, "COLLISION.BIN"));
            Morphs = new(Path.Combine(w, "MORPH_TARGET_DB.BIN"));
            Materials = new(Path.Combine(r, "LEVEL_MODELS.MTL"), global.Textures, Textures, Shaders);
            Models = new(Path.Combine(r, "LEVEL_MODELS.PAK"), Materials, Collisions, Morphs);
            Renderables = new(Path.Combine(w, "REDS.BIN"), Models, Materials);
            Mappings = new(Path.Combine(w, "MATERIAL_MAPPINGS.PAK"));
            CollisionMaps = new(Path.Combine(w, "COLLISION.MAP"), Materials, Mappings);
            Animations = new(Path.Combine(w, "ENVIRONMENT_ANIMATION.DAT"), global.AnimationStrings_Debug);
            Commands = new(Path.Combine(w, "COMMANDS.PAK"), Animations, CollisionMaps, Renderables);
            All = new CathodeFile[] { Textures, Shaders, Collisions, Morphs, Materials, Mappings, Models, Renderables, CollisionMaps, Animations, Commands };
            foreach (var f in All) if (!f.Loaded) throw new InvalidDataException($"can't parse {f.Filepath}");
        }
        public void SaveTo(string stage)
        {
            Directory.CreateDirectory(Path.Combine(stage, "RENDERABLE")); Directory.CreateDirectory(Path.Combine(stage, "WORLD"));
            var old = Console.Out; Console.SetOut(TextWriter.Null);   // CathodeLib prints a line per table it saves
            try
            {
                foreach (var f in All)   // dependency order matters: it rebuilds lookup tables later files use
                {
                    var sub = f.Filepath.StartsWith(WorldRoot + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) ? "WORLD" : "RENDERABLE";
                    if (!f.Save(Path.Combine(stage, sub, Path.GetFileName(f.Filepath)))) throw new IOException($"failed to write {sub}\\{Path.GetFileName(f.Filepath)}");
                }
            }
            finally { Console.SetOut(old); }
        }
    }

    // ---------- backup, publish, restore ----------
    static void Publish(string game, string env, string level, string stage)
    {
        var bk = Path.Combine(game, "coop_backup", "levels", Key(env, level));
        foreach (var file in Directory.GetFiles(stage, "*", SearchOption.AllDirectories))
        {
            var rel = Path.GetRelativePath(stage, file);
            var target = Path.Combine(level, rel); var backup = Path.Combine(bk, rel);
            if (File.Exists(target) && !File.Exists(backup)) { Directory.CreateDirectory(Path.GetDirectoryName(backup)!); File.Copy(target, backup); }
            if (!File.Exists(target) && !File.Exists(backup + ".new")) { Directory.CreateDirectory(Path.GetDirectoryName(backup)!); File.WriteAllText(backup + ".new", ""); }
        }
        foreach (var file in Directory.GetFiles(stage, "*", SearchOption.AllDirectories))
            File.Copy(file, Path.Combine(level, Path.GetRelativePath(stage, file)), true);
    }
    static void Restore(string game, string env, string level)
    {
        var bk = Path.Combine(game, "coop_backup", "levels", Key(env, level));
        if (!Directory.Exists(bk)) { Console.WriteLine($"{Key(env, level)}: no backup"); return; }
        foreach (var f in Directory.GetFiles(bk, "*", SearchOption.AllDirectories))
        {
            var rel = Path.GetRelativePath(bk, f);
            if (rel.EndsWith(".new")) { var t = Path.Combine(level, rel[..^4]); if (File.Exists(t)) File.Delete(t); continue; }
            File.Copy(f, Path.Combine(level, rel), true);
        }
        Console.WriteLine($"{Key(env, level)}: restored");
    }
}
