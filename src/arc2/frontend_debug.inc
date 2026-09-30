// Diagnostic path, enabled only when ARC2_DEBUG_TRACE names an output file.
// This lives in the frontend's anonymous namespace.
HANDLE diagnostic_file = INVALID_HANDLE_VALUE;
void debug_line(const char* line) noexcept {
    if (diagnostic_file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(diagnostic_file, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
}
void debug_trace(const char* event, const void* a = nullptr, const void* b = nullptr,
                 std::uint64_t value = 0) noexcept {
    if (diagnostic_file == INVALID_HANDLE_VALUE) return;
    char line[256]{};
    _snprintf_s(line, _TRUNCATE, "tid=%lu %-32s a=%p b=%p v=%llu\r\n",
                GetCurrentThreadId(), event, a, b, static_cast<unsigned long long>(value));
    debug_line(line);
}
LONG CALLBACK diagnostic_exception(EXCEPTION_POINTERS* exception) noexcept {
    if (diagnostic_file == INVALID_HANDLE_VALUE) return EXCEPTION_CONTINUE_SEARCH;
    char line[512]{};
    auto* context = exception->ContextRecord;
    _snprintf_s(line, _TRUNCATE,
        "EXCEPTION tid=%lu code=%08lx address=%p rip=%p rsp=%p rax=%p rcx=%p rdx=%p r8=%p r9=%p\r\n",
        GetCurrentThreadId(), exception->ExceptionRecord->ExceptionCode, exception->ExceptionRecord->ExceptionAddress,
        reinterpret_cast<void*>(context->Rip), reinterpret_cast<void*>(context->Rsp),
        reinterpret_cast<void*>(context->Rax), reinterpret_cast<void*>(context->Rcx),
        reinterpret_cast<void*>(context->Rdx), reinterpret_cast<void*>(context->R8),
        reinterpret_cast<void*>(context->R9));
    debug_line(line);
    auto* stack = reinterpret_cast<const std::uintptr_t*>(context->Rsp);
    for (int i = 0; i < 12; ++i) {
        _snprintf_s(line, _TRUNCATE, "stack[%d]=%p\r\n", i,
                    reinterpret_cast<const void*>(stack[i]));
        debug_line(line);
    }
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(exception->ExceptionRecord->ExceptionAddress), &module)) {
        wchar_t name[MAX_PATH]{};
        GetModuleFileNameW(module, name, MAX_PATH);
        char narrow[MAX_PATH * 3]{};
        WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof(narrow), nullptr, nullptr);
        _snprintf_s(line, _TRUNCATE, "module=%s base=%p\r\n", narrow, module);
        debug_line(line);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
void init_diagnostics() noexcept {
    static std::once_flag once;
    std::call_once(once, [] {
        wchar_t path[MAX_PATH]{};
        auto len = GetEnvironmentVariableW(L"ARC2_DEBUG_TRACE", path, MAX_PATH);
        if (!len || len >= MAX_PATH) return;
        diagnostic_file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (diagnostic_file != INVALID_HANDLE_VALUE) {
            AddVectoredExceptionHandler(1, diagnostic_exception);
            debug_trace("diagnostics enabled");
            HMODULE self = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(&diagnostic_exception), &self))
                debug_trace("frontend module base", self);
        }
    });
}
