param([Parameter(Mandatory=$true)][int]$ProcessId,[Parameter(Mandatory=$true)][string]$Dll,[Parameter(Mandatory=$true)][string]$Output)
$ErrorActionPreference='Stop'
if (-not ('Arc2DumpClient' -as [type])) {
Add-Type -TypeDefinition @"
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
public static class Arc2DumpClient {
 [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr OpenProcess(uint rights,bool inherit,int pid);
 [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr VirtualAllocEx(IntPtr p,IntPtr a,UIntPtr size,uint type,uint protection);
 [DllImport("kernel32.dll",SetLastError=true)] static extern bool VirtualFreeEx(IntPtr p,IntPtr a,UIntPtr size,uint type);
 [DllImport("kernel32.dll",SetLastError=true)] static extern bool WriteProcessMemory(IntPtr p,IntPtr a,byte[] b,UIntPtr size,out UIntPtr written);
 [DllImport("kernel32.dll",SetLastError=true)] static extern IntPtr CreateRemoteThread(IntPtr p,IntPtr attrs,UIntPtr stack,IntPtr entry,IntPtr arg,uint flags,out uint tid);
 [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr h,uint timeout);
 [DllImport("kernel32.dll")] static extern bool GetExitCodeThread(IntPtr h,out uint code);
 [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] static extern IntPtr LoadLibraryEx(string path,IntPtr file,uint flags);
 [DllImport("kernel32.dll",CharSet=CharSet.Ansi)] static extern IntPtr GetProcAddress(IntPtr m,string name);
 [DllImport("kernel32.dll")] static extern bool FreeLibrary(IntPtr m);
 public static uint Dump(int pid,string dll,string output) {
  IntPtr remote=IntPtr.Zero;
  foreach(ProcessModule m in Process.GetProcessById(pid).Modules) if(String.Equals(System.IO.Path.GetFullPath(m.FileName),System.IO.Path.GetFullPath(dll),StringComparison.OrdinalIgnoreCase)){remote=m.BaseAddress;break;}
  if(remote==IntPtr.Zero)throw new Exception("Requested frontend module is not loaded in this owned test process");
  IntPtr local=LoadLibraryEx(dll,IntPtr.Zero,1);if(local==IntPtr.Zero)throw new Exception("Map frontend exports failed");
  long rva;try{IntPtr f=GetProcAddress(local,"Arc2Dump");if(f==IntPtr.Zero)throw new Exception("Arc2Dump export missing");rva=f.ToInt64()-local.ToInt64();}finally{FreeLibrary(local);}
  IntPtr process=OpenProcess(0x043a,false,pid);if(process==IntPtr.Zero)throw new Exception("OpenProcess failed "+Marshal.GetLastWin32Error());
  IntPtr memory=IntPtr.Zero,thread=IntPtr.Zero;bool finished=true;
  try{byte[] bytes=Encoding.Unicode.GetBytes(output+"\0");memory=VirtualAllocEx(process,IntPtr.Zero,(UIntPtr)bytes.Length,0x3000,4);if(memory==IntPtr.Zero)throw new Exception("Allocate diagnostic argument failed");UIntPtr written;if(!WriteProcessMemory(process,memory,bytes,(UIntPtr)bytes.Length,out written)||written.ToUInt64()!=(ulong)bytes.Length)throw new Exception("Write diagnostic argument failed");uint tid;thread=CreateRemoteThread(process,IntPtr.Zero,UIntPtr.Zero,new IntPtr(remote.ToInt64()+rva),memory,0,out tid);if(thread==IntPtr.Zero)throw new Exception("Create diagnostic thread failed");finished=false;if(WaitForSingleObject(thread,30000)!=0)throw new Exception("Diagnostic dump timed out");finished=true;uint code;if(!GetExitCodeThread(thread,out code))throw new Exception("Diagnostic thread status unavailable");return code;}
  finally{if(thread!=IntPtr.Zero)CloseHandle(thread);if(memory!=IntPtr.Zero&&finished)VirtualFreeEx(process,memory,UIntPtr.Zero,0x8000);CloseHandle(process);}
 }
}
"@
}
$code=[Arc2DumpClient]::Dump($ProcessId,(Resolve-Path -LiteralPath $Dll).Path,[IO.Path]::GetFullPath($Output))
if($code -ne 0){throw "Arc2Dump failed with code $code"}
