// levelmod: inspect and patch Alien: Isolation level scripts (COMMANDS.PAK) with CathodeLib.
//   find <levelsDir> <text>                  composites / entities / function types matching text
//   funcs <COMMANDS.PAK> <FuncType>          every entity of a function type, with parameters
//   ids / comp                               dump entity IDs / one composite
//   drivenpc <COMMANDS.PAK> <composite> <npc>  add the partner-body marker, go-to and loop next to an NPC
//   addpartner                               old attempt at a new character (engine never spawns it)
using System;
using System.IO;
using System.Linq;
using System.Collections.Generic;
using CATHODE;
using CATHODE.Scripting;
using CathodeLib;

static class P {
    static Global _g;
    static Commands Load(string pak) {
        // pak = <DATA>\ENV\PRODUCTION\<LEVEL>\WORLD\COMMANDS.PAK ; Level needs Global (DATA\ENV\GLOBAL)
        string lvl = Path.GetDirectoryName(Path.GetDirectoryName(pak));
        string env = Path.GetDirectoryName(lvl);   // walk up to DATA\ENV (DLC maps sit one level deeper)
        while (env != null && !Path.GetFileName(env).Equals("ENV", StringComparison.OrdinalIgnoreCase)) env = Path.GetDirectoryName(env);
        if (env == null) throw new Exception("not under DATA/ENV: " + pak);
        _g ??= new Global(Path.Combine(env, "GLOBAL"));
        var level = new Level(lvl, _g);
        if (level.Commands == null || !level.Commands.Loaded) throw new Exception("failed to load " + pak);
        return level.Commands;
    }
    static int Main(string[] a) {
        if (a[0] == "find") {
            foreach (var lvl in Directory.GetDirectories(a[1])) {
                var pak = Path.Combine(lvl, "WORLD", "COMMANDS.PAK");
                if (!File.Exists(pak)) continue;
                Commands c;
                try { c = Load(pak); } catch (Exception e) { Console.WriteLine($"{Path.GetFileName(lvl)}: LOAD FAIL {e.Message}"); continue; }
                int hits = 0;
                foreach (var comp in c.Entries) {
                    if (comp.name.IndexOf(a[2], StringComparison.OrdinalIgnoreCase) >= 0) { if (hits++ < 8) Console.WriteLine($"{Path.GetFileName(lvl)}: composite {comp.name}"); }
                    foreach (var f in comp.functions) {
                        string n = c.Utils.GetEntityName(comp, f) ?? "";
                        if (n.IndexOf(a[2], StringComparison.OrdinalIgnoreCase) >= 0) { if (hits++ < 8) Console.WriteLine($"{Path.GetFileName(lvl)}: entity '{n}' type {f.function} in {comp.name}"); }
                    }
                }
                Console.WriteLine($"{Path.GetFileName(lvl)}: {c.Entries.Count} composites, {hits} hits");
            }
            return 0;
        }
        if (a[0] == "funcs") {
            var c = Load(a[1]);
            foreach (var comp in c.Entries)
                foreach (var f in comp.functions.Where(f => f.function.ToString() == a[2])) {
                    Console.WriteLine($"{comp.name} :: {c.Utils.GetEntityName(comp, f)} [{f.shortGUID.ToByteString()}]");
                    foreach (var p in f.parameters) Console.WriteLine($"    {p.name} = {p.content}");
                    foreach (var l in f.childLinks) Console.WriteLine($"    link {l.thisParamID} -> {l.linkedParamID} ({c.Utils.GetEntityName(comp.shortGUID, l.linkedEntityID)})");
                }
            return 0;
        }
        if (a[0] == "addpartner") {
            // addpartner <COMMANDS.PAK> <archetype composite> <x> <y> <z>
            // Adds to the level's root composite: an actor instance (the partner body), a PositionMarker seeded with
            // a magic position (coopnet finds it in memory and overwrites it), CMD_GoTo -> marker, a TriggerBindCharacter
            // binding the command to the actor, and a LogicDelay loop that re-applies the command every 0.25 s.
            var c = Load(a[1]);
            var root = c.EntryPoints[0];
            if (root.functions.Any(f => c.Utils.GetEntityName(root, f) == "COOP_Partner")) { Console.WriteLine("already added"); return 3; }
            var arche = c.GetComposite(a[2]) ?? throw new Exception("no archetype " + a[2]);
            float x = float.Parse(a[3], System.Globalization.CultureInfo.InvariantCulture), y = float.Parse(a[4], System.Globalization.CultureInfo.InvariantCulture), z = float.Parse(a[5], System.Globalization.CultureInfo.InvariantCulture);

            var partner = root.AddFunction(arche);
            c.Utils.SetEntityName(root, partner, "COOP_Partner");
            partner.AddParameter("position", new cTransform(new System.Numerics.Vector3(x, y, z), new System.Numerics.Vector3(0, 0, 0)));
            partner.AddParameter("spawn_on_reset", new cBool(true));
            // Without a character model the engine spawns nothing: copy what the story's civilians set
            // (HAB_AIRPORT M04_Pt01 scared_civvie_01).
            partner.AddParameter("custom_character_type", new cEnum(EnumType.CUSTOM_CHARACTER_TYPE, 2));
            partner.AddParameter("named_custom_character", new cString("CREW_MEDICAL"));
            partner.AddParameter("dialogue_voice", new cEnum(EnumType.DIALOGUE_VOICE_ACTOR, 2));
            partner.AddParameter("disable_looting", new cBool(true));
            partner.AddParameter("is_cinematic", new cBool(false));
            partner.AddParameter("holster_weapon_on_start", new cBool(true));
            partner.AddParameter("Debug_name", new cString("COOP_Partner"));

            var marker = root.AddFunction(FunctionType.PositionMarker);
            c.Utils.SetEntityName(root, marker, "COOP_Target");
            marker.AddParameter("position", new cTransform(new System.Numerics.Vector3(1234.5f, 987.25f, 4321.75f), new System.Numerics.Vector3(0, 0, 0)));

            var go = root.AddFunction(FunctionType.CMD_GoTo);
            c.Utils.SetEntityName(root, go, "COOP_GoTo");
            go.AddParameter("move_type", new cEnum(EnumType.MOVE, 3));
            go.AddParameter("override_all_ai", new cBool(true));
            go.AddParameter("use_stopping_anim", new cBool(true));
            go.AddParameter("arrived_radius", new cFloat(1.0f));
            go.AddParameterLink("Waypoint", marker, "reference");

            var bind = root.AddFunction(FunctionType.TriggerBindCharacter);
            c.Utils.SetEntityName(root, bind, "COOP_Bind");
            bind.AddParameterLink("characters", partner, "npc_reference");
            bind.AddParameterLink("bound_trigger", go, "apply_start");

            var loop = root.AddFunction(FunctionType.LogicDelay);
            c.Utils.SetEntityName(root, loop, "COOP_Loop");
            loop.AddParameter("delay", new cFloat(0.25f));
            loop.AddParameter("start_on_reset", new cBool(true));
            loop.AddParameterLink("on_delay_finished", bind, "trigger");
            loop.AddParameterLink("on_delay_finished", loop, "trigger");      // keep looping
            partner.AddParameterLink("finished_spawning", loop, "trigger");  // and start once the actor exists

            if (!c.Save()) { Console.WriteLine("SAVE FAILED"); return 4; }
            Console.WriteLine($"added partner ({a[2]}) at {x},{y},{z}; saved {a[1]}");
            return 0;
        }
        if (a[0] == "drivenpc") {
            // drivenpc <COMMANDS.PAK> <composite> <npc entity name>
            // Lets the co-op mod move an NPC the level already has (characters added through the level file are
            // not spawned by the engine). Adds to <composite>, next to the NPC: a PositionMarker seeded with a magic
            // position (coopnet overwrites it in memory), CMD_GoTo -> marker, a TriggerBindCharacter binding the
            // command to the NPC, and a LogicDelay loop re-applying it every 0.25 s once triggered.
            var c = Load(a[1]);
            var comp = c.Entries.FirstOrDefault(x => x.name == a[2]) ?? throw new Exception("no composite " + a[2]);
            if (comp.functions.Any(f => c.Utils.GetEntityName(comp, f) == "COOP_Target")) { Console.WriteLine("already added"); return 3; }
            var npc = comp.functions.FirstOrDefault(f => c.Utils.GetEntityName(comp, f) == a[3]) ?? throw new Exception("no entity " + a[3]);
            var marker = comp.AddFunction(FunctionType.PositionMarker);
            marker.shortGUID = ShortGuidUtils.Generate("COOP_Target");   // fixed ID: the mod addresses it by this
            c.Utils.SetEntityName(comp, marker, "COOP_Target");
            marker.AddParameter("position", new cTransform(new System.Numerics.Vector3(1234.5f, 987.25f, 4321.75f), new System.Numerics.Vector3(0, 0, 0)));
            var go = comp.AddFunction(FunctionType.CMD_GoTo);
            go.shortGUID = ShortGuidUtils.Generate("COOP_GoTo");   // fixed ID: the mod addresses it by this
            c.Utils.SetEntityName(comp, go, "COOP_GoTo");
            go.AddParameter("move_type", new cEnum(EnumType.MOVE, 3));
            go.AddParameter("override_all_ai", new cBool(true));
            go.AddParameter("arrived_radius", new cFloat(0.5f));
            go.AddParameterLink("Waypoint", marker, "reference");
            var bind = comp.AddFunction(FunctionType.TriggerBindCharacter);
            bind.shortGUID = ShortGuidUtils.Generate("COOP_Bind");   // fixed ID: the mod addresses it by this
            c.Utils.SetEntityName(comp, bind, "COOP_Bind");
            bind.AddParameterLink("characters", npc, "npc_reference");
            bind.AddParameterLink("bound_trigger", go, "apply_start");
            var loop = comp.AddFunction(FunctionType.LogicDelay);
            loop.shortGUID = ShortGuidUtils.Generate("COOP_Loop");   // fixed ID: the mod addresses it by this
            c.Utils.SetEntityName(comp, loop, "COOP_Loop");
            loop.AddParameter("delay", new cFloat(0.25f));
            loop.AddParameterLink("on_delay_finished", bind, "trigger");
            loop.AddParameterLink("on_delay_finished", loop, "trigger");
            if (!c.Save()) { Console.WriteLine("SAVE FAILED"); return 4; }
            Console.WriteLine($"drive logic for {a[3]} added to {a[2]}; saved {a[1]}");
            return 0;
        }
        if (a[0] == "ids") {
            // ids <COMMANDS.PAK>: every ShortGuid the level script defines, one per line, as the little-endian dword
            // the engine would hold in memory: kind  hex  composite  name  type. For matching runtime dumps.
            var c = Load(a[1]);
            foreach (var comp in c.Entries) {
                Console.WriteLine($"C\t{comp.shortGUID.AsUInt32:x8}\t{comp.name}\t\t");
                foreach (var f in comp.functions) {
                    var fc = c.GetComposite(f.function);
                    Console.WriteLine($"E\t{f.shortGUID.AsUInt32:x8}\t{comp.name}\t{c.Utils.GetEntityName(comp, f)}\t{(fc != null ? fc.name : f.function.ToString())}");
                }
                foreach (var v in comp.variables) Console.WriteLine($"V\t{v.shortGUID.AsUInt32:x8}\t{comp.name}\t{v.name}\t{v.type}");
                foreach (var px in comp.proxies) Console.WriteLine($"X\t{px.shortGUID.AsUInt32:x8}\t{comp.name}\t{c.Utils.GetEntityName(comp, px)}\tproxy");
                foreach (var al in comp.aliases) Console.WriteLine($"A\t{al.shortGUID.AsUInt32:x8}\t{comp.name}\t{c.Utils.GetEntityName(comp, al)}\talias");
            }
            return 0;
        }
        if (a[0] == "instances") {
            // instances <COMMANDS.PAK>: walk every composite instance from the level root and print the instance ID
            // CathodeLib computes for it (both conventions), with its composite and the entity path. For matching the
            // IDs the engine keeps on composite-instance entities at runtime.
            var c = Load(a[1]);
            int n = 0;
            void Walk(Composite comp, List<ShortGuid> path, int depth) {
                if (depth > 16) return;
                foreach (var f in comp.functions) {
                    var child = c.GetComposite(f.function);
                    if (child == null) continue;
                    var p = new List<ShortGuid>(path) { f.shortGUID };
                    var withEnt = p.ToArray().GenerateCompositeInstanceID(true);
                    var noEnt = p.ToArray().GenerateCompositeInstanceID(false);
                    Console.WriteLine($"{withEnt.AsUInt32:x8}\t{noEnt.AsUInt32:x8}\t{child.name}\t{string.Join("/", p.Select(g => g.AsUInt32.ToString("x8")))}");
                    n++;
                    Walk(child, p, depth + 1);
                }
            }
            Walk(c.EntryPoints[0], new List<ShortGuid>(), 0);
            Console.Error.WriteLine($"{n} instances");
            return 0;
        }
        if (a[0] == "comp") {
            var c = Load(a[1]);
            var comp = c.Entries.FirstOrDefault(x => x.name == a[2]);
            if (comp == null) { Console.WriteLine("no composite " + a[2]); return 2; }
            Console.WriteLine($"composite {comp.name} [{comp.shortGUID.ToByteString()}] vars={comp.variables.Count} funcs={comp.functions.Count} proxies={comp.proxies.Count} aliases={comp.aliases.Count}");
            foreach (var v in comp.variables) Console.WriteLine($"  VAR {v.name} : {v.type}");
            foreach (var f in comp.functions) {
                var fc = c.GetComposite(f.function);
                Console.WriteLine($"  FUNC {c.Utils.GetEntityName(comp, f)} : {(fc != null ? "composite " + fc.name : f.function.ToString())}");
                foreach (var p in f.parameters) Console.WriteLine($"      {p.name} = {p.content}");
                foreach (var l in f.childLinks) Console.WriteLine($"      link {l.thisParamID} -> {l.linkedParamID} ({c.Utils.GetEntityName(comp.shortGUID, l.linkedEntityID)})");
            }
            foreach (var px in comp.proxies) {
                Console.WriteLine($"  PROXY {c.Utils.GetEntityName(comp, px)}");
                foreach (var l in px.childLinks) Console.WriteLine($"      link {l.thisParamID} -> {l.linkedParamID} ({c.Utils.GetEntityName(comp.shortGUID, l.linkedEntityID)})");
            }
            foreach (var al in comp.aliases) {
                Console.WriteLine($"  ALIAS {c.Utils.GetEntityName(comp, al)}");
                foreach (var p in al.parameters) Console.WriteLine($"      {p.name} = {p.content}");
                foreach (var l in al.childLinks) Console.WriteLine($"      link {l.thisParamID} -> {l.linkedParamID} ({c.Utils.GetEntityName(comp.shortGUID, l.linkedEntityID)})");
            }
            return 0;
        }
        return 1;
    }
}
