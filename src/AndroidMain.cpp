#include "stdafx.h"
#include "Corona.h"
#include "PlatformWindow.h"

#if CORONA_PLATFORM_IS_ANDROID

#include <android/asset_manager.h>
#include <android/log.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/window.h>
#include <android_native_app_glue.h>
#include <jni.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

namespace
{
	constexpr const char* kLogTag = "Corona";
	constexpr const char* kAssetRoot = "CoronaRuntime";
	constexpr jint kScreenOrientationLandscape = 0;

	std::unique_ptr<Corona> gCoronaApp;
	bool gCoronaInitialized = false;
	bool gLoggedWaitingForLandscapeWindow = false;

	void LogInfo(const char* message)
	{
		__android_log_print(ANDROID_LOG_INFO, kLogTag, "%s", message ? message : "");
	}

	void LogError(const char* message)
	{
		__android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s", message ? message : "");
	}

	void RequestLandscapeOrientation(android_app* app)
	{
		if (!app || !app->activity || !app->activity->vm || !app->activity->clazz)
			return;

		ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON, 0);

		JNIEnv* env = nullptr;
		bool attachedThread = false;
		const jint getEnvResult = app->activity->vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
		if (getEnvResult == JNI_EDETACHED)
		{
			if (app->activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
				return;
			attachedThread = true;
		}
		else if (getEnvResult != JNI_OK || !env)
		{
			return;
		}

		jclass activityClass = env->GetObjectClass(app->activity->clazz);
		if (activityClass)
		{
			jmethodID setRequestedOrientation = env->GetMethodID(activityClass, "setRequestedOrientation", "(I)V");
			if (setRequestedOrientation)
				env->CallVoidMethod(app->activity->clazz, setRequestedOrientation, kScreenOrientationLandscape);
			env->DeleteLocalRef(activityClass);
		}

		if (env->ExceptionCheck())
		{
			env->ExceptionClear();
			LogError("Failed to request landscape orientation.");
		}

		if (attachedThread)
			app->activity->vm->DetachCurrentThread();
	}

	void RequestPreferredFrameRate(android_app* app, float targetFrameRate)
	{
		if (!app || !app->window)
			return;

#if __ANDROID_API__ >= 31
		using SetFrameRateWithChangeStrategyProc = int32_t (*)(ANativeWindow*, float, int8_t, int8_t);
		auto* setFrameRateWithChangeStrategy = reinterpret_cast<SetFrameRateWithChangeStrategyProc>(
			dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRateWithChangeStrategy"));
		if (!setFrameRateWithChangeStrategy)
		{
			LogInfo("ANativeWindow_setFrameRateWithChangeStrategy is unavailable.");
			return;
		}

		const int32_t result = setFrameRateWithChangeStrategy(
			app->window,
			targetFrameRate,
			ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_DEFAULT,
			ANATIVEWINDOW_CHANGE_FRAME_RATE_ALWAYS);
		__android_log_print(ANDROID_LOG_INFO, kLogTag, "Requested %.1fHz window frame rate: result=%d", targetFrameRate, result);
#else
		(void)app;
		(void)targetFrameRate;
#endif
	}

	std::chrono::steady_clock::duration GetFrameRateLimitInterval(const Corona* app)
	{
		if (!app)
			return std::chrono::steady_clock::duration::zero();

		const double targetHz = app->GetTargetFrameRateLimitHz();
		if (!(targetHz > 0.0))
			return std::chrono::steady_clock::duration::zero();

		return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
			std::chrono::duration<double>(1.0 / targetHz));
	}

	bool CopyAssetFile(AAssetManager* assetManager, const std::string& assetPath, const std::filesystem::path& destination)
	{
		AAsset* asset = AAssetManager_open(assetManager, assetPath.c_str(), AASSET_MODE_STREAMING);
		if (!asset)
			return false;

		std::error_code ec;
		std::filesystem::create_directories(destination.parent_path(), ec);
		std::ofstream output(destination, std::ios::binary | std::ios::trunc);
		if (!output.is_open())
		{
			AAsset_close(asset);
			return false;
		}

		char buffer[32768];
		for (;;)
		{
			const int bytesRead = AAsset_read(asset, buffer, sizeof(buffer));
			if (bytesRead < 0)
			{
				AAsset_close(asset);
				return false;
			}
			if (bytesRead == 0)
				break;
			output.write(buffer, bytesRead);
			if (!output.good())
			{
				AAsset_close(asset);
				return false;
			}
		}

		AAsset_close(asset);
		return true;
	}

	void CopyAssetTree(AAssetManager* assetManager, const std::string& assetPath, const std::filesystem::path& destination)
	{
		AAssetDir* directory = AAssetManager_openDir(assetManager, assetPath.c_str());
		if (!directory)
			return;

		std::error_code ec;
		std::filesystem::create_directories(destination, ec);

		while (const char* fileName = AAssetDir_getNextFileName(directory))
		{
			const std::string childAssetPath = assetPath.empty() ? fileName : assetPath + "/" + fileName;
			const std::filesystem::path childDestination = destination / fileName;
			if (!CopyAssetFile(assetManager, childAssetPath, childDestination))
				CopyAssetTree(assetManager, childAssetPath, childDestination);
		}

		AAssetDir_close(directory);
	}

	std::filesystem::path PrepareRuntimeFiles(android_app* app)
	{
		const char* dataPath =
			(app && app->activity && app->activity->internalDataPath) ?
			app->activity->internalDataPath :
			"/data/local/tmp";
		const std::filesystem::path runtimeRoot = std::filesystem::path(dataPath) / "CoronaRuntime";
		std::error_code ec;
		std::filesystem::create_directories(runtimeRoot, ec);

		if (app && app->activity && app->activity->assetManager)
		{
			// AAssetDir_getNextFileName only enumerates files in a single
			// directory, not subdirectories. The packaging script writes
			// asset_manifest.txt with the full relative path of every
			// runtime asset, so we walk that explicit list instead of
			// trying to recurse blindly.
			AAssetManager* assetManager = app->activity->assetManager;
			const std::string manifestAssetPath = std::string(kAssetRoot) + "/asset_manifest.txt";
			AAsset* manifestAsset = AAssetManager_open(assetManager, manifestAssetPath.c_str(), AASSET_MODE_BUFFER);
			if (manifestAsset)
			{
				const off_t manifestSize = AAsset_getLength(manifestAsset);
				std::string manifestText;
				if (manifestSize > 0)
				{
					manifestText.resize(static_cast<size_t>(manifestSize));
					AAsset_read(manifestAsset, manifestText.data(), static_cast<size_t>(manifestSize));
				}
				AAsset_close(manifestAsset);

				size_t lineStart = 0;
				while (lineStart < manifestText.size())
				{
					size_t lineEnd = manifestText.find('\n', lineStart);
					if (lineEnd == std::string::npos)
						lineEnd = manifestText.size();
					std::string relPath = manifestText.substr(lineStart, lineEnd - lineStart);
					lineStart = lineEnd + 1;

					while (!relPath.empty() && (relPath.back() == '\r' || relPath.back() == ' ' || relPath.back() == '\t'))
						relPath.pop_back();
					if (relPath.empty())
						continue;

					const std::string assetPath = std::string(kAssetRoot) + "/" + relPath;
					const std::filesystem::path destination = runtimeRoot / relPath;
					std::filesystem::create_directories(destination.parent_path(), ec);
					CopyAssetFile(assetManager, assetPath, destination);
				}
			}
			else
			{
				// Fallback for older packages without a manifest — copy the
				// historically-known trees so the app still launches with
				// some content.
				CopyAssetTree(assetManager, kAssetRoot, runtimeRoot);

				const struct RuntimeAssetTree
				{
					const char* AssetPath;
					const char* DestinationPath;
				} runtimeAssetTrees[] =
				{
					{ "VulkanShaders", "VulkanShaders" },
					{ "assets/default", "assets/default" },
					{ "src/scripts/entity", "src/scripts/entity" },
					{ "src/scripts/startup", "src/scripts/startup" }
				};

				for (const RuntimeAssetTree& tree : runtimeAssetTrees)
					CopyAssetTree(
						assetManager,
						std::string(kAssetRoot) + "/" + tree.AssetPath,
						runtimeRoot / tree.DestinationPath);
			}
		}

		setenv("CORONA_RUNTIME_ROOT", runtimeRoot.string().c_str(), 1);
		chdir(runtimeRoot.string().c_str());
		return runtimeRoot;
	}

	void ShutdownCorona()
	{
		if (!gCoronaApp)
			return;

		try
		{
			gCoronaApp->StopGameThread();
			gCoronaApp->OnDestroy();
		}
		catch (...)
		{
			LogError("Exception while shutting down Corona.");
		}

		gCoronaApp.reset();
		gCoronaInitialized = false;
		SetMainPlatformWindowHandle(nullptr);
	}

	void InitializeCorona(android_app* app)
	{
		if (!app || !app->window || gCoronaInitialized)
			return;

		RequestLandscapeOrientation(app);
		const int32_t rawWidth = ANativeWindow_getWidth(app->window);
		const int32_t rawHeight = ANativeWindow_getHeight(app->window);
		if (rawWidth > 0 && rawHeight > 0 && rawHeight > rawWidth)
		{
			if (!gLoggedWaitingForLandscapeWindow)
			{
				__android_log_print(ANDROID_LOG_INFO, kLogTag, "Waiting for landscape window: %dx%d", rawWidth, rawHeight);
				gLoggedWaitingForLandscapeWindow = true;
			}
			return;
		}
		gLoggedWaitingForLandscapeWindow = false;

		SetMainPlatformWindowHandle(app->window);
		const std::filesystem::path runtimeRoot = PrepareRuntimeFiles(app);
		__android_log_print(ANDROID_LOG_INFO, kLogTag, "Runtime root: %s", runtimeRoot.string().c_str());

		const int32_t width = std::max(rawWidth, 640);
		const int32_t height = std::max(rawHeight, 360);

		gCoronaApp = std::make_unique<Corona>(
			static_cast<UINT>(width),
			static_cast<UINT>(height),
			L"Corona Android");

		wchar_t arg0[] = L"Corona";
		wchar_t* argv[] = { arg0 };
		gCoronaApp->ParseCommandLineArgs(argv, static_cast<int>(_countof(argv)));
		const double targetFrameRate = gCoronaApp->GetTargetFrameRateLimitHz();
		RequestPreferredFrameRate(app, targetFrameRate > 0.0 ? static_cast<float>(targetFrameRate) : 120.0f);
		gCoronaApp->OnInit();
		gCoronaInitialized = true;
		gCoronaApp->StartGameThread();
		LogInfo("Corona initialized.");
	}

	void HandleAppCommand(android_app* app, int32_t command)
	{
		switch (command)
		{
		case APP_CMD_INIT_WINDOW:
		case APP_CMD_CONFIG_CHANGED:
		case APP_CMD_WINDOW_RESIZED:
		case APP_CMD_CONTENT_RECT_CHANGED:
			try
			{
				InitializeCorona(app);
			}
			catch (const std::exception& exception)
			{
				LogError(exception.what());
				if (app && app->activity)
					ANativeActivity_finish(app->activity);
			}
			catch (...)
			{
				LogError("Unknown exception while initializing Corona.");
				if (app && app->activity)
					ANativeActivity_finish(app->activity);
			}
			break;

		case APP_CMD_TERM_WINDOW:
			ShutdownCorona();
			break;

		case APP_CMD_DESTROY:
			ShutdownCorona();
			break;

		default:
			break;
		}
	}

	int32_t HandleInputEvent(android_app* app, AInputEvent* inputEvent)
	{
		(void)app;
		return HandlePlatformInputEvent(inputEvent);
	}
}

void android_main(android_app* app)
{
	app_dummy();
	if (!app)
		return;

	app->onAppCmd = HandleAppCommand;
	app->onInputEvent = HandleInputEvent;
	RequestLandscapeOrientation(app);
	LogInfo("android_main started.");

	constexpr auto kAndroidDefaultTargetFrameInterval = std::chrono::microseconds(8333);

	for (;;)
	{
		int events = 0;
		android_poll_source* source = nullptr;
		const int timeoutMs = (gCoronaInitialized && app->window) ? 0 : -1;

		while (ALooper_pollOnce(timeoutMs, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0)
		{
			if (source)
				source->process(app, source);
			if (app->destroyRequested)
			{
				ShutdownCorona();
				return;
			}
			if (gCoronaInitialized && app->window)
				break;
			if (timeoutMs == 0)
				break;
		}

		if (gCoronaInitialized && gCoronaApp && app->window)
		{
			try
			{
				const auto frameStart = std::chrono::steady_clock::now();
				gCoronaApp->RenderThreadTick();
				const auto frameElapsed = std::chrono::steady_clock::now() - frameStart;
				const auto targetFrameInterval = GetFrameRateLimitInterval(gCoronaApp.get());
				const auto effectiveFrameInterval =
					targetFrameInterval > std::chrono::steady_clock::duration::zero() ?
					targetFrameInterval :
					std::chrono::duration_cast<std::chrono::steady_clock::duration>(kAndroidDefaultTargetFrameInterval);
				if (frameElapsed < effectiveFrameInterval)
					std::this_thread::sleep_for(effectiveFrameInterval - frameElapsed);
			}
			catch (const std::exception& exception)
			{
				LogError(exception.what());
				ANativeActivity_finish(app->activity);
			}
			catch (...)
			{
				LogError("Unknown exception while rendering Corona.");
				ANativeActivity_finish(app->activity);
			}
		}
		else
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(16));
		}
	}
}

#endif
