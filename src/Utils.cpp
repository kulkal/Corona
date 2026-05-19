#include "Utils.h"
#include "DX12Backend.h"

#include <vector>

namespace
{
	std::filesystem::path FindRuntimeRootFrom(std::filesystem::path start)
	{
		if (start.empty())
			return {};

		std::error_code ec;
		if (std::filesystem::is_regular_file(start, ec))
			start = start.parent_path();

		std::filesystem::path canonical = std::filesystem::weakly_canonical(start, ec);
		if (!ec)
			start = canonical;

		for (;;)
		{
			const bool hasCMake = std::filesystem::exists(start / L"CMakeLists.txt", ec);
			const bool hasSrc = std::filesystem::is_directory(start / L"src", ec);
			if (hasCMake && hasSrc)
				return start;

			const std::filesystem::path parent = start.parent_path();
			if (parent.empty() || parent == start)
				break;
			start = parent;
		}

		return {};
	}

	std::filesystem::path GetExecutableDirectory()
	{
		std::wstring modulePath(MAX_PATH, L'\0');
		const DWORD length = GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
		if (length == 0 || length >= modulePath.size())
			return {};

		modulePath.resize(length);
		return std::filesystem::path(modulePath).parent_path();
	}

	std::filesystem::path ResolveRuntimeRoot()
	{
		std::vector<std::filesystem::path> candidates;

		std::error_code ec;
		const std::filesystem::path workingDirectory = std::filesystem::current_path(ec);
		if (!ec && !workingDirectory.empty())
			candidates.push_back(workingDirectory);

		const std::filesystem::path executableDirectory = GetExecutableDirectory();
		if (!executableDirectory.empty())
		{
			candidates.push_back(executableDirectory);
			if (executableDirectory.has_parent_path())
				candidates.push_back(executableDirectory.parent_path());
		}

		for (const std::filesystem::path& candidate : candidates)
		{
			const std::filesystem::path root = FindRuntimeRootFrom(candidate);
			if (!root.empty())
				return root;
		}

		if (!workingDirectory.empty() && workingDirectory.filename() == L"src" && workingDirectory.has_parent_path())
			return workingDirectory.parent_path();
		if (!executableDirectory.empty() && executableDirectory.filename() == L"bin" && executableDirectory.has_parent_path())
			return executableDirectory.parent_path();
		return workingDirectory.empty() ? std::filesystem::path(L".") : workingDirectory;
	}
}

namespace RuntimePaths
{
	std::filesystem::path RootDirectory()
	{
		static const std::filesystem::path root = ResolveRuntimeRoot();
		return root;
	}

	std::filesystem::path SourceDirectory()
	{
		return RootDirectory() / L"src";
	}

	std::filesystem::path ConfigDirectory()
	{
		return RootDirectory() / L"config";
	}

	std::filesystem::path LogDirectory()
	{
		return RootDirectory() / L"logs";
	}

	std::filesystem::path DumpDirectory()
	{
		return RootDirectory() / L"dumps";
	}

	std::filesystem::path SourceFile(const wchar_t* relativePath)
	{
		return SourceDirectory() / relativePath;
	}

	std::filesystem::path ConfigFile(const wchar_t* fileName)
	{
		return ConfigDirectory() / fileName;
	}

	std::filesystem::path LogFile(const wchar_t* fileName)
	{
		return LogDirectory() / fileName;
	}
}

std::wstring AnsiToWString(const char* ansiString)
{
	WCHAR buffer[512];
	MultiByteToWideChar(CP_ACP, 0, ansiString, -1, buffer, 512);
	return std::wstring(buffer);
}

std::wstring GetDirectoryFromFilePath(const WCHAR* filePath_)
{
	std::wstring filePath(filePath_);
	size_t idx = filePath.find_last_of(L"\\/");
	if (idx != std::wstring::npos )
		return filePath.substr(0, idx + 1);
	else
		return std::wstring(L"");
}

// Returns the name of the file given the path (extension included)
std::wstring GetFileName(const WCHAR* filePath_)
{

	std::wstring filePath(filePath_);
	size_t idx = filePath.rfind(L'\\');
	if (idx != std::wstring::npos && idx < filePath.length() - 1)
		return filePath.substr(idx + 1);
	else
	{
		idx = filePath.rfind(L'/');
		if (idx != std::wstring::npos && idx < filePath.length() - 1)
			return filePath.substr(idx + 1);
		else
			return filePath;
	}
}

bool FileExists(const WCHAR* filePath)
{
	if (filePath == NULL)
		return false;

	DWORD fileAttr = GetFileAttributesW(filePath);
	if (fileAttr == INVALID_FILE_ATTRIBUTES)
		return false;

	return true;
}

std::wstring GetFileExtension(const WCHAR* filePath_)
{
	std::wstring filePath(filePath_);
	size_t idx = filePath.rfind(L'.');
	if (idx != std::wstring::npos)
		return filePath.substr(idx + 1, filePath.length() - idx - 1);
	else
		return std::wstring(L"");
}

void NVAftermathMarker(GFSDK_Aftermath_ContextHandle ah, std::string markerName)
{
#if USE_AFTERMATH
	if (!ah || markerName.empty())
		return;
	GFSDK_Aftermath_Result ar = GFSDK_Aftermath_SetEventMarker(ah, markerName.c_str(), static_cast<uint32_t>(markerName.length()));
	(void)ar;
#else
	(void)ah;
	(void)markerName;
#endif
}
