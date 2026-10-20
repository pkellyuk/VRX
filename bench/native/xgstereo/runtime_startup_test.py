"""Check runtime isolation and Vulkan startup with an optional old host runtime.

The layer must not import MSVC runtime DLLs from the game's DLL search path.
Supplying --legacy-runtime reproduces System Shock's app-local runtime setup
without launching the game or requiring its content in the normal test suite.
"""
import argparse
import ctypes as c
import os
from pathlib import Path
import struct


def imports(path):
    data = path.read_bytes()
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    sections, optional_size = struct.unpack_from('<H12xH', data, pe + 6)
    optional = pe + 24
    assert struct.unpack_from('<H', data, optional)[0] == 0x20B, 'x64 layer required'
    headers = optional + optional_size

    def offset(rva):
        for i in range(sections):
            virtual_size, address, raw_size, raw = struct.unpack_from('<IIII', data, headers + i * 40 + 8)
            if address <= rva < address + max(virtual_size, raw_size):
                return raw + rva - address
        raise AssertionError(f'Unmapped PE RVA {rva:x}')

    table_rva = struct.unpack_from('<I', data, optional + 112 + 8)[0]
    at = offset(table_rva)
    result = []
    while any(data[at:at + 20]):
        name_at = offset(struct.unpack_from('<I', data, at + 12)[0])
        result.append(data[name_at:data.index(0, name_at)].decode('ascii').lower())
        at += 20
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--layer-directory', required=True, type=Path)
    parser.add_argument('--legacy-runtime', type=Path)
    args = parser.parse_args()
    layer = args.layer_directory.resolve()
    dependencies = imports(layer / 'vrx_xgstereo.dll')
    assert not any(name.startswith(('msvcp', 'vcruntime', 'concrt', 'ucrtbase', 'api-ms-win-crt')) for name in dependencies), dependencies
    if args.legacy_runtime:
        # Preload before Vulkan can bring in any overlay/driver libraries.
        legacy = c.WinDLL(str(args.legacy_runtime.resolve()))
        assert legacy._handle
    os.environ['VK_ADD_LAYER_PATH'] = str(layer)
    os.environ['VK_INSTANCE_LAYERS'] = 'VK_LAYER_VRX_xgstereo'
    os.environ['VRX_XG_EYE'] = 'alternate'

    class ApplicationInfo(c.Structure):
        _fields_ = [('sType', c.c_uint32), ('pNext', c.c_void_p), ('name', c.c_char_p),
                    ('version', c.c_uint32), ('engine', c.c_char_p), ('engineVersion', c.c_uint32), ('apiVersion', c.c_uint32)]

    class InstanceInfo(c.Structure):
        _fields_ = [('sType', c.c_uint32), ('pNext', c.c_void_p), ('flags', c.c_uint32),
                    ('application', c.POINTER(ApplicationInfo)), ('layerCount', c.c_uint32), ('layers', c.c_void_p),
                    ('extensionCount', c.c_uint32), ('extensions', c.c_void_p)]

    app = ApplicationInfo(sType=0, name=b'VRX runtime compatibility check', apiVersion=(1 << 22) | (3 << 12))
    info = InstanceInfo(sType=1, application=c.pointer(app))
    vk = c.WinDLL('vulkan-1.dll')
    vk.vkCreateInstance.argtypes = [c.POINTER(InstanceInfo), c.c_void_p, c.POINTER(c.c_void_p)]
    vk.vkCreateInstance.restype = c.c_int32
    vk.vkDestroyInstance.argtypes = [c.c_void_p, c.c_void_p]
    instance = c.c_void_p()
    result = vk.vkCreateInstance(c.byref(info), None, c.byref(instance))
    assert result == 0 and instance.value, f'vkCreateInstance: {result}'
    vk.vkDestroyInstance(instance, None)
    print('PASS: isolated C++ runtime and Vulkan instance mutex startup' + (' with preloaded legacy MSVC runtime' if args.legacy_runtime else ''))


if __name__ == '__main__':
    main()
