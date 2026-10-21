# Adapted from vrdual/tools/gen_mirror.py; see MIRROR_ORIGIN.md.
#!/usr/bin/env python3
"""Generates mirror_gen.hpp-style code for paired resources in one process from the vendored vk.xml.

For every device-level command (first parameter VkDevice/VkQueue/VkCommandBuffer)
it emits one of:
  * nothing    -- A-only: the layer passes it straight to the app's device
                  (queries, debug names, ...);
  * a wrapper  -- call on the app's device A, then translate every handle
                  (deeply: arrays, nested structs, pNext chains) to device B's
                  twin and call it again on B; created handles are recorded;
  * a stub     -- mirrored but not translatable (pointer-to-pointer, unions with
                  handles, ...): calls A and stops the mirror with a log line.
Commands listed in MANUAL are written by hand in mirror_backend.h.

usage: gen_mirror.py vk.xml dispatch.hpp wrappers.hpp
"""
import re
import sys
import xml.etree.ElementTree as ET

# Hand-written in the layer (memory mapping, sync, WSI, shaders, queues, ...).
MANUAL = {
    'vkGetDeviceProcAddr', 'vkDestroyDevice', 'vkGetDeviceQueue', 'vkGetDeviceQueue2',
    'vkCreateShaderModule', 'vkAllocateMemory', 'vkFreeMemory', 'vkMapMemory', 'vkUnmapMemory',
    'vkMapMemory2', 'vkUnmapMemory2', 'vkFlushMappedMemoryRanges', 'vkInvalidateMappedMemoryRanges',
    'vkQueueSubmit', 'vkQueueSubmit2', 'vkQueuePresentKHR', 'vkCreateSwapchainKHR',
    'vkDestroySwapchainKHR', 'vkGetSwapchainImagesKHR', 'vkAcquireNextImageKHR', 'vkAcquireNextImage2KHR',
    'vkWaitForFences', 'vkGetFenceStatus', 'vkResetFences', 'vkWaitSemaphores',
    'vkGetSemaphoreCounterValue', 'vkAllocateCommandBuffers', 'vkFreeCommandBuffers',
    'vkCreateDescriptorUpdateTemplate', 'vkDestroyDescriptorUpdateTemplate',
    'vkUpdateDescriptorSetWithTemplate', 'vkCmdPushDescriptorSetWithTemplate',
    'vkCmdPushDescriptorSetWithTemplate2', 'vkCreateGraphicsPipelines', 'vkBeginCommandBuffer',
    'vkCreateBuffer', 'vkGetBufferDeviceAddress', 'vkCmdPushConstants', 'vkCmdPushConstants2', 'vkCmdBindPipeline', 'vkCreateComputePipelines',
}

# Mirrored commands that are fine to run on A only (no GPU-visible effect on B).
A_ONLY = {
    'vkSetSwapchainPresentTimingQueueSizeEXT', 'vkSetLatencySleepModeLegacyNV',
    'vkDebugMarkerSetObjectNameEXT', 'vkDebugMarkerSetObjectTagEXT', 'vkSetDebugUtilsObjectNameEXT',
    'vkSetDebugUtilsObjectTagEXT', 'vkSetPrivateData', 'vkCreatePrivateDataSlot', 'vkDestroyPrivateDataSlot',
    'vkAcquireProfilingLockKHR', 'vkReleaseProfilingLockKHR', 'vkSetHdrMetadataEXT', 'vkWaitForPresentKHR',
    'vkWaitForPresent2KHR', 'vkDisplayPowerControlEXT', 'vkRegisterDeviceEventEXT', 'vkRegisterDisplayEventEXT',
    'vkCreateValidationCacheEXT', 'vkDestroyValidationCacheEXT', 'vkMergeValidationCachesEXT',
    'vkSetLocalDimmingAMD', 'vkReleaseSwapchainImagesEXT', 'vkReleaseSwapchainImagesKHR', 'vkAntiLagUpdateAMD',
    'vkSetLatencySleepModeNV', 'vkLatencySleepNV', 'vkSetLatencyMarkerNV', 'vkQueueNotifyOutOfBandNV',
    'vkReleaseFullScreenExclusiveModeEXT', 'vkAcquireFullScreenExclusiveModeEXT', 'vkSetDeviceMemoryPriorityEXT',
    'vkCreateCuModuleNVX', 'vkCreateCuFunctionNVX', 'vkDestroyCuModuleNVX', 'vkDestroyCuFunctionNVX',
    'vkQueueBeginDebugUtilsLabelEXT', 'vkQueueEndDebugUtilsLabelEXT', 'vkQueueInsertDebugUtilsLabelEXT',
    'vkCmdBeginDebugUtilsLabelEXT', 'vkCmdEndDebugUtilsLabelEXT', 'vkCmdInsertDebugUtilsLabelEXT',
    'vkCmdDebugMarkerBeginEXT', 'vkCmdDebugMarkerEndEXT', 'vkCmdDebugMarkerInsertEXT',
}

# Instance-level handles: never translated (same value on both sides is meaningless
# but they only appear in instance-level or WSI paths handled by hand).
INSTANCE_HANDLES = {'VkInstance', 'VkPhysicalDevice', 'VkSurfaceKHR', 'VkDisplayKHR', 'VkDisplayModeKHR',
                    'VkDebugUtilsMessengerEXT', 'VkDebugReportCallbackEXT'}
DISPATCHABLE_DEVICE = {'VkDevice', 'VkQueue', 'VkCommandBuffer'}

# Structs translated by hand (members that are ignored depending on other fields
# may hold garbage pointers, so generic deep translation would crash).
MANUAL_STRUCTS = {'VkWriteDescriptorSet', 'VkFramebufferCreateInfo', 'VkDescriptorSetLayoutBinding',
                  'VkShaderModuleCreateInfo'}


def text_of(e):
    return ''.join(e.itertext())


def api_ok(e):
    a = e.get('api')
    return a is None or 'vulkan' in a.split(',')


class Member:
    def __init__(self, e):
        self.name = e.find('name').text
        self.type = e.find('type').text
        te, ne = e.find('type'), e.find('name')
        pre, mid, rest = e.text or '', te.tail or '', ne.tail or ''
        self.ptr = mid.count('*')
        self.const = 'const' in pre
        m = re.search(r'\[([^\]]+)\]', rest)
        enum = e.find('enum')
        self.array = (enum.text if enum is not None else m.group(1)) if m else None
        self.len = e.get('altlen') or e.get('len')
        if self.len:
            self.len = self.len.split(',')[0]
        self.optional = e.get('optional', '')
        self.noauto = e.get('noautovalidity') == 'true'
        self.values = e.get('values')


def main(xml_path, disp_path, out_path):
    root = ET.parse(xml_path).getroot()

    # ---- which commands/types are part of the non-platform, non-provisional API
    required_cmds, required_types = set(), set()
    blocked_types = set()

    def collect(req_parent, into_cmds, into_types):
        for req in req_parent.findall('require'):
            if not api_ok(req):
                continue
            for c in req.findall('command'):
                into_cmds.add(c.get('name'))
            for t in req.findall('type'):
                into_types.add(t.get('name'))

    for feat in root.findall('feature'):
        if 'vulkan' not in feat.get('api', '').split(','):
            continue
        collect(feat, required_cmds, required_types)
    for ext in root.find('extensions').findall('extension'):
        sup = ext.get('supported', '').split(',')
        bad = 'vulkan' not in sup or ext.get('platform') or ext.get('provisional') == 'true'
        if bad:
            tc, tt = set(), set()
            collect(ext, tc, tt)
            blocked_types |= tt
            continue
        collect(ext, required_cmds, required_types)

    # ---- types
    handles, aliases_t, structs, unions, returnedonly, extends = {}, {}, {}, set(), set(), {}
    stype_of = {}
    for t in root.find('types').findall('type'):
        if not api_ok(t):
            continue
        cat, name = t.get('category'), t.get('name') or (t.find('name').text if t.find('name') is not None else None)
        if t.get('alias'):
            aliases_t[name] = t.get('alias')
            continue
        if cat == 'handle':
            handles[name] = t.find('type').text == 'VK_DEFINE_HANDLE'
        elif cat in ('struct', 'union'):
            ms = [Member(m) for m in t.findall('member') if api_ok(m)]
            structs[name] = ms
            if cat == 'union':
                unions.add(name)
            if t.get('returnedonly') == 'true':
                returnedonly.add(name)
            for m in ms:
                if m.name == 'sType' and m.values:
                    stype_of[name] = m.values
            if t.get('structextends'):
                extends[name] = t.get('structextends').split(',')

    def canon(t):
        while t in aliases_t:
            t = aliases_t[t]
        return t

    def is_handle(t):
        return canon(t) in handles

    def xl_handle(t):  # needs translation to B
        t = canon(t)
        return t in handles and t not in INSTANCE_HANDLES

    required_canon = {canon(t) for t in required_types}
    usable = lambda s: s not in blocked_types and s in structs and s in required_canon

    # structs extended by S (reverse: who can sit in X's pNext chain)
    chain_members = {}
    for s, parents in extends.items():
        if not usable(s):
            continue
        for p in parents:
            chain_members.setdefault(canon(p), set()).add(s)

    # NT: struct may (transitively) carry a device handle on input
    nt = {}

    def struct_nt(s, stack=()):
        s = canon(s)
        if s in nt:
            return nt[s]
        if s in stack or not usable(s):
            return False
        r = any(canon(m.type) == 'VkImageLayout' for m in structs[s])
        for m in structs[s]:
            if xl_handle(m.type):
                r = True
            elif canon(m.type) in structs and canon(m.type) != s and struct_nt(m.type, stack + (s,)):
                r = True
        for c in chain_members.get(s, ()):
            if c != s and c not in returnedonly and struct_nt(c, stack + (s,)):
                r = True
        if s in stack:
            return r
        nt[s] = r
        return r

    for s in structs:
        struct_nt(s)
    # second pass to settle recursion
    for _ in range(3):
        nt.clear()
        for s in structs:
            struct_nt(s)

    # can we translate s generically?
    bad_reason = {}

    def check_struct(s):
        s = canon(s)
        if s in MANUAL_STRUCTS:
            return None
        if s in unions and nt.get(s):
            return 'union with handles'
        for m in structs[s]:
            ct = canon(m.type)
            needs = ct == 'VkImageLayout' or xl_handle(ct) or (ct in structs and nt.get(ct))
            if not needs or m.name == 'pNext':
                continue
            if m.ptr > 1:
                return f'{m.name}: pointer to pointer'
            if m.ptr == 1 and not m.const:
                return f'{m.name}: non-const pointer'
            if m.ptr == 1 and m.noauto and xl_handle(ct) is False and not nt.get(ct):
                continue
            if m.ptr == 1 and m.noauto:
                return f'{m.name}: conditionally ignored pointer'
            if m.ptr == 1 and m.len and not re.match(r'^[A-Za-z_][A-Za-z0-9_]*$', m.len):
                return f'{m.name}: len {m.len}'
        return None

    for s in structs:
        if nt.get(s) and usable(s) and s not in returnedonly:
            r = check_struct(s)
            if r:
                bad_reason[s] = r
    # a struct holding an untranslatable one by value/pointer is untranslatable too
    changed = True
    while changed:
        changed = False
        for s in structs:
            if s in bad_reason or not nt.get(s) or not usable(s) or s in returnedonly or s in MANUAL_STRUCTS:
                continue
            for m in structs[s]:
                if m.name != 'pNext' and canon(m.type) in bad_reason:
                    bad_reason[s] = f'{m.name}: {canon(m.type)}'
                    changed = True
                    break

    # ---- commands
    cmds, cmd_alias = {}, {}
    for c in root.find('commands').findall('command'):
        if not api_ok(c):
            continue
        if c.get('alias'):
            cmd_alias[c.get('name')] = c.get('alias')
            continue
        proto = c.find('proto')
        name = proto.find('name').text
        ret = proto.find('type').text
        params = [Member(p) for p in c.findall('param') if api_ok(p)]
        cmds[name] = (ret, params)

    device_cmds = {}
    for name, (ret, params) in cmds.items():
        names = [name] + [a for a, t in cmd_alias.items() if t == name]
        if not any(n in required_cmds for n in names):
            continue
        if canon(params[0].type) not in DISPATCHABLE_DEVICE:
            continue
        # skip commands whose parameter types are platform/provisional
        if any(canon(p.type) in blocked_types for p in params):
            continue
        device_cmds[name] = (ret, params, [n for n in names if n in required_cmds or n == name])

    def classify(name, ret, params):
        allnames = device_cmds[name][2]
        if any(n in MANUAL for n in allnames):
            return 'manual'
        if any(n in A_ONLY for n in allnames) or name.startswith('vkGet'):
            return 'aonly'
        if name in ('vkCmdBindDescriptorBuffersEXT', 'vkCmdBindDescriptorBufferEmbeddedSamplersEXT'):
            return 'stub:opaque descriptor buffers require address translation'
        if ret not in ('void', 'VkResult'):
            return 'aonly'
        for p in params[1:]:
            ct = canon(p.type)
            needs = ct == 'VkImageLayout' or xl_handle(ct) or (ct in structs and nt.get(ct))
            if p.ptr == 1 and not p.const and xl_handle(ct):
                continue  # created handles
            if not needs:
                if p.ptr >= 1 and not p.const and ct != 'void' and ct in structs:
                    pass  # output struct: B gets scratch
                continue
            if p.ptr > 1:
                return 'stub:pointer to pointer ' + p.name
            if ct in bad_reason:
                return f'stub:{ct} ({bad_reason[ct]})'
            if p.ptr == 1 and not p.const:
                return 'stub:non-const struct with handles ' + p.name
        return 'gen'

    out = []
    w = out.append
    w('// Generated by tools/gen_mirror.py from vk.xml. Do not edit.')
    w('// Included by layer/vrdual_layer.cpp inside namespace mirror; expects:')
    w('//   struct Ctx (arena alloc<T>(n), xl<T>(handle), record(a, b)), MDev (a, b dispatch, bdev),')
    w('//   MDev* mdev_of(void* dispatchable), Ctx& ctx_begin(MDev*), void mirror_fail(MDev*, const char*),')
    w('//   and hand-written translators tr_<S> for MANUAL_STRUCTS.')
    w('')

    # dispatch table (separate file: the layer's structs embed it)
    wrappers_head = out[:]
    out.clear()
    w('// Generated by tools/gen_mirror.py from vk.xml. Do not edit.')
    w('struct Dispatch {')
    for name in sorted(device_cmds):
        w(f'    PFN_{name} {name[2:]} = nullptr;')
    w('};')
    w('')
    w('template <typename GDPA, typename D> static void load_dispatch(Dispatch& d, GDPA gdpa, D dev) {')
    for name in sorted(device_cmds):
        names = device_cmds[name][2]
        names = [name] + [n for n in names if n != name]
        w(f'    d.{name[2:]} = (PFN_{name})gdpa(dev, "{names[0]}");')
        for n in names[1:]:
            w(f'    if (!d.{name[2:]}) d.{name[2:]} = (PFN_{name})gdpa(dev, "{n}");')
    w('}')
    w('')
    w('// sizeof by sType, 0 = unknown (used to copy a pNext chain node by node)')
    w('static size_t stype_size(VkStructureType st) {')
    w('    switch (st) {')
    seen = set()
    for s_ in sorted(structs):
        if s_ in stype_of and usable(s_) and stype_of[s_] not in seen:
            seen.add(stype_of[s_])
            w(f'    case {stype_of[s_]}: return sizeof({s_});')
    w('    default: return 0;')
    w('    }')
    w('}')
    with open(disp_path, 'w') as f:
        f.write('\n'.join(out) + '\n')
    out.clear()
    out.extend(wrappers_head)

    # forward declarations of struct translators
    gen_structs = sorted(s for s in structs if nt.get(s) and usable(s) and s not in returnedonly and s not in bad_reason)
    for s in gen_structs:
        w(f'static void tr_{s}(Ctx& c, const {s}* in, {s}* out);')
    w('static const void* tr_chain(Ctx& c, const void* p);')
    w('')

    def emit_member_fix(s, m, dst, src):
        ct = canon(m.type)
        lines = []
        if m.name == 'pNext':
            lines.append(f'    {dst}pNext = (decltype({dst}pNext))tr_chain(c, {src}pNext);')
            return lines
        if ct == 'VkImageLayout' and not m.ptr:
            lines.append(f'    {dst}{m.name} = eye_layout({src}{m.name});')
        elif xl_handle(ct):
            if m.ptr == 0 and m.array:
                lines.append(f'    for (uint32_t i = 0; i < (uint32_t)({m.array}); ++i) {dst}{m.name}[i] = c.xl({src}{m.name}[i]);')
            elif m.ptr == 0:
                lines.append(f'    {dst}{m.name} = c.xl({src}{m.name});')
            else:
                n = f'{src}{m.len}' if m.len else '1'
                lines.append(f'    if ({src}{m.name}) {{ auto* t = c.alloc<{m.type}>({n}); for (uint32_t i = 0; i < (uint32_t)({n}); ++i) t[i] = c.xl({src}{m.name}[i]); {dst}{m.name} = t; }}')
        elif ct in structs and nt.get(ct):
            if m.ptr == 0 and m.array:
                lines.append(f'    for (uint32_t i = 0; i < (uint32_t)({m.array}); ++i) tr_{ct}(c, &{src}{m.name}[i], &{dst}{m.name}[i]);')
            elif m.ptr == 0:
                lines.append(f'    tr_{ct}(c, &{src}{m.name}, &{dst}{m.name});')
            else:
                n = f'{src}{m.len}' if m.len else '1'
                lines.append(f'    if ({src}{m.name}) {{ auto* t = c.alloc<{ct}>({n}); for (uint32_t i = 0; i < (uint32_t)({n}); ++i) tr_{ct}(c, &{src}{m.name}[i], &t[i]); {dst}{m.name} = t; }}')
        return lines

    for s in gen_structs:
        if s in MANUAL_STRUCTS:
            continue
        w(f'static void tr_{s}(Ctx& c, const {s}* in, {s}* out) {{')
        w('    *out = *in;')
        for m in structs[s]:
            # B must build what A built: A may have hit its pipeline cache where B would
            # answer VK_PIPELINE_COMPILE_REQUIRED and leave a null twin.
            if m.name == 'flags' and canon(m.type) in ('VkPipelineCreateFlags', 'VkPipelineCreateFlags2'):
                w('    out->flags &= ~(decltype(out->flags))(VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT | VK_PIPELINE_CREATE_EARLY_RETURN_ON_FAILURE_BIT);')
        for m in structs[s]:
            ct = canon(m.type)
            if m.name == 'pNext' and nt.get(s):
                w('    out->pNext = (decltype(out->pNext))tr_chain(c, in->pNext);')
                continue
            needs = ct == 'VkImageLayout' or xl_handle(ct) or (ct in structs and nt.get(ct))
            if not needs:
                continue
            for l in emit_member_fix(s, m, 'out->', 'in->'):
                w(l)
        w('}')
    w('')

    # pNext chain translation: copy + translate structs that may carry handles,
    # keep the rest of the chain by reference when nothing below needs it.
    w('static const void* tr_chain(Ctx& c, const void* p) {')
    w('    if (!p) return nullptr;')
    w('    auto* b = (const VkBaseInStructure*)p;')
    w('    switch (b->sType) {')
    # Win32 imports retain caller ownership of HANDLE/name. The same external
    # payload can be imported more than once; keep its external identity while
    # translating the rest of the allocation chain (e.g. dedicated image).
    # Platform commands remain excluded from automatic generation.
    w('#ifdef VK_USE_PLATFORM_WIN32_KHR')
    for s in ('VkImportMemoryWin32HandleInfoKHR', 'VkImportMemoryWin32HandleInfoNV'):
        st = stype_of[s]
        w(f'    case {st}: {{ auto* o = c.alloc<{s}>(1); *o = *({s}*)p; o->pNext = tr_chain(c, b->pNext); return o; }}')
    w('#endif')
    seen_st = set()
    for s in sorted(structs):
        if s not in stype_of or not usable(s) or s in returnedonly:
            continue
        st = stype_of[s]
        if st in seen_st:
            continue
        seen_st.add(st)
        if nt.get(s) and s not in bad_reason:
            w(f'    case {st}: {{ auto* o = c.alloc<{s}>(1); tr_{s}(c, ({s}*)p, o); return o; }}')
        elif nt.get(s):
            w(f'    case {st}: c.fail("pNext {s}: {bad_reason[s]}"); return p;')
        else:
            w(f'    case {st}: {{ auto* o = c.alloc<{s}>(1); *o = *({s}*)p; o->pNext = (decltype(o->pNext))tr_chain(c, b->pNext); return o; }}')
    w('    default: c.unknown_stype(b->sType); return nullptr;')
    w('    }')
    w('}')
    w('')

    # wrappers
    table = []
    stubs = []
    for name in sorted(device_cmds):
        ret, params, names = device_cmds[name]
        k = classify(name, ret, params)
        if k in ('manual', 'aonly'):
            continue
        sig = ', '.join(text_of_param(p) for p in params)
        w(f'static VKAPI_ATTR {ret} VKAPI_CALL m_{name[2:]}({sig}) {{')
        args = ', '.join(p.name for p in params)
        w(f'    if (g_trace) trace_call("{name}");')
        w(f'    MDev* md = mdev_of(key({params[0].name}));')
        w(f'    if (!md) {{ auto state = FindDevice(reinterpret_cast<VkDevice>({params[0].name})); auto fn = reinterpret_cast<PFN_{name}>(state.next(state.handle, "{name}")); return fn({args}); }}')
        if name == 'vkDestroyBuffer':
            w('    if (md) { std::lock_guard<std::mutex> l(md->addressLock); md->addresses.erase(h2u(buffer)); md->bufferSizes.erase(h2u(buffer)); }')
        if ret == 'VkResult':
            w(f'    VkResult r = md->a.{name[2:]}({args});')
            w('    if (r < 0 || !md->live) return r;')
            w('    if (!md->b.' + name[2:] + ') { mirror_fail(md, "missing replay entry"); return r; }')
        else:
            w(f'    md->a.{name[2:]}({args});')
            w('    if (!md->live) return;')
            w('    if (!md->b.' + name[2:] + ') { mirror_fail(md, "missing replay entry"); return; }')
        if k.startswith('stub:'):
            reason = k[5:].replace('"', "'")
            stubs.append((name, reason))
            w(f'    mirror_fail(md, "{name}: {reason}");')
            w('    return' + (' r;' if ret == 'VkResult' else ';'))
            w('}')
            table.append(name)
            continue
        w('    Ctx& c = ctx_begin(md);')
        bargs, post = [], []
        for i, p in enumerate(params):
            ct = canon(p.type)
            if i == 0:
                bargs.append('md->bdev' if ct == 'VkDevice' else f'c.xl({p.name})')
                continue
            if p.name == 'pAllocator':
                bargs.append('nullptr')
                continue
            if p.ptr == 1 and not p.const and xl_handle(ct):
                n = p.len if p.len else '1'
                w(f'    auto* b_{p.name} = c.alloc<{p.type}>({n});')
                bargs.append(f'b_{p.name}')
                post.append(f'    for (uint32_t i = 0; i < (uint32_t)({n}); ++i) c.record({p.name}[i], b_{p.name}[i]);')
                continue
            if p.ptr >= 1 and not p.const:
                if ct == 'void':
                    n = p.len if p.len else '0'
                    w(f'    void* b_{p.name} = {p.name} ? c.alloc<char>({n}) : nullptr;')
                elif ct in structs:
                    n = p.len if p.len else '1'
                    w(f'    auto* b_{p.name} = {p.name} ? c.alloc<{p.type}>({n}) : nullptr;')
                    if p.ptr == 1:
                        w(f'    if ({p.name}) for (uint32_t i = 0; i < (uint32_t)({n}); ++i) {{ b_{p.name}[i] = {p.name}[i]; }}')
                else:
                    n = p.len if p.len else '1'
                    w(f'    auto* b_{p.name} = {p.name} ? c.alloc<{p.type}>({n}) : nullptr;')
                bargs.append(f'b_{p.name}')
                continue
            if xl_handle(ct):
                if p.ptr == 0:
                    bargs.append(f'c.xl({p.name})')
                else:
                    n = p.len if p.len else '1'
                    w(f'    {p.type}* b_{p.name} = nullptr;')
                    w(f'    if ({p.name}) {{ b_{p.name} = c.alloc<{p.type}>({n}); for (uint32_t i = 0; i < (uint32_t)({n}); ++i) b_{p.name}[i] = c.xl({p.name}[i]); }}')
                    bargs.append(f'b_{p.name}')
                continue
            if ct in structs and nt.get(ct):
                if p.ptr == 0:
                    w(f'    {ct} b_{p.name}; tr_{ct}(c, &{p.name}, &b_{p.name});')
                    bargs.append(f'b_{p.name}')
                else:
                    n = p.len if p.len else '1'
                    w(f'    {ct}* b_{p.name} = nullptr;')
                    w(f'    if ({p.name}) {{ b_{p.name} = c.alloc<{ct}>({n}); for (uint32_t i = 0; i < (uint32_t)({n}); ++i) tr_{ct}(c, &{p.name}[i], &b_{p.name}[i]); }}')
                    bargs.append(f'b_{p.name}')
                continue
            bargs.append(f'eye_layout({p.name})' if ct == 'VkImageLayout' else p.name)
        # Resolve scalar handles before checking c.ok(). Argument evaluation
        # inside the call can otherwise stop mirroring and still call the driver
        # with a null translated handle.
        for i, argument in enumerate(bargs):
            if argument.startswith('c.xl('):
                w(f'    const auto resolved_{i} = {argument};')
                bargs[i] = f'resolved_{i}'
        if ret == 'VkResult':
            w(f'    VkResult rb = c.ok() ? md->b.{name[2:]}({", ".join(bargs)}) : VK_ERROR_UNKNOWN;')
            w(f'    if (rb < 0) mirror_fail(md, "{name} failed on B");')
            w('    else {')
            for l in post:
                w('    ' + l)
            w('    }')
            w('    ctx_end(c);')
            w('    return r;')
        else:
            w(f'    if (c.ok()) md->b.{name[2:]}({", ".join(bargs)});')
            w(f'    else mirror_fail(md, "{name}: translation failed");')
            for l in post:
                w(l)
            w('    ctx_end(c);')
        w('}')
        table.append(name)
    w('')
    w('// Alias -> canonical name for hand-written commands (the layer matches canonical names).')
    w('static const char* manual_canonical(const char* name) {')
    for name in sorted(device_cmds):
        ret, params, names = device_cmds[name]
        if classify(name, ret, params) != 'manual':
            continue
        canon_name = next((n for n in names if n in MANUAL), name)
        for n in names:
            w(f'    if (!strcmp(name, "{n}")) return "{canon_name}";')
    w('    return nullptr;')
    w('}')
    w('')
    w('static PFN_vkVoidFunction gen_lookup(const char* name) {')
    for name in table:
        for n in device_cmds[name][2]:
            w(f'    if (!strcmp(name, "{n}")) return (PFN_vkVoidFunction)m_{name[2:]};')
    w('    return nullptr;')
    w('}')
    w('')
    w('// Generated wrappers that only stop the mirror (not translatable):')
    for n, r in stubs:
        w(f'//   {n}: {r}')
    with open(out_path, 'w') as f:
        f.write('\n'.join(out) + '\n')
    print(f'{len(device_cmds)} device commands, {len(table)} wrapped ({len(stubs)} stubs), '
          f'{len(gen_structs)} struct translators, untranslatable structs: {len(bad_reason)}', file=sys.stderr)
    if '-v' in sys.argv:
        for s, r in sorted(bad_reason.items()):
            print(f'  struct {s}: {r}', file=sys.stderr)
        for n, r in stubs:
            print(f'  stub {n}: {r}', file=sys.stderr)


def text_of_param(p):
    return p._decl


_orig_init = Member.__init__


def _init(self, e):
    _orig_init(self, e)
    s = ' '.join(text_of(e).split())
    s = s.replace(' *', '*').replace('* ', '* ')
    self._decl = s


Member.__init__ = _init

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], sys.argv[3])
