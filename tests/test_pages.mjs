// Lays out DSPreset's pages with the HOSTS' OWN planner (dbxhost
// src/shared/param_pages, what stock and dAVEBOx both run) and checks what a
// user would see. A green C suite only says the params are wired; this says
// the Amp/Voice page draws an envelope. Usage: node test_pages.mjs <param_pages dir> <contract.json>
import { readFileSync } from "fs";
const [dir, contractPath] = process.argv.slice(2);
const { planPages } = await import(dir + "/page_plan.mjs");
const { resolveViz } = await import(dir + "/viz.mjs");
const { buildMetaIndex } = await import(dir + "/param_meta.mjs");
const c = JSON.parse(readFileSync(contractPath, "utf8"));
const plan = planPages({ hierarchy: c.hierarchy, chainParams: c.chainParams });
const pages = plan.pages || plan;
const metaIndex = buildMetaIndex({ hierarchy: c.hierarchy, chainParams: c.chainParams });
const fail = (why) => { console.error("FAIL " + why); process.exit(1); };
const describe = (p) => {
    const { groups } = p.keys ? resolveViz({ keys: p.keys, metaIndex }) : { groups: [] };
    return { kind: p.kind, name: p.name, keys: p.keys || [], groups };
};
const seen = pages.map(describe);
for (const p of seen) console.log(`  ${p.kind.padEnd(6)} ${String(p.name).padEnd(14)} ${p.groups.map((g) => g.kind + "[" + g.keys.join("+") + "]").join(" ")}`);
/* Banks, Presets, Main, Amp/Voice (Josh, 2026-09-19); you land on Main */
const order = seen.map((p) => p.kind + ":" + p.name).join(",");
if (order !== "items:Banks,preset:Presets,knobs:Main,knobs:Amp/Voice") fail("page order: " + order);
const { firstGrid } = await import(dir + "/page_nav.mjs");
if (pages[firstGrid(pages)].name !== "Main") fail("lands on " + pages[firstGrid(pages)].name + ", not Main");
if (!seen[2].keys.length) fail("Main has no knobs");
/* choosing a bank goes to its presets: the controller's rule (page_controller, navigateTo) */
const banks = pages[0];
if (pages.findIndex((q) => q.level === banks.navigateTo && q.kind === "preset") !== 1) fail("choosing a bank does not open Presets");
const amp = seen.find((p) => p.name === "Amp/Voice");
if (!amp) fail("no Amp/Voice page");
const env = amp.groups.find((g) => g.kind === "envelope");
if (!env) fail("Amp/Voice does not draw an envelope");
if (env.keys.join(",") !== "amp_attack,amp_decay,amp_sustain,amp_release") fail("the envelope is not attack..release: " + env.keys);
if (!amp.groups.some((g) => g.kind === "switch" && g.keys[0] === "amp_override")) fail("Override is not a switch");
if (JSON.stringify(amp.keys) !== JSON.stringify(["amp_attack", "amp_decay", "amp_sustain", "amp_release", "amp_override", "polyphony", "", "gain"]))
    fail("Amp/Voice knobs: " + JSON.stringify(amp.keys));
if (seen.some((p) => p !== amp && p.keys.includes("gain"))) fail("Gain is on a page besides Amp/Voice");
console.log("pages test passed");
