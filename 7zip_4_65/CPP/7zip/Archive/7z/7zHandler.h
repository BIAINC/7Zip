// 7z/Handler.h

#ifndef __7Z_HANDLER_H
#define __7Z_HANDLER_H

#include <iostream>
#include <fstream>

#include "../../ICoder.h"
#include "../IArchive.h"
#include "7zIn.h"
#include "7zOut.h"

#include "7zCompressionMode.h"

#include "../../Common/CreateCoder.h"
#include "../../Common/FileStreams.h"
#include "../../Common/Update.h"

#ifndef EXTRACT_ONLY
#include "../Common/HandlerOut.h"
#endif

namespace NArchive {
namespace N7z {

#ifndef __7Z_SET_PROPERTIES

#ifdef EXTRACT_ONLY
#ifdef COMPRESS_MT
#define __7Z_SET_PROPERTIES
#endif
#else
#define __7Z_SET_PROPERTIES
#endif

#endif

struct CRecoveryIndex
{
  int lastRecoveryStartPosIndexToUpdate;
  int lastRecoveryCTimeIndexToUpdate;
  int lastRecoveryMTimeIndexToUpdate;
  int lastRecoveryATimeIndexToUpdate;
  int lastRecoveryPackCRCsDefinedIndexToUpdate;
  int lastRecoveryPackCRCsIndexToUpdate;
  int lastRecoveryFilesIndexToUpdate;
  int lastRecoveryFoldersIndexToUpdate;
  int lastRecoveryPackSizesIndexToUpdate;
  int lastRecoveryNumUnpackStreamsVectorIndexToUpdate;
  int lastRecoveryIsAntiIndexToUpdate;

  void Init()
  {
    lastRecoveryStartPosIndexToUpdate = 0;
    lastRecoveryCTimeIndexToUpdate = 0;
    lastRecoveryMTimeIndexToUpdate = 0;
    lastRecoveryATimeIndexToUpdate = 0;
    lastRecoveryPackCRCsDefinedIndexToUpdate = 0;
    lastRecoveryPackCRCsIndexToUpdate = 0;
    lastRecoveryFilesIndexToUpdate = 0;
    lastRecoveryFoldersIndexToUpdate = 0;
    lastRecoveryPackSizesIndexToUpdate = 0;
    lastRecoveryNumUnpackStreamsVectorIndexToUpdate = 0;
    lastRecoveryIsAntiIndexToUpdate = 0;
  }
};

class CHandler:
  #ifndef EXTRACT_ONLY
  public NArchive::COutHandler,
  #endif
  public IInArchive,
  #ifdef __7Z_SET_PROPERTIES
  public ISetProperties,
  #endif
  #ifndef EXTRACT_ONLY
  public IOutArchive,
  #endif
  PUBLIC_ISetCompressCodecsInfo
  public CMyUnknownImp
{
public:
  MY_QUERYINTERFACE_BEGIN2(IInArchive)
  #ifdef __7Z_SET_PROPERTIES
  MY_QUERYINTERFACE_ENTRY(ISetProperties)
  #endif
  #ifndef EXTRACT_ONLY
  MY_QUERYINTERFACE_ENTRY(IOutArchive)
  #endif
  QUERY_ENTRY_ISetCompressCodecsInfo
  MY_QUERYINTERFACE_END
  MY_ADDREF_RELEASE

  INTERFACE_IInArchive(;)

  #ifdef __7Z_SET_PROPERTIES
  STDMETHOD(SetProperties)(const wchar_t **names, const PROPVARIANT *values, Int32 numProperties);
  #endif

  #ifndef EXTRACT_ONLY
  INTERFACE_IOutArchive(;)
  #endif

  HRESULT MoveItemToTrash(UString &path);
  HRESULT OpenWithRecoveryData(UString& recoveryFileName,
    COutMultiVolStream *outStream,
    CObjectVector<UString> &filterDirs,
	UString& itemStatFilter,
	UString& cocEntryFilter);
  HRESULT SetRecoveryOption(UString& recoveryFileName);
  unsigned long long GetFileCount();
  unsigned long long GetTotalPackSize();
  unsigned long long GetRecoveredFileCount();
  unsigned long long GetRecoveredUncompressedFileSize();

  DECL_ISetCompressCodecsInfo

  CHandler();

private:
  HRESULT UpdateRecoveryData();

  void ReadRecoveryStartPosData(std::ifstream& recoveryStream, CUInt64DefVector& startPos);
  void ReadRecoveryCTimeData(std::ifstream& recoveryStream, CUInt64DefVector& cTime);
  void ReadRecoveryMTimeData(std::ifstream& recoveryStream, CUInt64DefVector& mTime);
  void ReadRecoveryATimeData(std::ifstream& recoveryStream, CUInt64DefVector& aTime);
  void ReadRecoveryPackCRCsDefinedData(std::ifstream& recoveryStream, CRecordVector<bool>& packCRCsDefined);
  void ReadRecoveryPackCRCsData(std::ifstream& recoveryStream, CRecordVector<UInt32>& packCRCs);
  void ReadRecoveryPackSizesData(std::ifstream& recoveryStream, CRecordVector<UInt64>& packSizes);
  void ReadRecoveryNumUnpackStreamsVectorData(std::ifstream& recoveryStream, CRecordVector<CNum>& numUnpackStreamsVector);
  void ReadRecoveryIsAntiData(std::ifstream& recoveryStream, CRecordVector<bool>& isAnti);
  void ReadRecoveryFilesData(std::ifstream& recoveryStream, CObjectVector<CFileItem>& files);
  void ReadRecoveryFoldersData(std::ifstream& recoveryStream, CObjectVector<CFolder>& folders);

  void WriteRecoveryStartPosData();
  void WriteRecoveryCTimeData();
  void WriteRecoveryMTimeData();
  void WriteRecoveryATimeData();
  void WriteRecoveryPackCRCsDefinedData();
  void WriteRecoveryPackCRCsData();
  void WriteRecoveryPackSizesData();
  void WriteRecoveryNumUnpackStreamsVectorData();
  void WriteRecoveryIsAntiData();
  void WriteRecoveryFilesData();
  void WriteRecoveryFoldersData();

  bool IsItemFiltered(UString& item, CObjectVector<UString> &filterDirs);

  CMyComPtr<IInStream> _inStream;
  NArchive::N7z::CArchiveDatabaseEx _db;
  NArchive::N7z::CArchiveDatabase _newDB;
  NArchive::N7z::COutArchive _archive;

  unsigned long long _totalPackSize;

  unsigned long long _recoveredFileCount;
  unsigned long long _recoveredUncompressedFileSize;

  UString _recoveryFileName;
  std::fstream _recoveryStreamOut;

  CRecoveryIndex _recoveryIndex;

  #ifndef _NO_CRYPTO
  bool _passwordIsDefined;
  #endif

  #ifdef EXTRACT_ONLY
  
  #ifdef COMPRESS_MT
  UInt32 _numThreads;
  #endif

  UInt32 _crcSize;

  #else
  
  CRecordVector<CBind> _binds;

  HRESULT SetPassword(CCompressionMethodMode &methodMode, IArchiveUpdateCallback *updateCallback);

  HRESULT SetCompressionMethod(CCompressionMethodMode &method,
      CObjectVector<COneMethodInfo> &methodsInfo
      #ifdef COMPRESS_MT
      , UInt32 numThreads
      #endif
      );

  HRESULT SetCompressionMethod(
      CCompressionMethodMode &method,
      CCompressionMethodMode &headerMethod);

  #endif

  bool IsEncrypted(UInt32 index2) const;
  #ifndef _SFX

  CRecordVector<UInt64> _fileInfoPopIDs;
  void FillPopIDs();

  #endif

  DECL_EXTERNAL_CODECS_VARS
};

}}

#endif
