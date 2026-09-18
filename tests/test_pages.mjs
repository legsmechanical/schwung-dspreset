// Lays out DSPreset's pages with the HOSTS' OWN planner (dbxhost
// src/shared/param_pages, what stock and dAVEBOx both run) and checks what a
// user would see. A green C suite only says the params are wired; this says
// the Amp Envelope page draws an envelope. Usage: node test_pages.mjs <param_pages dir> <contract.json>
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
if (seen[0].kind !== "preset") fail("the first page is not the preset browser");
if (!seen.some((p) => p.kind === "items" && p.name === "Banks")) fail("no Banks page");
const amp = seen.find((p) => p.name === "Amp Envelope");
if (!amp) fail("no Amp Envelope page");
const env = amp.groups.find((g) => g.kind === "envelope");
if (!env) fail("Amp Envelope does not draw an envelope");
if (env.keys.join(",") !== "amp_attack,amp_decay,amp_sustain,amp_release") fail("the envelope is not attack..release: " + env.keys);
if (!amp.groups.some((g) => g.kind === "switch" && g.keys[0] === "amp_override")) fail("Override is not a switch");
console.log("pages test passed");
