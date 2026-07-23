using System;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.Win32.SafeHandles;

namespace BlindEval
{
    public sealed class NativeProcessResult
    {
        public bool Started { get; set; }
        public bool TimedOut { get; set; }
        public bool OutputLimitExceeded { get; set; }
        public bool StreamReadFailed { get; set; }
        public int ExitCode { get; set; }
        public string StandardOutput { get; set; }
        public string StandardError { get; set; }
    }

    public static class NativeProcessRunner
    {
        private const uint StartfUseStdHandles = 0x00000100;
        private const uint CreateSuspended = 0x00000004;
        private const uint CreateNoWindow = 0x08000000;
        private const uint ExtendedStartupInfoPresent = 0x00080000;
        private const uint HandleFlagInherit = 0x00000001;
        private const uint JobObjectLimitKillOnJobClose = 0x00002000;
        private const int JobObjectExtendedLimitInformationClass = 9;
        private const long ProcThreadAttributeHandleList = 0x00020002;
        private const uint WaitObject0 = 0x00000000;
        private const uint WaitTimeout = 0x00000102;
        private const uint WaitStreamFailure = 0xFFFFFFFE;
        private const uint GenericRead = 0x80000000;
        private const uint FileShareRead = 0x00000001;
        private const uint FileShareWrite = 0x00000002;
        private const uint OpenExisting = 3;
        private const uint FileAttributeNormal = 0x00000080;
        private static readonly IntPtr InvalidHandleValue = new IntPtr(-1);

        [StructLayout(LayoutKind.Sequential)]
        private struct SecurityAttributes
        {
            public int Length;
            public IntPtr SecurityDescriptor;
            public int InheritHandle;
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct StartupInfo
        {
            public int Size;
            public string Reserved;
            public string Desktop;
            public string Title;
            public int X;
            public int Y;
            public int XSize;
            public int YSize;
            public int XCountChars;
            public int YCountChars;
            public int FillAttribute;
            public uint Flags;
            public short ShowWindow;
            public short ReservedByteCount;
            public IntPtr ReservedBytes;
            public IntPtr StandardInput;
            public IntPtr StandardOutput;
            public IntPtr StandardError;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct StartupInfoEx
        {
            public StartupInfo StartupInfo;
            public IntPtr AttributeList;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct ProcessInformation
        {
            public IntPtr Process;
            public IntPtr Thread;
            public uint ProcessId;
            public uint ThreadId;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct JobObjectBasicLimitInformation
        {
            public long PerProcessUserTimeLimit;
            public long PerJobUserTimeLimit;
            public uint LimitFlags;
            public UIntPtr MinimumWorkingSetSize;
            public UIntPtr MaximumWorkingSetSize;
            public uint ActiveProcessLimit;
            public UIntPtr Affinity;
            public uint PriorityClass;
            public uint SchedulingClass;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct IoCounters
        {
            public ulong ReadOperationCount;
            public ulong WriteOperationCount;
            public ulong OtherOperationCount;
            public ulong ReadTransferCount;
            public ulong WriteTransferCount;
            public ulong OtherTransferCount;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct JobObjectExtendedLimitInformation
        {
            public JobObjectBasicLimitInformation BasicLimitInformation;
            public IoCounters IoInfo;
            public UIntPtr ProcessMemoryLimit;
            public UIntPtr JobMemoryLimit;
            public UIntPtr PeakProcessMemoryUsed;
            public UIntPtr PeakJobMemoryUsed;
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool CreatePipe(
            out IntPtr readPipe,
            out IntPtr writePipe,
            ref SecurityAttributes pipeAttributes,
            uint size);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool SetHandleInformation(
            IntPtr handle,
            uint mask,
            uint flags);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr CreateFile(
            string fileName,
            uint desiredAccess,
            uint shareMode,
            ref SecurityAttributes securityAttributes,
            uint creationDisposition,
            uint flagsAndAttributes,
            IntPtr templateFile);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool CreateProcess(
            string applicationName,
            StringBuilder commandLine,
            IntPtr processAttributes,
            IntPtr threadAttributes,
            bool inheritHandles,
            uint creationFlags,
            IntPtr environment,
            string currentDirectory,
            ref StartupInfoEx startupInfo,
            out ProcessInformation processInformation);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool InitializeProcThreadAttributeList(
            IntPtr attributeList,
            int attributeCount,
            int flags,
            ref IntPtr size);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool UpdateProcThreadAttribute(
            IntPtr attributeList,
            uint flags,
            IntPtr attribute,
            IntPtr value,
            IntPtr size,
            IntPtr previousValue,
            IntPtr returnSize);

        [DllImport("kernel32.dll")]
        private static extern void DeleteProcThreadAttributeList(
            IntPtr attributeList);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr CreateJobObject(
            IntPtr jobAttributes,
            string name);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool SetInformationJobObject(
            IntPtr job,
            int informationClass,
            IntPtr information,
            uint informationLength);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool AssignProcessToJobObject(
            IntPtr job,
            IntPtr process);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool TerminateJobObject(
            IntPtr job,
            uint exitCode);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool TerminateProcess(
            IntPtr process,
            uint exitCode);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern uint ResumeThread(IntPtr thread);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern uint WaitForSingleObject(
            IntPtr handle,
            uint milliseconds);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool GetExitCodeProcess(
            IntPtr process,
            out uint exitCode);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool CloseHandle(IntPtr handle);

        public static NativeProcessResult Run(
            string application,
            string arguments,
            int timeoutMilliseconds,
            int maximumOutputCharacters)
        {
            NativeProcessResult result = new NativeProcessResult
            {
                ExitCode = -1,
                StandardOutput = string.Empty,
                StandardError = string.Empty
            };
            if (Environment.OSVersion.Platform != PlatformID.Win32NT)
            {
                return result;
            }
            if (string.IsNullOrWhiteSpace(application) ||
                timeoutMilliseconds <= 0 ||
                maximumOutputCharacters <= 0)
            {
                return result;
            }

            IntPtr job = IntPtr.Zero;
            IntPtr standardInput = IntPtr.Zero;
            IntPtr standardOutputRead = IntPtr.Zero;
            IntPtr standardOutputWrite = IntPtr.Zero;
            IntPtr standardErrorRead = IntPtr.Zero;
            IntPtr standardErrorWrite = IntPtr.Zero;
            IntPtr attributeList = IntPtr.Zero;
            IntPtr inheritedHandleList = IntPtr.Zero;
            ProcessInformation process = new ProcessInformation();

            try
            {
                job = CreateKillOnCloseJob();
                if (job == IntPtr.Zero)
                {
                    return result;
                }

                SecurityAttributes attributes = new SecurityAttributes
                {
                    Length = Marshal.SizeOf(typeof(SecurityAttributes)),
                    SecurityDescriptor = IntPtr.Zero,
                    InheritHandle = 1
                };
                if (!CreatePipe(
                        out standardOutputRead,
                        out standardOutputWrite,
                        ref attributes,
                        0) ||
                    !SetHandleInformation(
                        standardOutputRead,
                        HandleFlagInherit,
                        0) ||
                    !CreatePipe(
                        out standardErrorRead,
                        out standardErrorWrite,
                        ref attributes,
                        0) ||
                    !SetHandleInformation(
                        standardErrorRead,
                        HandleFlagInherit,
                        0))
                {
                    return result;
                }

                standardInput = CreateFile(
                    "NUL",
                    GenericRead,
                    FileShareRead | FileShareWrite,
                    ref attributes,
                    OpenExisting,
                    FileAttributeNormal,
                    IntPtr.Zero);
                if (standardInput == InvalidHandleValue)
                {
                    standardInput = IntPtr.Zero;
                    return result;
                }

                if (!CreateInheritedHandleList(
                        new IntPtr[]
                        {
                            standardInput,
                            standardOutputWrite,
                            standardErrorWrite
                        },
                        out attributeList,
                        out inheritedHandleList))
                {
                    return result;
                }

                StartupInfoEx startup = new StartupInfoEx
                {
                    StartupInfo = new StartupInfo
                    {
                        Size = Marshal.SizeOf(typeof(StartupInfoEx)),
                        Flags = StartfUseStdHandles,
                        StandardInput = standardInput,
                        StandardOutput = standardOutputWrite,
                        StandardError = standardErrorWrite
                    },
                    AttributeList = attributeList
                };
                StringBuilder commandLine = new StringBuilder();
                commandLine.Append(QuoteArgument(application));
                if (!string.IsNullOrEmpty(arguments))
                {
                    commandLine.Append(' ');
                    commandLine.Append(arguments);
                }

                if (!CreateProcess(
                        application,
                        commandLine,
                        IntPtr.Zero,
                        IntPtr.Zero,
                        true,
                        CreateSuspended |
                            CreateNoWindow |
                            ExtendedStartupInfoPresent,
                        IntPtr.Zero,
                        Environment.CurrentDirectory,
                        ref startup,
                        out process))
                {
                    return result;
                }
                result.Started = true;

                CloseHandle(standardOutputWrite);
                standardOutputWrite = IntPtr.Zero;
                CloseHandle(standardErrorWrite);
                standardErrorWrite = IntPtr.Zero;
                CloseHandle(standardInput);
                standardInput = IntPtr.Zero;

                if (!AssignProcessToJobObject(job, process.Process))
                {
                    TerminateProcess(process.Process, 1);
                    WaitForSingleObject(process.Process, 2000);
                    return result;
                }
                if (ResumeThread(process.Thread) == uint.MaxValue)
                {
                    TerminateJobObject(job, 1);
                    TerminateProcess(process.Process, 1);
                    WaitForSingleObject(process.Process, 2000);
                    return result;
                }

                Stopwatch timer = Stopwatch.StartNew();
                SafeFileHandle outputHandle =
                    new SafeFileHandle(standardOutputRead, true);
                standardOutputRead = IntPtr.Zero;
                SafeFileHandle errorHandle =
                    new SafeFileHandle(standardErrorRead, true);
                standardErrorRead = IntPtr.Zero;
                using (outputHandle)
                using (errorHandle)
                using (FileStream outputStream = new FileStream(
                    outputHandle, FileAccess.Read, 4096, false))
                using (FileStream errorStream = new FileStream(
                    errorHandle, FileAccess.Read, 4096, false))
                using (StreamReader outputReader = new StreamReader(
                    outputStream, Encoding.UTF8, true, 4096, false))
                using (StreamReader errorReader = new StreamReader(
                    errorStream, Encoding.UTF8, true, 4096, false))
                {
                    Task<string> outputTask = Task.Factory.StartNew(
                        delegate
                        {
                            return ReadLimited(
                                outputReader,
                                maximumOutputCharacters);
                        },
                        CancellationToken.None,
                        TaskCreationOptions.LongRunning,
                        TaskScheduler.Default);
                    Task<string> errorTask = Task.Factory.StartNew(
                        delegate
                        {
                            return ReadLimited(
                                errorReader,
                                maximumOutputCharacters);
                        },
                        CancellationToken.None,
                        TaskCreationOptions.LongRunning,
                        TaskScheduler.Default);

                    uint waitResult = WaitForProcessOrStreamFailure(
                        process.Process,
                        outputTask,
                        errorTask,
                        timer,
                        timeoutMilliseconds,
                        result);
                    int remaining = Math.Max(
                        0,
                        timeoutMilliseconds - (int)timer.ElapsedMilliseconds);
                    bool streamsCompleted = false;

                    if (waitResult == WaitObject0)
                    {
                        streamsCompleted = WaitForStreams(
                            outputTask,
                            errorTask,
                            remaining,
                            result);
                    }

                    if (waitResult == WaitTimeout ||
                        waitResult == WaitStreamFailure ||
                        !streamsCompleted)
                    {
                        result.TimedOut = waitResult == WaitTimeout ||
                            (!streamsCompleted &&
                             !result.OutputLimitExceeded &&
                             !result.StreamReadFailed);
                        TerminateJobObject(job, 1);
                        WaitForSingleObject(process.Process, 2000);
                        WaitForStreams(
                            outputTask,
                            errorTask,
                            2000,
                            result);
                    }

                    if (outputTask.Status == TaskStatus.RanToCompletion)
                    {
                        result.StandardOutput = outputTask.Result;
                    }
                    if (errorTask.Status == TaskStatus.RanToCompletion)
                    {
                        result.StandardError = errorTask.Result;
                    }
                }

                uint processExitCode;
                if (GetExitCodeProcess(process.Process, out processExitCode))
                {
                    result.ExitCode = unchecked((int)processExitCode);
                }
                return result;
            }
            finally
            {
                if (job != IntPtr.Zero)
                {
                    CloseHandle(job);
                }
                if (attributeList != IntPtr.Zero)
                {
                    DeleteProcThreadAttributeList(attributeList);
                    Marshal.FreeHGlobal(attributeList);
                }
                if (inheritedHandleList != IntPtr.Zero)
                {
                    Marshal.FreeHGlobal(inheritedHandleList);
                }
                if (process.Thread != IntPtr.Zero)
                {
                    CloseHandle(process.Thread);
                }
                if (process.Process != IntPtr.Zero)
                {
                    CloseHandle(process.Process);
                }
                CloseIfValid(standardInput);
                CloseIfValid(standardOutputRead);
                CloseIfValid(standardOutputWrite);
                CloseIfValid(standardErrorRead);
                CloseIfValid(standardErrorWrite);
            }
        }

        private static IntPtr CreateKillOnCloseJob()
        {
            IntPtr job = CreateJobObject(IntPtr.Zero, null);
            if (job == IntPtr.Zero)
            {
                return IntPtr.Zero;
            }

            JobObjectExtendedLimitInformation information =
                new JobObjectExtendedLimitInformation();
            information.BasicLimitInformation.LimitFlags =
                JobObjectLimitKillOnJobClose;
            int size = Marshal.SizeOf(
                typeof(JobObjectExtendedLimitInformation));
            IntPtr buffer = Marshal.AllocHGlobal(size);
            try
            {
                Marshal.StructureToPtr(information, buffer, false);
                if (!SetInformationJobObject(
                        job,
                        JobObjectExtendedLimitInformationClass,
                        buffer,
                        (uint)size))
                {
                    CloseHandle(job);
                    return IntPtr.Zero;
                }
            }
            finally
            {
                Marshal.FreeHGlobal(buffer);
            }
            return job;
        }

        private static bool CreateInheritedHandleList(
            IntPtr[] handles,
            out IntPtr attributeList,
            out IntPtr inheritedHandleList)
        {
            attributeList = IntPtr.Zero;
            inheritedHandleList = IntPtr.Zero;
            IntPtr attributeListSize = IntPtr.Zero;
            InitializeProcThreadAttributeList(
                IntPtr.Zero, 1, 0, ref attributeListSize);
            if (attributeListSize == IntPtr.Zero)
            {
                return false;
            }

            attributeList = Marshal.AllocHGlobal(attributeListSize);
            if (!InitializeProcThreadAttributeList(
                    attributeList,
                    1,
                    0,
                    ref attributeListSize))
            {
                Marshal.FreeHGlobal(attributeList);
                attributeList = IntPtr.Zero;
                return false;
            }

            int handleListSize = IntPtr.Size * handles.Length;
            inheritedHandleList = Marshal.AllocHGlobal(handleListSize);
            Marshal.Copy(
                handles, 0, inheritedHandleList, handles.Length);
            if (!UpdateProcThreadAttribute(
                    attributeList,
                    0,
                    new IntPtr(ProcThreadAttributeHandleList),
                    inheritedHandleList,
                    new IntPtr(handleListSize),
                    IntPtr.Zero,
                    IntPtr.Zero))
            {
                DeleteProcThreadAttributeList(attributeList);
                Marshal.FreeHGlobal(attributeList);
                Marshal.FreeHGlobal(inheritedHandleList);
                attributeList = IntPtr.Zero;
                inheritedHandleList = IntPtr.Zero;
                return false;
            }
            return true;
        }

        private static bool WaitForStreams(
            Task<string> outputTask,
            Task<string> errorTask,
            int timeoutMilliseconds,
            NativeProcessResult result)
        {
            try
            {
                return Task.WaitAll(
                    new Task[] { outputTask, errorTask },
                    timeoutMilliseconds);
            }
            catch (AggregateException exception)
            {
                RecordException(exception, result);
                return false;
            }
        }

        private static uint WaitForProcessOrStreamFailure(
            IntPtr process,
            Task<string> outputTask,
            Task<string> errorTask,
            Stopwatch timer,
            int timeoutMilliseconds,
            NativeProcessResult result)
        {
            while (true)
            {
                if (outputTask.IsFaulted)
                {
                    RecordException(outputTask.Exception, result);
                    return WaitStreamFailure;
                }
                if (errorTask.IsFaulted)
                {
                    RecordException(errorTask.Exception, result);
                    return WaitStreamFailure;
                }
                if (outputTask.IsCanceled || errorTask.IsCanceled)
                {
                    result.StreamReadFailed = true;
                    return WaitStreamFailure;
                }

                int remaining = timeoutMilliseconds -
                    (int)timer.ElapsedMilliseconds;
                if (remaining <= 0)
                {
                    return WaitTimeout;
                }
                uint slice = (uint)Math.Min(remaining, 25);
                uint waitResult = WaitForSingleObject(process, slice);
                if (waitResult == WaitObject0)
                {
                    return waitResult;
                }
                if (waitResult != WaitTimeout)
                {
                    result.StreamReadFailed = true;
                    return WaitStreamFailure;
                }
            }
        }

        private static void RecordException(
            AggregateException exception,
            NativeProcessResult result)
        {
            if (exception == null)
            {
                result.StreamReadFailed = true;
                return;
            }
            AggregateException flattened = exception.Flatten();
            foreach (Exception inner in flattened.InnerExceptions)
            {
                if (inner is InvalidDataException)
                {
                    result.OutputLimitExceeded = true;
                }
                else
                {
                    result.StreamReadFailed = true;
                }
            }
        }

        private static string ReadLimited(
            StreamReader reader,
            int maximumCharacters)
        {
            StringBuilder output = new StringBuilder();
            char[] buffer = new char[4096];
            while (true)
            {
                int read = reader.Read(buffer, 0, buffer.Length);
                if (read == 0)
                {
                    return output.ToString();
                }
                if (output.Length + read > maximumCharacters)
                {
                    throw new InvalidDataException(
                        "Detector output exceeded the configured limit.");
                }
                output.Append(buffer, 0, read);
            }
        }

        private static string QuoteArgument(string value)
        {
            if (value.IndexOf('\0') >= 0 || value.IndexOf('"') >= 0)
            {
                throw new ArgumentException("Unsupported application path.");
            }
            int trailingBackslashes = 0;
            for (int index = value.Length - 1;
                 index >= 0 && value[index] == '\\';
                 --index)
            {
                ++trailingBackslashes;
            }
            return "\"" + value +
                new string('\\', trailingBackslashes) + "\"";
        }

        private static void CloseIfValid(IntPtr handle)
        {
            if (handle != IntPtr.Zero && handle != InvalidHandleValue)
            {
                CloseHandle(handle);
            }
        }
    }
}
