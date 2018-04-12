#include "ZipUtilityPrivatePCH.h"

#include "ZipFileFunctionLibrary.h"
#include "ZipFileFunctionInternalCallback.h"
#include "ListCallback.h"
#include "ProgressCallback.h"
#include "IPluginManager.h"
#include "WFULambdaRunnable.h"
#include "ZULambdaDelegate.h"
#include "SevenZipCallbackHandler.h"

#include "7zpp.h"

using namespace SevenZip;

//Private Namespace
namespace{

	//Threaded Lambda convenience wrappers - Task graph is only suitable for short duration lambdas, but doesn't incur thread overhead
	FGraphEventRef RunLambdaOnAnyThread(TFunction< void()> InFunction)
	{
		return FFunctionGraphTask::CreateAndDispatchWhenReady(InFunction, TStatId(), nullptr, ENamedThreads::AnyThread);
	}

	//Uses proper threading, for any task that may run longer than about 2 seconds.
	void RunLongLambdaOnAnyThread(TFunction< void()> InFunction)
	{
		WFULambdaRunnable::RunLambdaOnBackGroundThread(InFunction);
	}

	// Run the lambda on the queued threadpool
	IQueuedWork* RunLambdaOnThreadPool(TFunction< void()> InFunction)
	{
		return WFULambdaRunnable::AddLambdaToQueue(InFunction);
	}

	//Private static vars
	SevenZipLibrary SZLib;

	//Utility functions
	FString PluginRootFolder()
	{
		return IPluginManager::Get().FindPlugin("ZipUtility")->GetBaseDir();
		//return FPaths::ConvertRelativePathToFull(FPaths::GameDir());
	}

	FString DLLPath()
	{
#if _WIN64

		FString PlatformString = FString(TEXT("Win64"));
#else
		FString PlatformString = FString(TEXT("Win32"));
#endif
		//Swap these to change which license you wish to fall under for zip-utility

		FString dllString = FString("7z.dll");		//Using 7z.dll: GNU LGPL + unRAR restriction
		//FString dllString = FString("7za.dll");	//Using 7za.dll: GNU LGPL license, crucially doesn't support .zip out of the box

		return FPaths::ConvertRelativePathToFull(FPaths::Combine(*PluginRootFolder(), TEXT("ThirdParty/7zpp/dll"), *PlatformString, *dllString));
	}

	FString ReversePathSlashes(FString forwardPath)
	{
		return forwardPath.Replace(TEXT("/"), TEXT("\\"));
	}

	bool isValidDirectory(FString& directory, FString& fileName, const FString& path)
	{
		bool found = path.Split(TEXT("/"), &directory, &fileName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		//try a back split
		if (!found)
		{
			found = path.Split(TEXT("\\"), &directory, &fileName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		}

		//No valid directory found
		if (!found)
			return false;
		else
			return true;
	}

	SevenZip::CompressionLevelEnum libZipLevelFromUELevel(ZipUtilityCompressionLevel ueLevel) {
		switch (ueLevel)
		{
		case COMPRESSION_LEVEL_NONE:
			return SevenZip::CompressionLevel::None;
		case COMPRESSION_LEVEL_FAST:
			return SevenZip::CompressionLevel::Fast;
		case COMPRESSION_LEVEL_NORMAL:
			return SevenZip::CompressionLevel::Normal;
		default:
			return SevenZip::CompressionLevel::None;
		}
	}

	SevenZip::CompressionFormatEnum libZipFormatFromUEFormat(ZipUtilityCompressionFormat ueFormat) {
		switch (ueFormat)
		{
		case COMPRESSION_FORMAT_UNKNOWN:
			return CompressionFormat::Unknown;
		case COMPRESSION_FORMAT_SEVEN_ZIP:
			return CompressionFormat::SevenZip;
		case COMPRESSION_FORMAT_ZIP:
			return CompressionFormat::Zip;
		case COMPRESSION_FORMAT_GZIP:
			return CompressionFormat::GZip;
		case COMPRESSION_FORMAT_BZIP2:
			return CompressionFormat::BZip2;
		case COMPRESSION_FORMAT_RAR:
			return CompressionFormat::Rar;
		case COMPRESSION_FORMAT_TAR:
			return CompressionFormat::Tar;
		case COMPRESSION_FORMAT_ISO:
			return CompressionFormat::Iso;
		case COMPRESSION_FORMAT_CAB:
			return CompressionFormat::Cab;
		case COMPRESSION_FORMAT_LZMA:
			return CompressionFormat::Lzma;
		case COMPRESSION_FORMAT_LZMA86:
			return CompressionFormat::Lzma86;
		default:
			return CompressionFormat::Unknown;
		}
	}

	FString defaultExtensionFromUEFormat(ZipUtilityCompressionFormat ueFormat) 
	{
		switch (ueFormat)
		{
		case COMPRESSION_FORMAT_UNKNOWN:
			return FString(TEXT(".dat"));
		case COMPRESSION_FORMAT_SEVEN_ZIP:
			return FString(TEXT(".7z"));
		case COMPRESSION_FORMAT_ZIP:
			return FString(TEXT(".zip"));
		case COMPRESSION_FORMAT_GZIP:
			return FString(TEXT(".gz"));
		case COMPRESSION_FORMAT_BZIP2:
			return FString(TEXT(".bz2"));
		case COMPRESSION_FORMAT_RAR:
			return FString(TEXT(".rar"));
		case COMPRESSION_FORMAT_TAR:
			return FString(TEXT(".tar"));
		case COMPRESSION_FORMAT_ISO:
			return FString(TEXT(".iso"));
		case COMPRESSION_FORMAT_CAB:
			return FString(TEXT(".cab"));
		case COMPRESSION_FORMAT_LZMA:
			return FString(TEXT(".lzma"));
		case COMPRESSION_FORMAT_LZMA86:
			return FString(TEXT(".lzma86"));
		default:
			return FString(TEXT(".dat"));
		}
	}

	using namespace std;

	

	//Background Thread convenience functions
	UZipOperation* UnzipFilesOnBGThreadWithFormat(const TArray<int32> fileIndices, const FString& archivePath, const FString& destinationDirectory, const UObject* progressDelegate, ZipUtilityCompressionFormat format)
	{
		UZipOperation* ZipOperation = NewObject<UZipOperation>();

		IQueuedWork* Work = RunLambdaOnThreadPool([progressDelegate, fileIndices, archivePath, destinationDirectory, format, ZipOperation] 
		{
			SevenZipCallbackHandler PrivateCallback;
			PrivateCallback.progressDelegate = (UObject*)progressDelegate;
			ZipOperation->SetCallbackHandler(&PrivateCallback);

			//UE_LOG(LogClass, Log, TEXT("path is: %s"), *path);
			SevenZipExtractor extractor(SZLib, *archivePath);

			if (format == COMPRESSION_FORMAT_UNKNOWN) 
			{
				if (!extractor.DetectCompressionFormat())
				{
					extractor.SetCompressionFormat(SevenZip::CompressionFormat::Zip);
				}
			}
			else
			{
				extractor.SetCompressionFormat(libZipFormatFromUEFormat(format));
			}

			// Extract indices
			const int32 numberFiles = fileIndices.Num(); 
			unsigned int* indices = new unsigned int[numberFiles]; 

			for (int32 idx = 0; idx < numberFiles; idx++)
			{
				indices[idx] = fileIndices[idx]; 
			}
			
			// Perform the extraction
			extractor.ExtractFilesFromArchive(indices, numberFiles, *destinationDirectory, &PrivateCallback);

			// Clean up the indices
			delete indices;

			// Null out the callback handler now that we're exiting
			ZipOperation->SetCallbackHandler(nullptr);
		});

		ZipOperation->SetThreadPoolWorker(Work);
		return ZipOperation;
	}

	//Background Thread convenience functions
	UZipOperation* UnzipOnBGThreadWithFormat(const FString& archivePath, const FString& destinationDirectory, const UObject* progressDelegate, ZipUtilityCompressionFormat format)
	{
		UZipOperation* ZipOperation = NewObject<UZipOperation>();

		IQueuedWork* Work = RunLambdaOnThreadPool([progressDelegate, archivePath, destinationDirectory, format, ZipOperation] 
		{
			SevenZipCallbackHandler PrivateCallback;
			PrivateCallback.progressDelegate = (UObject*)progressDelegate;
			ZipOperation->SetCallbackHandler(&PrivateCallback);

			//UE_LOG(LogClass, Log, TEXT("path is: %s"), *path);
			SevenZipExtractor extractor(SZLib, *archivePath);

			if (format == COMPRESSION_FORMAT_UNKNOWN) 
			{
				if (!extractor.DetectCompressionFormat())
				{
					extractor.SetCompressionFormat(SevenZip::CompressionFormat::Zip);
				}
			}
			else
			{
				extractor.SetCompressionFormat(libZipFormatFromUEFormat(format));
			}

			extractor.ExtractArchive(*destinationDirectory, &PrivateCallback);

			// Null out the callback handler now that we're exiting
			ZipOperation->SetCallbackHandler(nullptr);
		});

		ZipOperation->SetThreadPoolWorker(Work);
		return ZipOperation;
	}

	void ListOnBGThread(const FString& path, const FString& directory, const UObject* listDelegate, ZipUtilityCompressionFormat format)
	{
		//RunLongLambdaOnAnyThread - this shouldn't take long, but if it lags, swap the lambda methods
		RunLambdaOnAnyThread([listDelegate, path, format, directory] {
			SevenZipCallbackHandler PrivateCallback;
			PrivateCallback.progressDelegate = (UObject*)listDelegate;
			SevenZipLister lister(SZLib, *path);

			if (format == COMPRESSION_FORMAT_UNKNOWN) 
			{
				if (!lister.DetectCompressionFormat())
				{
					lister.SetCompressionFormat(SevenZip::CompressionFormat::Zip);
				}
			}
			else
			{
				lister.SetCompressionFormat(libZipFormatFromUEFormat(format));
			}

			if (!lister.ListArchive(&PrivateCallback))
			{
				// If ListArchive returned false, it was most likely because the compression format was unsupported
				// Call OnDone with a failure message, make sure to call this on the game thread.
				if (IZipUtilityInterface* ZipInterface = Cast<IZipUtilityInterface>((UObject*)listDelegate))
				{
					UE_LOG(LogClass, Warning, TEXT("ZipUtility: Unknown failure for list operation on %s"), *path);
					UZipFileFunctionLibrary::RunLambdaOnGameThread([ZipInterface, listDelegate, path] 
					{
						ZipInterface->Execute_OnDone((UObject*)listDelegate, *path, EZipUtilityCompletionState::FAILURE_UNKNOWN);
					});
				}
			}
		});
	}

	UZipOperation* ZipOnBGThread(const FString& path, const FString& fileName, const FString& directory, const UObject* progressDelegate, ZipUtilityCompressionFormat ueCompressionformat, ZipUtilityCompressionLevel ueCompressionlevel)
	{
		UZipOperation* ZipOperation = NewObject<UZipOperation>();

		IQueuedWork* Work = RunLambdaOnThreadPool([progressDelegate, fileName, path, ueCompressionformat, ueCompressionlevel, directory, ZipOperation] 
		{
			SevenZipCallbackHandler PrivateCallback;
			PrivateCallback.progressDelegate = (UObject*)progressDelegate;
			ZipOperation->SetCallbackHandler(&PrivateCallback);

			//Set the zip format
			ZipUtilityCompressionFormat ueFormat = ueCompressionformat;

			if (ueFormat == COMPRESSION_FORMAT_UNKNOWN) 
			{
				ueFormat = COMPRESSION_FORMAT_ZIP;
			}
			//Disallow creating .rar archives as per unrar restriction, this won't work anyway so redirect to 7z
			else if (ueFormat == COMPRESSION_FORMAT_RAR) 
			{
				UE_LOG(LogClass, Warning, TEXT("ZipUtility: Rar compression not supported for creating archives, re-targeting as 7z."));
				ueFormat = COMPRESSION_FORMAT_SEVEN_ZIP;
			}
			
			//concatenate the output filename
			FString outputFileName = FString::Printf(TEXT("%s/%s%s"), *directory, *fileName, *defaultExtensionFromUEFormat(ueFormat));
			//UE_LOG(LogClass, Log, TEXT("\noutputfile is: <%s>\n path is: <%s>"), *outputFileName, *path);
			
			SevenZipCompressor compressor(SZLib, *ReversePathSlashes(outputFileName));
			compressor.SetCompressionFormat(libZipFormatFromUEFormat(ueFormat));
			compressor.SetCompressionLevel(libZipLevelFromUELevel(ueCompressionlevel));

			if (PathIsDirectory(*path))
			{
				//UE_LOG(LogClass, Log, TEXT("Compressing Folder"));
				compressor.CompressDirectory(*ReversePathSlashes(path), &PrivateCallback);
			}
			else
			{
				//UE_LOG(LogClass, Log, TEXT("Compressing File"));
				compressor.CompressFile(*ReversePathSlashes(path), &PrivateCallback);
			}

			// Null out the callback handler
			ZipOperation->SetCallbackHandler(nullptr);
			//Todo: expand to support zipping up contents of current folder
			//compressor.CompressFiles(*ReversePathSlashes(path), TEXT("*"),  &PrivateCallback);
		});
		ZipOperation->SetThreadPoolWorker(Work);
		return ZipOperation;
	}

}//End private namespace

UZipFileFunctionLibrary::UZipFileFunctionLibrary(const class FObjectInitializer& PCIP)
	: Super(PCIP)
{
	UE_LOG(LogTemp, Log, TEXT("DLLPath is: %s"), *DLLPath());
	SZLib.Load(*DLLPath());
}

UZipFileFunctionLibrary::~UZipFileFunctionLibrary()
{
	SZLib.Free();
}

bool UZipFileFunctionLibrary::UnzipFileNamed(const FString& archivePath, const FString& Name, UObject* ZipUtilityInterfaceDelegate, TEnumAsByte<ZipUtilityCompressionFormat> format /*= COMPRESSION_FORMAT_UNKNOWN*/)
{	
	UZipFileFunctionInternalCallback* InternalCallback = NewObject<UZipFileFunctionInternalCallback>();
	InternalCallback->SetFlags(RF_MarkAsRootSet);
	InternalCallback->SetCallback(Name, ZipUtilityInterfaceDelegate, format);

	ListFilesInArchive(archivePath, InternalCallback, format);

	return true;
}

bool UZipFileFunctionLibrary::UnzipFileNamedTo(const FString& archivePath, const FString& Name, const FString& destinationPath, UObject* ZipUtilityInterfaceDelegate, TEnumAsByte<ZipUtilityCompressionFormat> format /*= COMPRESSION_FORMAT_UNKNOWN*/)
{
	UZipFileFunctionInternalCallback* InternalCallback = NewObject<UZipFileFunctionInternalCallback>();
	InternalCallback->SetFlags(RF_MarkAsRootSet);
	InternalCallback->SetCallback(Name, destinationPath, ZipUtilityInterfaceDelegate, format);

	ListFilesInArchive(archivePath, InternalCallback, format);

	return true;
}

UZipOperation* UZipFileFunctionLibrary::UnzipFilesTo(const TArray<int32> fileIndices, const FString & archivePath, const FString & destinationPath, UObject * ZipUtilityInterfaceDelegate, TEnumAsByte<ZipUtilityCompressionFormat> format)
{
	return UnzipFilesOnBGThreadWithFormat(fileIndices, archivePath, destinationPath, ZipUtilityInterfaceDelegate, format);
}

UZipOperation* UZipFileFunctionLibrary::UnzipFiles(const TArray<int32> fileIndices, const FString & ArchivePath, UObject * ZipUtilityInterfaceDelegate, TEnumAsByte<ZipUtilityCompressionFormat> format)
{
	FString directory;
	FString fileName;

	//Check directory validity
	if (!isValidDirectory(directory, fileName, ArchivePath))
	{
		return nullptr;
	}
		

	if (fileIndices.Num() == 0)
	{
		return nullptr;
	}

	return UnzipFilesTo(fileIndices, ArchivePath, directory, ZipUtilityInterfaceDelegate, format);
}

UZipOperation* UZipFileFunctionLibrary::Unzip(const FString& archivePath, UObject* progressDelegate, TEnumAsByte<ZipUtilityCompressionFormat> format)
{
	FString directory;
	FString fileName;

	//Check directory validity
	if (!isValidDirectory(directory, fileName, archivePath))
	{
		return nullptr;
	}

	return UnzipTo(archivePath, directory, progressDelegate, COMPRESSION_FORMAT_UNKNOWN);
}

UZipOperation* UZipFileFunctionLibrary::UnzipWithLambda(const FString& ArchivePath, TFunction<void()> OnDoneCallback, TFunction<void(float)> OnProgressCallback, TEnumAsByte<ZipUtilityCompressionFormat> format)
{
	UZULambdaDelegate* LambdaDelegate = NewObject<UZULambdaDelegate>();
	LambdaDelegate->AddToRoot();
	LambdaDelegate->SetOnDoneCallback([LambdaDelegate, OnDoneCallback]() 
	{
		OnDoneCallback();
		LambdaDelegate->RemoveFromRoot();
	});
	LambdaDelegate->SetOnProgessCallback(OnProgressCallback);

	return Unzip(ArchivePath, LambdaDelegate, format);
}


UZipOperation* UZipFileFunctionLibrary::UnzipTo(const FString& archivePath, const FString& destinationPath, UObject* ZipUtilityInterfaceDelegate, TEnumAsByte<ZipUtilityCompressionFormat> format)
{
	return UnzipOnBGThreadWithFormat(archivePath, destinationPath, ZipUtilityInterfaceDelegate, COMPRESSION_FORMAT_UNKNOWN);
}

UZipOperation* UZipFileFunctionLibrary::Zip(const FString& path, UObject* progressDelegate, TEnumAsByte<ZipUtilityCompressionFormat> Format, TEnumAsByte<ZipUtilityCompressionLevel> Level)
{
	FString directory;
	FString fileName;

	//Check directory validity
	if (!isValidDirectory(directory, fileName, path))
	{
		return nullptr;
	}

	return ZipOnBGThread(path, fileName, directory, progressDelegate, Format, Level);
}

UZipOperation* UZipFileFunctionLibrary::ZipWithLambda(const FString& ArchivePath, TFunction<void()> OnDoneCallback, TFunction<void(float)> OnProgressCallback /*= nullptr*/, TEnumAsByte<ZipUtilityCompressionFormat> Format /*= COMPRESSION_FORMAT_UNKNOWN*/, TEnumAsByte<ZipUtilityCompressionLevel> Level /*=COMPRESSION_LEVEL_NORMAL*/)
{
	UZULambdaDelegate* LambdaDelegate = NewObject<UZULambdaDelegate>();
	LambdaDelegate->AddToRoot();
	LambdaDelegate->SetOnDoneCallback([OnDoneCallback, LambdaDelegate]() 
	{
		OnDoneCallback();
		LambdaDelegate->RemoveFromRoot();
	});
	LambdaDelegate->SetOnProgessCallback(OnProgressCallback);

	return Zip(ArchivePath, LambdaDelegate, Format);
}

bool UZipFileFunctionLibrary::ListFilesInArchive(const FString& path, UObject* listDelegate, TEnumAsByte<ZipUtilityCompressionFormat> format)
{
	FString directory;
	FString fileName;

	//Check directory validity
	if (!isValidDirectory(directory, fileName, path))
	{
		return false;
	}

	ListOnBGThread(path, directory, listDelegate, format);
	return true;
}

FGraphEventRef UZipFileFunctionLibrary::RunLambdaOnGameThread(TFunction< void()> InFunction)
{
	return FFunctionGraphTask::CreateAndDispatchWhenReady(InFunction, TStatId(), nullptr, ENamedThreads::GameThread);
}
