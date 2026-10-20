using System.Buffers.Binary;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Text;

namespace VRX.Desktop;

// status.h v1 + Counts ABI v4. Parse explicit byte offsets, never CLR packing.
internal sealed record NativeStereoStatus(int Pid, bool Mirroring, ulong AgeMs,
    ulong HookFailures, ulong Replays, ulong Stops, ulong Publications, string Reason)
{
    internal const int Size = 504;
    public bool Ready => Mirroring && AgeMs <= 2000 && HookFailures == 0 && Stops == 0 && Replays > 0 && Publications > 0;

    internal static NativeStereoStatus? Parse(ReadOnlySpan<byte> bytes, int expectedPid, long now)
    {
        if (bytes.Length != Size || expectedPid <= 0 || now < 0) return null;
        static uint U(ReadOnlySpan<byte> b, int at) => BinaryPrimitives.ReadUInt32LittleEndian(b.Slice(at,4));
        static ulong Q(ReadOnlySpan<byte> b, int at) => BinaryPrimitives.ReadUInt64LittleEndian(b.Slice(at,8));
        if (U(bytes,0)!=0x53445856 || U(bytes,4)!=1 || U(bytes,8)!=Size || U(bytes,12)!=(uint)expectedPid ||
            U(bytes,24)>1 || U(bytes,28)!=0 || U(bytes,32)!=472 || U(bytes,36)!=4) return null;
        ulong updated=Q(bytes,16); if(updated==0 || updated>(ulong)now) return null;
        var reason=bytes[248..]; int end=reason.IndexOf((byte)0); if(end<0) return null;
        return new(expectedPid,U(bytes,24)==1,(ulong)now-updated,Q(bytes,128),Q(bytes,136),Q(bytes,144),Q(bytes,192),Encoding.UTF8.GetString(reason[..end]));
    }

    internal static NativeStereoStatus? Read(string session, int pid)
    {
        if (!Profile.ValidGeometrySession(session) || pid<=0) return null;
        try {
            using var mutex=Mutex.OpenExisting($@"Local\VRX.dx12.{session}.lock");
            bool held; try {held=mutex.WaitOne(0);} catch(AbandonedMutexException){mutex.ReleaseMutex();return null;}
            if(!held) return null;
            byte[] bytes=new byte[Size];
            try {
                using var map=MemoryMappedFile.OpenExisting($@"Local\VRX.dx12.{session}.status",MemoryMappedFileRights.Read);
                using var view=map.CreateViewAccessor(0,Size,MemoryMappedFileAccess.Read);view.ReadArray(0,bytes,0,bytes.Length);
            } finally {mutex.ReleaseMutex();}
            return Parse(bytes,pid,Environment.TickCount64);
        } catch(Exception ex) when(ex is IOException or UnauthorizedAccessException or WaitHandleCannotBeOpenedException or ArgumentException) {return null;}
    }
}
