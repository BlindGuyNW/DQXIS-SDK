#include "pch.h"
#include <fstream>
#include <mutex>
#include <string>

// Accessibility mod for DQXI S
// Hooks UObject::ProcessEvent to detect UI events and speak menu items
// via Tolk (screen reader abstraction library).

// --- Log file ---

static std::ofstream g_LogFile;
static std::mutex g_LogMutex;

static void LogEvent(const char* prefix, const std::string& text)
{
  std::lock_guard<std::mutex> lock(g_LogMutex);
  if (g_LogFile.is_open())
  {
    g_LogFile << prefix << text << std::endl;
    g_LogFile.flush();
  }
}

static void LogEventW(const char* prefix, const wchar_t* text)
{
  std::lock_guard<std::mutex> lock(g_LogMutex);
  if (g_LogFile.is_open())
  {
    int len = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (len > 0)
    {
      std::string narrow(len - 1, '\0');
      WideCharToMultiByte(CP_UTF8, 0, text, -1, &narrow[0], len, nullptr, nullptr);
      g_LogFile << prefix << narrow << std::endl;
    }
    g_LogFile.flush();
  }
}

// --- Tolk dynamic loading ---
// We load Tolk.dll at runtime so there's no build-time dependency.
// IMPORTANT: Loading is deferred until after DllMain completes to avoid loader lock.

typedef void   (*Tolk_Load_Fn)();
typedef void   (*Tolk_Unload_Fn)();
typedef bool   (*Tolk_IsLoaded_Fn)();
typedef void   (*Tolk_TrySAPI_Fn)(bool);
typedef const wchar_t* (*Tolk_DetectScreenReader_Fn)();
typedef bool   (*Tolk_Output_Fn)(const wchar_t*, bool);
typedef bool   (*Tolk_Speak_Fn)(const wchar_t*, bool);
typedef bool   (*Tolk_Silence_Fn)();

static HMODULE g_TolkModule = nullptr;
static Tolk_Load_Fn                g_Tolk_Load = nullptr;
static Tolk_Unload_Fn              g_Tolk_Unload = nullptr;
static Tolk_IsLoaded_Fn            g_Tolk_IsLoaded = nullptr;
static Tolk_TrySAPI_Fn             g_Tolk_TrySAPI = nullptr;
static Tolk_DetectScreenReader_Fn  g_Tolk_DetectScreenReader = nullptr;
static Tolk_Output_Fn              g_Tolk_Output = nullptr;
static Tolk_Speak_Fn               g_Tolk_Speak = nullptr;
static Tolk_Silence_Fn             g_Tolk_Silence = nullptr;

// Tolk loading state: 0 = not tried, 1 = loaded OK, -1 = failed
static int g_TolkState = 0;

// Speak text through the active screen reader, interrupting previous speech
static void Speak(const wchar_t* text)
{
  if (g_Tolk_Output)
    g_Tolk_Output(text, true);
}

static bool LoadTolk()
{
  if (g_TolkState != 0)
    return g_TolkState == 1;

  g_TolkModule = LoadLibraryW(L"Tolk.dll");
  if (!g_TolkModule)
  {
    g_TolkState = -1;
    LogEvent("[TOLK] ", "Failed to load Tolk.dll - place it next to the game EXE");
    return false;
  }

  g_Tolk_Load = (Tolk_Load_Fn)GetProcAddress(g_TolkModule, "Tolk_Load");
  g_Tolk_Unload = (Tolk_Unload_Fn)GetProcAddress(g_TolkModule, "Tolk_Unload");
  g_Tolk_IsLoaded = (Tolk_IsLoaded_Fn)GetProcAddress(g_TolkModule, "Tolk_IsLoaded");
  g_Tolk_TrySAPI = (Tolk_TrySAPI_Fn)GetProcAddress(g_TolkModule, "Tolk_TrySAPI");
  g_Tolk_DetectScreenReader = (Tolk_DetectScreenReader_Fn)GetProcAddress(g_TolkModule, "Tolk_DetectScreenReader");
  g_Tolk_Output = (Tolk_Output_Fn)GetProcAddress(g_TolkModule, "Tolk_Output");
  g_Tolk_Speak = (Tolk_Speak_Fn)GetProcAddress(g_TolkModule, "Tolk_Speak");
  g_Tolk_Silence = (Tolk_Silence_Fn)GetProcAddress(g_TolkModule, "Tolk_Silence");

  if (!g_Tolk_Load || !g_Tolk_Output || !g_Tolk_Speak)
  {
    FreeLibrary(g_TolkModule);
    g_TolkModule = nullptr;
    g_TolkState = -1;
    LogEvent("[TOLK] ", "Tolk.dll loaded but missing required exports");
    return false;
  }

  // Initialize Tolk, enable SAPI as fallback if no screen reader is running
  if (g_Tolk_TrySAPI)
    g_Tolk_TrySAPI(true);
  g_Tolk_Load();

  auto sr = g_Tolk_DetectScreenReader ? g_Tolk_DetectScreenReader() : nullptr;
  if (sr)
    LogEventW("[TOLK] Screen reader detected: ", sr);
  else
    LogEvent("[TOLK] ", "No screen reader detected, using SAPI fallback");

  g_TolkState = 1;
  Speak(L"DQXI accessibility mod loaded");
  return true;
}

// --- FText string extraction ---
// In UE4 4.18, FText (0x18 bytes) holds a shared pointer to ITextData.
// ITextData subclasses store an FString (wchar_t* Data, int32 Count, int32 Max)
// at varying offsets depending on the subclass. We scan plausible offsets.

// SEH-safe: check if an FString struct lives at (base + offset)
// FString layout: [wchar_t* Data (8)] [int32 Count (4)] [int32 Max (4)]
// Validates the full structure to avoid false positives from vtable pointers etc.
static const wchar_t* TryReadFStringAt(uintptr_t base, uintptr_t offset)
{
  __try
  {
    // Read FString: [wchar_t* Data (8)] [int32 Count (4)] [int32 Max (4)]
    auto dataPtr = *reinterpret_cast<const wchar_t**>(base + offset);
    if (!dataPtr)
      return nullptr;

    auto count = *reinterpret_cast<int32_t*>(base + offset + 8);
    auto max = *reinterpret_cast<int32_t*>(base + offset + 12);

    // Validate FString structure
    if (count <= 1 || count > 1024 || max < count || max > 4096)
      return nullptr;

    // Read and validate string content (SEH will catch bad pointers)
    wchar_t first = *dataPtr;
    if (first < 0x20)
      return nullptr;

    auto len = wcslen(dataPtr);
    if (len == 0 || len > 1024)
      return nullptr;

    return dataPtr;
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {}
  return nullptr;
}

static const wchar_t* TryReadFText(void* ftextPtr)
{
  if (!ftextPtr)
    return nullptr;

  __try
  {
    auto textDataPtr = *reinterpret_cast<uintptr_t*>(ftextPtr);
    if (!textDataPtr || IsBadReadPtr((void*)textDataPtr, 8))
      return nullptr;

    // Pass 1: Scan ITextData for FString structures directly
    // Known layouts from hex dumps:
    //   Simple FText: FString at 0x08
    //   History-based: FString at 0x28 (source), 0x38 (display)
    static const uintptr_t offsets[] = { 0x38, 0x28, 0x08, 0x18, 0x48 };
    for (auto off : offsets)
    {
      auto result = TryReadFStringAt(textDataPtr, off);
      if (result)
        return result;
    }

    // Pass 2: Follow the shared ref pointer at ITextData+0x08
    // For FTextHistory_StringTableEntry, the resolved display text lives in a
    // referenced FStringTableEntry object, not inline in ITextData.
    auto subObjPtr = *reinterpret_cast<uintptr_t*>(textDataPtr + 0x08);
    if (subObjPtr && subObjPtr > 0x10000)
    {
      static const uintptr_t subOffsets[] = { 0x00, 0x08, 0x10, 0x18, 0x20, 0x28, 0x38, 0x48, 0x58 };
      for (auto off : subOffsets)
      {
        auto result = TryReadFStringAt(subObjPtr, off);
        if (result)
          return result;
      }
    }
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
  }

  return nullptr;
}

// SEH-safe: try calling ITextData::GetDisplayString() via vtable
// In UE4, ITextData has virtual methods: [0] destructor, [1] GetDisplayString()
// GetDisplayString() returns const FString& and may trigger lazy text resolution
static const wchar_t* TryCallGetDisplayString(uintptr_t textDataPtr)
{
  __try
  {
    auto vtable = *reinterpret_cast<uintptr_t*>(textDataPtr);
    if (!vtable || vtable < 0x10000)
      return nullptr;

    // Try vtable[1] first (GetDisplayString after destructor)
    // Then vtable[2] in case there's an extra entry
    for (int idx = 1; idx <= 2; idx++)
    {
      auto funcPtr = *reinterpret_cast<uintptr_t*>(vtable + idx * 8);
      if (!funcPtr || funcPtr < 0x10000)
        continue;

      // Call: const FString& ITextData::GetDisplayString() const
      // MSVC x64: this in RCX, returns FString* in RAX
      typedef uintptr_t (__fastcall *GetDisplayStringFn)(uintptr_t);
      auto fn = reinterpret_cast<GetDisplayStringFn>(funcPtr);
      auto fstringAddr = fn(textDataPtr);

      if (!fstringAddr || fstringAddr < 0x10000)
        continue;

      // Read FString: [wchar_t* Data (8)] [int32 Count (4)] [int32 Max (4)]
      auto data = *reinterpret_cast<const wchar_t**>(fstringAddr);
      if (!data)
        continue;

      auto count = *reinterpret_cast<int32_t*>(fstringAddr + 8);
      if (count <= 1 || count > 1024)
        continue;

      wchar_t first = *data;
      if (first < 0x20)
        continue;

      return data;
    }
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {}
  return nullptr;
}

// Forward declaration (defined later, after ProcessEvent hook section)
static const char* TryReadFName(uintptr_t baseAddr, uintptr_t offset);

// --- GetCaption via ProcessEvent ---
// Instead of trying to read FText memory directly, call the game's own
// GetCaption() method through UE4's reflection system. This lets the
// engine resolve string-table-based FText entries properly.

static UFunction* g_GetCaptionFunc = nullptr;
static bool g_GetCaptionSearched = false;

// Find the GetCaption UFunction (no SEH - uses std::string via FindObject)
static void EnsureGetCaptionFunc()
{
  if (g_GetCaptionSearched)
    return;
  g_GetCaptionSearched = true;

  g_GetCaptionFunc = UObject::FindObject<UFunction>("Function JackGame.JackUMGItemBase.GetCaption");
  if (g_GetCaptionFunc)
    LogEvent("[CAPTION] ", "Found GetCaption UFunction");
  else
    LogEvent("[CAPTION] ", "GetCaption UFunction NOT found");
}

// SEH-safe wrapper: call ProcessEvent to invoke GetCaption on an item
// Returns the resolved caption text, or nullptr on failure
// MUST NOT have any C++ objects with destructors in this function
typedef void (*ProcessEventFn_t)(UObject*, UFunction*, void*);
static ProcessEventFn_t g_ProcessEvent_Orig = nullptr; // set when hook is installed

static const wchar_t* TryCallGetCaption_SEH(UObject* item, UFunction* func)
{
  if (!g_ProcessEvent_Orig)
    return nullptr;

  // GetCaption returns FText (0x18 bytes)
  struct { char ReturnValue[0x18]; } params;
  memset(&params, 0, sizeof(params));

  __try
  {
    g_ProcessEvent_Orig(item, func, &params);
    return TryReadFText(&params.ReturnValue);
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {}
  return nullptr;
}

// Dump hex bytes of FText area for debugging failed reads
// Uses IsBadReadPtr guards instead of SEH (can't mix __try with std::string params)
static void DumpFTextHex(void* ftextPtr, const char* itemId)
{
  if (!ftextPtr || IsBadReadPtr(ftextPtr, 8))
    return;

  auto textDataPtr = *reinterpret_cast<uintptr_t*>(ftextPtr);
  if (!textDataPtr || IsBadReadPtr((void*)textDataPtr, 0x50))
    return;

  char buf[256];
  sprintf_s(buf, "[FTEXT-DUMP] %s -> ITextData at %p:", itemId, (void*)textDataPtr);
  LogEvent("", buf);

  // Dump first 80 bytes of ITextData in hex
  auto bytes = reinterpret_cast<uint8_t*>(textDataPtr);
  for (int row = 0; row < 5; row++)
  {
    int off = row * 16;
    sprintf_s(buf, "  +0x%02X: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X",
      off,
      bytes[off+0], bytes[off+1], bytes[off+2], bytes[off+3],
      bytes[off+4], bytes[off+5], bytes[off+6], bytes[off+7],
      bytes[off+8], bytes[off+9], bytes[off+10], bytes[off+11],
      bytes[off+12], bytes[off+13], bytes[off+14], bytes[off+15]);
    LogEvent("", buf);
  }

  // Follow pointer at +0x08 (shared ref to subobject) and dump those bytes too
  auto subObjPtr = *reinterpret_cast<uintptr_t*>(textDataPtr + 0x08);
  if (subObjPtr && subObjPtr > 0x10000 && !IsBadReadPtr((void*)subObjPtr, 0x60))
  {
    sprintf_s(buf, "[FTEXT-DUMP] %s -> SubObj at %p:", itemId, (void*)subObjPtr);
    LogEvent("", buf);
    auto subBytes = reinterpret_cast<uint8_t*>(subObjPtr);
    for (int row = 0; row < 6; row++)
    {
      int off = row * 16;
      sprintf_s(buf, "  +0x%02X: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X",
        off,
        subBytes[off+0], subBytes[off+1], subBytes[off+2], subBytes[off+3],
        subBytes[off+4], subBytes[off+5], subBytes[off+6], subBytes[off+7],
        subBytes[off+8], subBytes[off+9], subBytes[off+10], subBytes[off+11],
        subBytes[off+12], subBytes[off+13], subBytes[off+14], subBytes[off+15]);
      LogEvent("", buf);
    }

    // Read FName at SubObj+0x18 (string table key)
    const char* subKeyName = TryReadFName((uintptr_t)subObjPtr, 0x18);
    if (subKeyName)
    {
      sprintf_s(buf, "[FTEXT-KEY] %s -> SubObj FName: %s", itemId, subKeyName);
      LogEvent("", buf);
    }

    // Follow pointer at SubObj+0x40 for deeper inspection
    auto deepPtr = *reinterpret_cast<uintptr_t*>((uintptr_t)subObjPtr + 0x40);
    if (deepPtr && deepPtr > 0x10000 && !IsBadReadPtr((void*)deepPtr, 0x60))
    {
      sprintf_s(buf, "[FTEXT-DUMP] %s -> Deep at %p:", itemId, (void*)deepPtr);
      LogEvent("", buf);
      auto deepBytes = reinterpret_cast<uint8_t*>(deepPtr);
      for (int row = 0; row < 6; row++)
      {
        int off = row * 16;
        sprintf_s(buf, "  +0x%02X: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X",
          off,
          deepBytes[off+0], deepBytes[off+1], deepBytes[off+2], deepBytes[off+3],
          deepBytes[off+4], deepBytes[off+5], deepBytes[off+6], deepBytes[off+7],
          deepBytes[off+8], deepBytes[off+9], deepBytes[off+10], deepBytes[off+11],
          deepBytes[off+12], deepBytes[off+13], deepBytes[off+14], deepBytes[off+15]);
        LogEvent("", buf);
      }
    }
  }
}

// --- ProcessEvent hook ---

typedef void (*ProcessEventFn)(UObject*, UFunction*, void*);
ProcessEventFn ProcessEvent_Orig = nullptr;

// Track last spoken text to avoid repeating the same thing
static std::wstring g_LastSpoken;

// SEH-safe helper: try to read a pointer at an address
static uintptr_t TryReadPtr(uintptr_t addr)
{
  __try
  {
    return *reinterpret_cast<uintptr_t*>(addr);
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {}
  return 0;
}

// SEH-safe helper: try to read FName from an item at an offset
static const char* TryReadFName(uintptr_t baseAddr, uintptr_t offset)
{
  __try
  {
    auto fname = reinterpret_cast<FName*>(baseAddr + offset);
    if (fname && fname->ComparisonIndex > 0)
      return fname->GetName();
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {}
  return nullptr;
}

// SEH-safe: follow FText -> ITextData+0x08 -> SubObj -> FName at +0x18
// For string table FText entries, this reads the string table key name
static const char* TryReadFTextSubObjFName(void* ftextPtr)
{
  __try
  {
    if (!ftextPtr) return nullptr;
    auto textDataPtr = *reinterpret_cast<uintptr_t*>(ftextPtr);
    if (!textDataPtr) return nullptr;
    auto subObjPtr = *reinterpret_cast<uintptr_t*>(textDataPtr + 0x08);
    if (!subObjPtr || subObjPtr < 0x10000) return nullptr;
    auto fname = reinterpret_cast<FName*>(subObjPtr + 0x18);
    if (fname && fname->ComparisonIndex > 0)
      return fname->GetName();
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {}
  return nullptr;
}

// SEH-safe: read a uint8 at an address
static uint8_t TryReadByte(uintptr_t addr)
{
  __try { return *reinterpret_cast<uint8_t*>(addr); }
  __except (EXCEPTION_EXECUTE_HANDLER) {}
  return 0xFF;
}

// SEH-safe: read an int32 at an address
static int32_t TryReadInt32(uintptr_t addr)
{
  __try { return *reinterpret_cast<int32_t*>(addr); }
  __except (EXCEPTION_EXECUTE_HANDLER) {}
  return -1;
}

// --- GetValue via ProcessEvent ---
// For HorizontalParts widgets (display mode, etc.), GetValue() returns the
// currently selected option index. We use this to pick the right TextBlock.

static UFunction* g_GetValueFunc = nullptr;
static bool g_GetValueSearched = false;

static void EnsureGetValueFunc()
{
  if (g_GetValueSearched)
    return;
  g_GetValueSearched = true;

  g_GetValueFunc = UObject::FindObject<UFunction>("Function JackGame.JackUMGHorizontalParts.GetValue");
  if (g_GetValueFunc)
    LogEvent("[GETVALUE] ", "Found GetValue UFunction");
  else
    LogEvent("[GETVALUE] ", "GetValue UFunction NOT found");
}

// SEH-safe wrapper: call ProcessEvent to invoke GetValue on a widget
// Returns the selected index, or -1 on failure
static int TryCallGetValue_SEH(UObject* widget, UFunction* func)
{
  if (!g_ProcessEvent_Orig)
    return -1;

  struct { int ReturnValue; } params;
  params.ReturnValue = -1;

  __try
  {
    g_ProcessEvent_Orig(widget, func, &params);
    return params.ReturnValue;
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {}
  return -1;
}

// SEH-safe helper: speak ItemID/TextID fallback when FText read fails
static void SpeakItemFallbackName(UObject* selectedItemPtr)
{
  auto itemAddr = reinterpret_cast<uintptr_t>(selectedItemPtr);
  const char* textName = TryReadFName(itemAddr, 0x0388);
  const char* itemName = TryReadFName(itemAddr, 0x0380);

  const char* nameToSpeak = textName ? textName : itemName;
  if (!nameToSpeak || strlen(nameToSpeak) == 0 || strcmp(nameToSpeak, "None") == 0)
    return;

  // Log the actual class of this item
  const char* itemClassName = nullptr;
  auto classPtr = TryReadPtr(itemAddr + offsetof(UObject, Class));
  if (classPtr)
  {
    itemClassName = TryReadFName(classPtr, 0x18);
    if (itemClassName)
    {
      char classBuf[256];
      sprintf_s(classBuf, "[ITEM-CLASS] %s is a %s", nameToSpeak, itemClassName);
      LogEvent("", classBuf);
    }
  }

  // --- Method 4: GetValue + TextBlock index for HorizontalParts widgets ---
  // These widgets (e.g. Display Mode selector) have multiple TextBlocks but use
  // position/clipping (not visibility) to show the selected one.
  // We call GetValue() to get the selected index, then collect all TextBlock texts
  // and pick the one at that index.
  EnsureGetValueFunc();
  if (g_GetValueFunc && g_ProcessEvent_Orig)
  {
    int selectedIdx = TryCallGetValue_SEH(selectedItemPtr, g_GetValueFunc);
    if (selectedIdx >= 0)
    {
      char idxBuf[128];
      sprintf_s(idxBuf, "[GETVALUE] %s -> GetValue() = %d", nameToSpeak, selectedIdx);
      LogEvent("", idxBuf);

      // Collect TextBlock texts from child widgets
      const wchar_t* textBlockTexts[16] = {};
      int textBlockCount = 0;

      for (uintptr_t off = 0x03D0; off < 0x0600 && textBlockCount < 16; off += 8)
      {
        auto childPtr = TryReadPtr(itemAddr + off);
        if (!childPtr || childPtr < 0x10000)
          continue;
        auto childClassPtr = TryReadPtr(childPtr + 0x10);
        if (!childClassPtr || childClassPtr < 0x10000)
          continue;
        const char* childClassName = TryReadFName(childClassPtr, 0x18);
        if (!childClassName || strlen(childClassName) == 0)
          continue;

        if (strstr(childClassName, "TextBlock"))
        {
          const wchar_t* txt = TryReadFText(reinterpret_cast<void*>(childPtr + 0x00F8));
          if (txt && txt[0] != L'-')
          {
            textBlockTexts[textBlockCount++] = txt;
          }
        }
      }

      sprintf_s(idxBuf, "[GETVALUE] Found %d TextBlocks, selectedIdx=%d", textBlockCount, selectedIdx);
      LogEvent("", idxBuf);

      // Use the selected index to pick the right TextBlock
      if (selectedIdx < textBlockCount && textBlockTexts[selectedIdx])
      {
        const wchar_t* selectedText = textBlockTexts[selectedIdx];
        std::wstring text(selectedText);
        if (text != g_LastSpoken && !text.empty())
        {
          g_LastSpoken = text;
          Speak(selectedText);
          LogEventW("[SPEAK-VALUE] ", selectedText);
        }
        else
        {
          LogEventW("[SKIP-DUP] ", selectedText);
        }
        return;
      }
    }
  }

  // --- Fallback: scan child TextBlocks (first readable one) ---
  const wchar_t* visibleText = nullptr;
  {
    char scanBuf[256];
    for (uintptr_t off = 0x03D0; off < 0x0600; off += 8)
    {
      auto childPtr = TryReadPtr(itemAddr + off);
      if (!childPtr || childPtr < 0x10000)
        continue;
      auto childClassPtr = TryReadPtr(childPtr + 0x10);
      if (!childClassPtr || childClassPtr < 0x10000)
        continue;
      const char* childClassName = TryReadFName(childClassPtr, 0x18);
      if (!childClassName || strlen(childClassName) == 0)
        continue;

      if (strstr(childClassName, "TextBlock"))
      {
        const wchar_t* txt = TryReadFText(reinterpret_cast<void*>(childPtr + 0x00F8));
        if (!txt) continue;

        int nLen = WideCharToMultiByte(CP_UTF8, 0, txt, -1, nullptr, 0, nullptr, nullptr);
        if (nLen > 0 && nLen < 200)
        {
          char nBuf[200];
          WideCharToMultiByte(CP_UTF8, 0, txt, -1, nBuf, 200, nullptr, nullptr);
          sprintf_s(scanBuf, "[TEXTBLOCK] +0x%04X text: \"%s\"", (int)off, nBuf);
          LogEvent("", scanBuf);
        }

        if (!visibleText && txt[0] != L'-')
          visibleText = txt;
      }
    }
  }

  if (visibleText)
  {
    std::wstring text(visibleText);
    if (text != g_LastSpoken && !text.empty())
    {
      g_LastSpoken = text;
      Speak(visibleText);
      LogEventW("[SPEAK-CHILD] ", visibleText);
    }
    else
    {
      LogEventW("[SKIP-DUP-CHILD] ", visibleText);
    }
    return;
  }

  // --- Final fallback: speak the FName (ItemID/TextID) ---
  int wlen = MultiByteToWideChar(CP_UTF8, 0, nameToSpeak, -1, nullptr, 0);
  if (wlen > 0)
  {
    auto wbuf = new wchar_t[wlen];
    MultiByteToWideChar(CP_UTF8, 0, nameToSpeak, -1, wbuf, wlen);

    if (wcscmp(wbuf, g_LastSpoken.c_str()) != 0 && wcslen(wbuf) > 0)
    {
      g_LastSpoken = wbuf;
      Speak(wbuf);
      LogEventW("[SPEAK-ID] ", wbuf);
    }
    else
    {
      LogEventW("[SKIP-DUP-ID] ", wbuf);
    }
    delete[] wbuf;
  }
}

// Try to read and speak the selected item from a window widget
static void SpeakSelectedItem(UObject* windowObj)
{
  if (!windowObj)
    return;

  auto selectedItemPtr = *reinterpret_cast<UObject**>(reinterpret_cast<uintptr_t>(windowObj) + 0x03A0);
  if (!selectedItemPtr)
    return;

  // Method 1: Call GetCaption() via ProcessEvent (most reliable - handles string table FText)
  EnsureGetCaptionFunc();
  const wchar_t* captionStr = nullptr;
  if (g_GetCaptionFunc && g_ProcessEvent_Orig)
    captionStr = TryCallGetCaption_SEH(selectedItemPtr, g_GetCaptionFunc);

  // Method 2: Direct FText memory read from CaptionText at +0x0398
  if (!captionStr)
  {
    void* captionTextPtr = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(selectedItemPtr) + 0x0398);
    captionStr = TryReadFText(captionTextPtr);

    // Method 3: ITextData::GetDisplayString() via vtable
    if (!captionStr)
    {
      auto textDataPtr = TryReadPtr(reinterpret_cast<uintptr_t>(captionTextPtr));
      if (textDataPtr && textDataPtr > 0x10000)
        captionStr = TryCallGetDisplayString(textDataPtr);
    }
  }

  if (captionStr)
  {
    std::wstring text(captionStr);
    if (text != g_LastSpoken && !text.empty())
    {
      g_LastSpoken = text;
      Speak(captionStr);
      LogEventW("[SPEAK-CAPTION] ", captionStr);
      return;
    }
    else if (!text.empty())
    {
      LogEventW("[SKIP-DUP-CAPTION] ", captionStr);
      return;
    }
  }

  SpeakItemFallbackName(selectedItemPtr);
}

// SEH-safe event handler - no C++ objects with destructors allowed here
// This runs on EVERY UE4 function call, so it must be fast and crash-proof
static void TryHandleEvent(UObject* obj, UFunction* func)
{
  __try
  {
    if (!obj || !func)
      return;
    if (IsBadReadPtr(obj, sizeof(void*)))
      return;
    if (!obj->Class || IsBadReadPtr(obj->Class, sizeof(void*)))
      return;

    const char* funcName = func->Name.GetName();
    if (!funcName || IsBadReadPtr(funcName, 1))
      return;

    // Fast reject: check first character before doing full strcmp
    char fc = funcName[0];
    if (fc != 'O' && fc != 'C' && fc != 'S')
      return;

    if (strcmp(funcName, "OnItemListControl") == 0)
    {
      SpeakSelectedItem(obj);
    }
    else if (strcmp(funcName, "Construct") == 0)
    {
      const char* className = obj->Class->Name.GetName();
      if (!className || IsBadReadPtr(className, 1))
        return;

      if (strstr(className, "Menu_Title_Push"))
        Speak(L"Press any button");
      else if (strstr(className, "Menu_Title_000"))
        Speak(L"Title Screen");
      else if (strstr(className, "Namae_Window"))
        Speak(L"Menu");
      else if (strstr(className, "Namae_Save_data"))
        Speak(L"Save Data");
      else if (strstr(className, "CS_Skip"))
        Speak(L"Press to skip cutscene");
    }
    else if (strcmp(funcName, "SkipCutScene") == 0)
    {
      Speak(L"Cutscene skipped");
    }
    else if (strcmp(funcName, "SelectAutoSave") == 0)
    {
      SpeakSelectedItem(obj);
    }
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
  }
}

void ProcessEvent_Hook(UObject* obj, UFunction* func, void* params)
{
  // Defer Tolk loading until after DllMain has finished (avoids loader lock issues)
  if (g_TolkState == 0)
    LoadTolk();

  // Set the global for SEH-safe GetCaption calls
  if (!g_ProcessEvent_Orig && ProcessEvent_Orig)
    g_ProcessEvent_Orig = ProcessEvent_Orig;

  TryHandleEvent(obj, func);

  if (ProcessEvent_Orig)
    ProcessEvent_Orig(obj, func, params);
}

void Init_AccessibilityLogger()
{
  // Open log file next to game EXE
  WCHAR exePath[4096];
  GetModuleFileName(NULL, exePath, 4096);

  std::wstring basePath = exePath;
  auto lastSep = basePath.find_last_of(L"\\/");
  if (lastSep != std::wstring::npos)
    basePath = basePath.substr(0, lastSep + 1);

  std::wstring logPath = basePath + L"DQXIS-accessibility.log";

  g_LogFile.open(logPath, std::ios::trunc);
  if (g_LogFile.is_open())
  {
    g_LogFile << "=== DQXIS Accessibility Mod ===" << std::endl;
    g_LogFile.flush();
  }

  // NOTE: Tolk loading is deferred to the first ProcessEvent call
  // Loading DLLs during DLL_PROCESS_ATTACH (DllMain) causes loader lock issues
  LogEvent("[INIT] ", "Tolk loading deferred until after DllMain completes");

  // Hook ProcessEvent
  auto hookAddr = (LPVOID)(mBaseAddress + GameAddrs->UObject__ProcessEvent);
  MH_STATUS status = MH_CreateHook(hookAddr, (LPVOID)ProcessEvent_Hook, (LPVOID*)&ProcessEvent_Orig);

  if (status == MH_OK)
    LogEvent("[HOOK] ", "ProcessEvent hook created successfully");
  else
  {
    char buf[128];
    sprintf_s(buf, "MH_CreateHook FAILED with status %d", (int)status);
    LogEvent("[HOOK] ", buf);
  }
}
