"""Generate shallow-copy sizes for device-create pNext nodes from pinned vk.xml."""
import pathlib
import sys
import xml.etree.ElementTree as ET

root = ET.parse(sys.argv[1]).getroot()
platforms = {p.attrib["name"]: p.attrib["protect"] for p in root.findall("./platforms/platform")}
guards = {}
unguarded = set()
for feature in root.findall("./feature"):
    if "vulkan" in feature.attrib.get("api", "").split(","):
        for required in feature.findall("require"):
            if "vulkan" in required.attrib.get("api", "vulkan").split(","):
                unguarded.update(t.attrib["name"] for t in required.findall("type") if "vulkan" in t.attrib.get("api", "vulkan").split(","))
for extension in root.findall("./extensions/extension"):
    if "vulkan" not in extension.attrib.get("supported", "").split(","):
        continue
    protect = platforms.get(extension.attrib.get("platform"))
    if extension.attrib.get("provisional") == "true":
        protect = "VK_ENABLE_BETA_EXTENSIONS"
    for t in [t for r in extension.findall("require") if "vulkan" in r.attrib.get("api", "vulkan").split(",") for t in r.findall("type")]:
        if "vulkan" not in t.attrib.get("api", "vulkan").split(","):
            continue
        if protect:
            guards.setdefault(t.attrib["name"], set()).add(protect)
        else:
            unguarded.add(t.attrib["name"])
lines = ["// Generated from the pinned Vulkan registry; do not edit.", "switch (type) {"]
seen = set()
for t in root.findall("./types/type[@category='struct']"):
    name = t.attrib.get("name")
    if not name or "alias" in t.attrib or t.attrib.get("api", "vulkan") == "vulkansc":
        continue
    stype = next((m.attrib.get("values") for m in t.findall("member") if m.findtext("name") == "sType"), None)
    if not stype or stype in seen or (name not in unguarded and name not in guards):
        continue
    seen.add(stype)
    protected = name not in unguarded
    if protected:
        lines.append("#if " + " || ".join(f"defined({g})" for g in sorted(guards[name])))
    lines.append(f"case {stype}: return sizeof({name});")
    if protected:
        lines.append("#endif")
lines.extend(["default: return 0;", "}", ""])
pathlib.Path(sys.argv[2]).write_text("\n".join(lines), encoding="utf-8")
