// ModelAppend: writes a level's LEVEL_MODELS.PAK and MODELS_LEVEL.BIN as the level's own bytes, unchanged, with the
// new models added at the end.
//
// CathodeLib's writer rebuilds both files from what it parsed, and the game won't load what it writes (even with
// nothing added: docs/testing.md). So I never re-encode what's there. Each new submesh is copied as its raw record
// from the level it comes from, and only its index fields are changed: names, next submesh / next LOD, material,
// weighted collision, morph set, vertex formats, bone offset. Each new component's PAK header and content are
// copied the same way, with only the submesh indices and the offset changed.
//
// File layout (as CathodeLib reads it, and checked on the Steam build's levels):
//   BIN: "XMDB", submesh count, i16 record size (96), i16 version, vertex format count; vertex formats (24-byte
//        attributes, each format ends with index 0xFF); i32 length + name strings; records; i32 length + bone bytes.
//   PAK: 32-byte header (big endian; component count at 12, header count at 16); one 48-byte header per submesh
//        (the components' first, then fillers); then the content, offsets relative to its start.
using System.Buffers.Binary;
using System.Reflection;
using System.Text;
using CATHODE;
using Submesh = CATHODE.Models.CS2.Component.LOD.Submesh;

sealed class ModelFiles
{
    public const int Rec = 96, Hdr = 48;
    public readonly string PakPath, BinPath;
    public readonly byte[] Bin, Pak;
    public readonly int Count, VfStart, StrStart, StrLen, RecStart, BoneStart, BoneLen, TailStart;
    public readonly List<(int s, int e)> Vf = new();
    public readonly int Comps, Entries, ContentStart;

    public static string BinOf(string pak) => Path.Combine(Path.GetDirectoryName(pak)!, "MODELS_" + Path.GetFileName(pak).Split('_')[0] + ".BIN");

    public ModelFiles(string pakPath)
    {
        PakPath = pakPath; BinPath = BinOf(pakPath);
        Bin = File.ReadAllBytes(BinPath); Pak = File.ReadAllBytes(PakPath);
        if (Encoding.ASCII.GetString(Bin, 0, 4) != "XMDB" || I16(Bin, 8) != Rec) throw new InvalidDataException("unknown model BIN layout");
        Count = I32(Bin, 4);
        int vfCount = I32(Bin, 12), pos = 16;
        VfStart = pos;
        for (int i = 0; i < vfCount; i++)
        {
            int s = pos;
            while (true) { int idx = I32(Bin, pos); pos += 24; if (idx == 0xFF) break; }
            Vf.Add((s, pos));
        }
        StrLen = I32(Bin, pos); StrStart = pos + 4; pos = StrStart + StrLen;
        RecStart = pos; pos += Count * Rec;
        BoneLen = I32(Bin, pos); BoneStart = pos + 4; TailStart = BoneStart + BoneLen;
        if (TailStart > Bin.Length) throw new InvalidDataException("model BIN is shorter than its tables");

        if (BE(Pak, 4) != 14 || BE(Pak, 8) != 19) throw new InvalidDataException("unknown model PAK layout");
        Comps = BE(Pak, 12); Entries = BE(Pak, 16); ContentStart = 32 + Entries * Hdr;
        if (Entries != Count) throw new InvalidDataException("model PAK and BIN don't agree on the submesh count");
    }

    public int R(int i) => RecStart + i * Rec;
    public string Str(int off) { int e = Array.IndexOf(Bin, (byte)0, StrStart + off); return Encoding.Latin1.GetString(Bin, StrStart + off, e - StrStart - off); }
    public int H(int i) => 32 + i * Hdr;
    public bool Real(int i) => BE(Pak, H(i) + 32) != -1;
    public ArraySegment<byte> Content(int hdr) => new(Pak, ContentStart + BE(Pak, H(hdr) + 16), BE(Pak, H(hdr) + 8));
    public ArraySegment<byte> VfBytes(int i) => new(Bin, Vf[i].s, Vf[i].e - Vf[i].s);

    // the component headers of each model, in file order (CathodeLib adds components to a model in this order)
    public Dictionary<string, List<int>> ComponentsByModel()
    {
        var d = new Dictionary<string, List<int>>(StringComparer.Ordinal);
        for (int i = 0; i < Entries; i++)
        {
            if (!Real(i)) continue;
            var name = Str(I32(Bin, R(BE(Pak, H(i) + 32))));
            if (!d.TryGetValue(name, out var l)) d[name] = l = new();
            l.Add(i);
        }
        return d;
    }

    public static int I32(byte[] b, int o) => BinaryPrimitives.ReadInt32LittleEndian(b.AsSpan(o));
    public static short I16(byte[] b, int o) => BinaryPrimitives.ReadInt16LittleEndian(b.AsSpan(o));
    public static int BE(byte[] b, int o) => BinaryPrimitives.ReadInt32BigEndian(b.AsSpan(o));
    public static void W32(byte[] b, int o, int v) => BinaryPrimitives.WriteInt32LittleEndian(b.AsSpan(o), v);
    public static void W16(byte[] b, int o, short v) => BinaryPrimitives.WriteInt16LittleEndian(b.AsSpan(o), v);
    public static void WBE(byte[] b, int o, int v) => BinaryPrimitives.WriteInt32BigEndian(b.AsSpan(o), v);
}

// A model the importer added to the level, and where it came from.
sealed record NewModel(Models.CS2 Copy, Models.CS2 Source, ModelFiles SourceFiles);

static class ModelAppend
{
    static FieldInfo WriteListField(Models m) =>
        typeof(Models).GetField("_writeList", BindingFlags.NonPublic | BindingFlags.Instance) ?? throw new MissingFieldException("CathodeLib Models._writeList (CathodeLib version changed?)");

    public static List<Submesh> LoadedOrder(Models m) => new((List<Submesh>)WriteListField(m).GetValue(m)!);

    // Call after Materials, Collisions and MorphTargets are saved (their write indices are final) and before
    // RenderableElements is saved (it writes our submesh indices). orig: the level's files as they are now.
    // loaded: the level's submeshes in file order, from LoadedOrder before anything was changed.
    public static (byte[] pak, byte[] bin, int patched) Build(ModelFiles orig, List<Submesh> loaded, IReadOnlyList<NewModel> added,
        Materials mats, Collisions cols, MorphTargets morphs, out List<Submesh> writeOrder)
    {
        if (loaded.Count != orig.Count) throw new InvalidDataException("loaded submeshes don't match the file");

        // ---- vertex formats: reuse an identical one, else add the source's bytes
        var vfs = new List<byte[]>(); for (int i = 0; i < orig.Vf.Count; i++) vfs.Add(orig.VfBytes(i).ToArray());
        int VfIndex(ModelFiles src, int i)
        {
            if (i < 0 || i >= src.Vf.Count) throw new InvalidDataException("vertex format index out of range");
            var b = src.VfBytes(i);
            for (int k = 0; k < vfs.Count; k++) if (b.AsSpan().SequenceEqual(vfs[k])) return k;
            vfs.Add(b.ToArray()); return vfs.Count - 1;
        }

        // ---- names: reuse the level's offsets, append new ones
        var strOff = new Dictionary<string, int>(StringComparer.Ordinal);
        for (int o = 0; o < orig.StrLen;)
        {
            int e = Array.IndexOf(orig.Bin, (byte)0, orig.StrStart + o);
            if (e < 0 || e >= orig.StrStart + orig.StrLen) break;
            if (e > orig.StrStart + o) strOff.TryAdd(Encoding.Latin1.GetString(orig.Bin, orig.StrStart + o, e - orig.StrStart - o), o);
            o = e - orig.StrStart + 1;
        }
        var newStr = new List<byte>();
        int StrIndex(string s)
        {
            if (strOff.TryGetValue(s, out var o)) return o;
            o = orig.StrLen + newStr.Count; strOff[s] = o;
            newStr.AddRange(Encoding.Latin1.GetBytes(s)); newStr.Add(0); return o;
        }

        int Idx<T>(T? obj, Func<T, int> find, string what) where T : class
        {
            if (obj == null) return -1;
            int i = find(obj); if (i < 0) throw new InvalidDataException($"{what} missing from the level after import");
            return i;
        }

        // ---- the level's own records: same bytes, unless a material / collision / morph moved when its file was saved
        var recs = new byte[orig.Count * ModelFiles.Rec];
        Buffer.BlockCopy(orig.Bin, orig.RecStart, recs, 0, recs.Length);
        int patched = 0;
        for (int i = 0; i < orig.Count; i++)
        {
            var sm = loaded[i]; int o = i * ModelFiles.Rec; bool ch = false;
            int m = ModelFiles.I32(recs, o + 56);
            if (sm.Material != null && !ReferenceEquals(mats.GetAtWriteIndex(m), sm.Material)) { ModelFiles.W32(recs, o + 56, Idx(sm.Material, mats.GetWriteIndex, "material")); ch = true; }
            int c = ModelFiles.I32(recs, o + 72);
            if (sm.WeightedCollision != null && !ReferenceEquals(cols.GetAtWriteIndex(c), sm.WeightedCollision)) { ModelFiles.W32(recs, o + 72, Idx(sm.WeightedCollision, cols.GetWriteIndex, "weighted collision")); ch = true; }
            int mo = ModelFiles.I16(recs, o + 88);
            if (sm.MorphAnimSet != null && !ReferenceEquals(morphs.GetAtWriteIndex(mo), sm.MorphAnimSet)) { ModelFiles.W16(recs, o + 88, (short)Idx(sm.MorphAnimSet, morphs.GetWriteIndex, "morph set")); ch = true; }
            if (ch) patched++;
        }

        // ---- new submeshes: number them in each source component's content order
        writeOrder = new List<Submesh>(loaded);
        var newRecs = new List<byte>(); var newBones = new List<byte>();
        var newHdrs = new List<byte[]>(); var newContent = new List<(byte[] data, int hdr)>();
        foreach (var nm in added)
        {
            var src = nm.SourceFiles;
            var comps = src.ComponentsByModel().TryGetValue(nm.Source.Name, out var l) ? l : throw new InvalidDataException($"{nm.Source.Name} not found in its level's files");
            if (comps.Count != nm.Copy.Components.Count) throw new InvalidDataException($"{nm.Source.Name}: component count differs from its files");
            var map = new Dictionary<int, int>();   // source record -> new record
            var order = new List<(int srcIdx, Submesh copy)>();
            var contents = new List<byte[]>();
            for (int ci = 0; ci < comps.Count; ci++)
            {
                var content = src.Content(comps[ci]).ToArray();
                int n = ModelFiles.BE(content, 4);
                var flat = nm.Copy.Components[ci].LODs.SelectMany(x => x.Submeshes).ToList();
                if (flat.Count != n) throw new InvalidDataException($"{nm.Source.Name}: submesh count differs from its files");
                for (int k = 0; k < n; k++)
                {
                    int e = 24 + k * 16, si = ModelFiles.BE(content, e);
                    int start = ModelFiles.BE(content, e + 4), len = ModelFiles.BE(content, e + 8);
                    if (!content.AsSpan(start, len).SequenceEqual(flat[k].Data)) throw new InvalidDataException($"{nm.Source.Name}: submesh data differs from its files");
                    map[si] = orig.Count + writeOrder.Count - loaded.Count + order.Count;
                    order.Add((si, flat[k]));
                }
                contents.Add(content);
            }
            int Map(int v) => v == -1 ? -1 : map.TryGetValue(v, out var x) ? x : throw new InvalidDataException($"{nm.Source.Name}: a record points outside the model");

            foreach (var (si, copy) in order)
            {
                var r = src.Bin.AsSpan(src.R(si), ModelFiles.Rec).ToArray();
                ModelFiles.W32(r, 0, StrIndex(src.Str(ModelFiles.I32(r, 0))));
                ModelFiles.W32(r, 8, StrIndex(src.Str(ModelFiles.I32(r, 8))));
                ModelFiles.W32(r, 48, Map(ModelFiles.I32(r, 48)));
                ModelFiles.W32(r, 52, Map(ModelFiles.I32(r, 52)));
                if (ModelFiles.I32(r, 64) != -1) throw new InvalidDataException($"{nm.Source.Name}: uses a collision proxy of its level, can't be copied");
                ModelFiles.W32(r, 56, Idx(copy.Material, mats.GetWriteIndex, "material"));
                ModelFiles.W32(r, 72, Environment.GetEnvironmentVariable("SKINPREP_DIAG_NOCOL") == "1" ? -1 : Idx(copy.WeightedCollision, cols.GetWriteIndex, "weighted collision"));
                ModelFiles.W16(r, 88, Environment.GetEnvironmentVariable("SKINPREP_DIAG_NOMORPH") == "1" ? (short)-1 : (short)Idx(copy.MorphAnimSet, morphs.GetWriteIndex, "morph set"));
                for (int f = 80; f <= 84; f += 2) ModelFiles.W16(r, f, (short)VfIndex(src, (ushort)ModelFiles.I16(r, f)));
                int bones = (ushort)ModelFiles.I16(r, 94);
                if (bones > 0)
                {
                    int bo = ModelFiles.I32(r, 76);
                    ModelFiles.W32(r, 76, orig.BoneLen + newBones.Count);
                    newBones.AddRange(src.Bin.AsSpan(src.BoneStart + bo, bones).ToArray());
                }
                newRecs.AddRange(r);
                writeOrder.Add(copy);
            }
            for (int ci = 0; ci < comps.Count; ci++)
            {
                var c = contents[ci];
                ModelFiles.WBE(c, 0, Map(ModelFiles.BE(c, 0)));
                for (int k = 0, n = ModelFiles.BE(c, 4); k < n; k++) ModelFiles.WBE(c, 24 + k * 16, Map(ModelFiles.BE(c, 24 + k * 16)));
                var h = src.Pak.AsSpan(src.H(comps[ci]), ModelFiles.Hdr).ToArray();
                ModelFiles.WBE(h, 32, Map(ModelFiles.BE(h, 32)));
                newHdrs.Add(h); newContent.Add((c, newHdrs.Count - 1));
            }
        }
        int addedSub = writeOrder.Count - loaded.Count;

        // ---- BIN
        var bin = new MemoryStream();
        var head = orig.Bin.AsSpan(0, 16).ToArray();
        ModelFiles.W32(head, 4, orig.Count + addedSub); ModelFiles.W32(head, 12, vfs.Count);
        bin.Write(head);
        foreach (var v in vfs) bin.Write(v);
        var strings = new List<byte>(orig.Bin.AsSpan(orig.StrStart, orig.StrLen).ToArray());
        if (newStr.Count > 0) { strings.AddRange(newStr); strings.Add(0); while (strings.Count % 16 != 0) strings.Add(0); }
        bin.Write(BitConverter.GetBytes(strings.Count)); bin.Write(strings.ToArray());
        bin.Write(recs); bin.Write(newRecs.ToArray());
        bin.Write(BitConverter.GetBytes(orig.BoneLen + newBones.Count));
        bin.Write(orig.Bin, orig.BoneStart, orig.BoneLen); bin.Write(newBones.ToArray());
        bin.Write(orig.Bin, orig.TailStart, orig.Bin.Length - orig.TailStart);

        // ---- PAK: headers = the level's components, the new components, then fillers (one per other submesh)
        int lastReal = -1; for (int i = 0; i < orig.Entries; i++) if (orig.Real(i)) lastReal = i;
        for (int i = lastReal + 1; i < orig.Entries; i++) if (orig.Real(i)) throw new InvalidDataException("model PAK headers aren't in the usual order");
        int contentLen = orig.Pak.Length - orig.ContentStart;
        var filler = lastReal + 1 < orig.Entries ? orig.Pak.AsSpan(orig.H(lastReal + 1), ModelFiles.Hdr).ToArray() : null;
        if (filler == null && addedSub > newHdrs.Count) throw new InvalidDataException("no filler header to copy");
        var tail = new MemoryStream();
        int at = contentLen;
        foreach (var (data, hi) in newContent)
        {
            int pad = (16 - at % 16) % 16; tail.Write(new byte[pad]); at += pad;
            ModelFiles.WBE(newHdrs[hi], 16, at);
            ModelFiles.WBE(newHdrs[hi], 8, data.Length); ModelFiles.WBE(newHdrs[hi], 12, data.Length);
            tail.Write(data); at += data.Length;
        }
        var pak = new MemoryStream();
        var ph = orig.Pak.AsSpan(0, 32).ToArray();
        ModelFiles.WBE(ph, 12, orig.Comps + newHdrs.Count); ModelFiles.WBE(ph, 16, orig.Entries + addedSub);
        pak.Write(ph);
        pak.Write(orig.Pak, 32, (lastReal + 1) * ModelFiles.Hdr);
        foreach (var h in newHdrs) pak.Write(h);
        pak.Write(orig.Pak, orig.H(lastReal + 1), (orig.Entries - lastReal - 1) * ModelFiles.Hdr);
        for (int i = 0; i < addedSub - newHdrs.Count; i++) pak.Write(filler!);
        pak.Write(orig.Pak, orig.ContentStart, contentLen);
        tail.WriteTo(pak);
        return (pak.ToArray(), bin.ToArray(), patched);
    }

    // After writing the files: make CathodeLib's write indices match them, so the files saved next (REDS.BIN) point
    // at the right submeshes.
    public static void SetWriteOrder(Models m, List<Submesh> order)
    {
        var list = (List<Submesh>)WriteListField(m).GetValue(m)!;
        list.Clear(); list.AddRange(order);
        typeof(Models).GetField("_writeIndex", BindingFlags.NonPublic | BindingFlags.Instance)?.SetValue(m, null);
        typeof(Models).GetField("_writeIndexCount", BindingFlags.NonPublic | BindingFlags.Instance)?.SetValue(m, 0);
    }
}
