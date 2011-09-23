// 7zHandlerOut.cpp

#include "StdAfx.h"

#include "../../../Windows/PropVariant.h"

#include "../../../Common/ComTry.h"
#include "../../../Common/StringToInt.h"

#include "../../ICoder.h"

#include "../Common/ItemNameUtils.h"
#include "../Common/ParseProperties.h"

#include "7zHandler.h"
#include "7zOut.h"
#include "7zUpdate.h"

using namespace NWindows;

namespace NArchive {
namespace N7z {

static const wchar_t *kLZMAMethodName = L"LZMA";
static const wchar_t *kCopyMethod = L"Copy";
static const wchar_t *kDefaultMethodName = kLZMAMethodName;

static const wchar_t *kTrashFolderName = L"Trash/";
static const wchar_t *kRecoverySignature = L"D7ZR";

static const UInt32 kLzmaAlgorithmX5 = 1;
static const wchar_t *kLzmaMatchFinderForHeaders = L"BT2";
static const UInt32 kDictionaryForHeaders = 1 << 20;
static const UInt32 kNumFastBytesForHeaders = 273;
static const UInt32 kAlgorithmForHeaders = kLzmaAlgorithmX5;

static inline bool IsCopyMethod(const UString &methodName)
  { return (methodName.CompareNoCase(kCopyMethod) == 0); }

STDMETHODIMP CHandler::GetFileTimeType(UInt32 *type)
{
  *type = NFileTimeType::kWindows;
  return S_OK;
}

HRESULT CHandler::SetPassword(CCompressionMethodMode &methodMode,
    IArchiveUpdateCallback *updateCallback)
{
  CMyComPtr<ICryptoGetTextPassword2> getTextPassword;
  if (!getTextPassword)
  {
    CMyComPtr<IArchiveUpdateCallback> udateCallback2(updateCallback);
    udateCallback2.QueryInterface(IID_ICryptoGetTextPassword2, &getTextPassword);
  }
  
  if (getTextPassword)
  {
    CMyComBSTR password;
    Int32 passwordIsDefined;
    RINOK(getTextPassword->CryptoGetTextPassword2(
        &passwordIsDefined, &password));
    methodMode.PasswordIsDefined = IntToBool(passwordIsDefined);
    if (methodMode.PasswordIsDefined)
      methodMode.Password = password;
  }
  else
    methodMode.PasswordIsDefined = false;
  return S_OK;
}

HRESULT CHandler::SetCompressionMethod(
    CCompressionMethodMode &methodMode,
    CCompressionMethodMode &headerMethod)
{
  HRESULT res = SetCompressionMethod(methodMode, _methods
  #ifdef COMPRESS_MT
  , _numThreads
  #endif
  );
  RINOK(res);
  methodMode.Binds = _binds;

  if (_compressHeaders)
  {
    // headerMethod.Methods.Add(methodMode.Methods.Back());

    CObjectVector<COneMethodInfo> headerMethodInfoVector;
    COneMethodInfo oneMethodInfo;
    oneMethodInfo.MethodName = kLZMAMethodName;
    {
      CProp prop;
      prop.Id = NCoderPropID::kMatchFinder;
      prop.Value = kLzmaMatchFinderForHeaders;
      oneMethodInfo.Props.Add(prop);
    }
    {
      CProp prop;
      prop.Id = NCoderPropID::kAlgorithm;
      prop.Value = kAlgorithmForHeaders;
      oneMethodInfo.Props.Add(prop);
    }
    {
      CProp prop;
      prop.Id = NCoderPropID::kNumFastBytes;
      prop.Value = (UInt32)kNumFastBytesForHeaders;
      oneMethodInfo.Props.Add(prop);
    }
    {
      CProp prop;
      prop.Id = NCoderPropID::kDictionarySize;
      prop.Value = (UInt32)kDictionaryForHeaders;
      oneMethodInfo.Props.Add(prop);
    }
    headerMethodInfoVector.Add(oneMethodInfo);
    HRESULT res = SetCompressionMethod(headerMethod, headerMethodInfoVector
      #ifdef COMPRESS_MT
      ,1
      #endif
    );
    RINOK(res);
  }
  return S_OK;
}

HRESULT CHandler::SetCompressionMethod(
    CCompressionMethodMode &methodMode,
    CObjectVector<COneMethodInfo> &methodsInfo
    #ifdef COMPRESS_MT
    , UInt32 numThreads
    #endif
    )
{
  UInt32 level = _level;
  
  if (methodsInfo.IsEmpty())
  {
    COneMethodInfo oneMethodInfo;
    oneMethodInfo.MethodName = ((level == 0) ? kCopyMethod : kDefaultMethodName);
    methodsInfo.Add(oneMethodInfo);
  }

  bool needSolid = false;
  for(int i = 0; i < methodsInfo.Size(); i++)
  {
    COneMethodInfo &oneMethodInfo = methodsInfo[i];
    SetCompressionMethod2(oneMethodInfo
      #ifdef COMPRESS_MT
      , numThreads
      #endif
      );

    if (!IsCopyMethod(oneMethodInfo.MethodName))
      needSolid = true;

    CMethodFull methodFull;

    if (!FindMethod(
        EXTERNAL_CODECS_VARS
        oneMethodInfo.MethodName, methodFull.Id, methodFull.NumInStreams, methodFull.NumOutStreams))
      return E_INVALIDARG;
    methodFull.Props = oneMethodInfo.Props;
    methodMode.Methods.Add(methodFull);

    if (!_numSolidBytesDefined)
    {
      for (int j = 0; j < methodFull.Props.Size(); j++)
      {
        const CProp &prop = methodFull.Props[j];
        if ((prop.Id == NCoderPropID::kDictionarySize ||
             prop.Id == NCoderPropID::kUsedMemorySize) && prop.Value.vt == VT_UI4)
        {
          _numSolidBytes = ((UInt64)prop.Value.ulVal) << 7;
          const UInt64 kMinSize = (1 << 24);
          if (_numSolidBytes < kMinSize)
            _numSolidBytes = kMinSize;
          _numSolidBytesDefined = true;
          break;
        }
      }
    }
  }

  if (!needSolid && !_numSolidBytesDefined)
  {
    _numSolidBytesDefined = true;
    _numSolidBytes  = 0;
  }
  return S_OK;
}

static HRESULT GetTime(IArchiveUpdateCallback *updateCallback, int index, bool writeTime, PROPID propID, UInt64 &ft, bool &ftDefined)
{
  ft = 0;
  ftDefined = false;
  if (!writeTime)
    return S_OK;
  NCOM::CPropVariant prop;
  RINOK(updateCallback->GetProperty(index, propID, &prop));
  if (prop.vt == VT_FILETIME)
  {
    ft = prop.filetime.dwLowDateTime | ((UInt64)prop.filetime.dwHighDateTime << 32);
    ftDefined = true;
  }
  else if (prop.vt != VT_EMPTY)
    return E_INVALIDARG;
  return S_OK;
}

HRESULT CHandler::MoveItemToTrash(UString &path)
{
  COM_TRY_BEGIN

  if (!_recoveryStreamOut.is_open())
    return E_FAIL;

  for (int i = 0; i < _newDB.Files.Size(); i++)
  {
    if (path == _newDB.Files[i].Name)
	{
      if(_newDB.Files.Size() > i)
	  {
        // Rename the folder path
	    _newDB.Files[i].Name = kTrashFolderName + path;

        // Erase recovery record starting from this position
        Int64 currentEndOfRecoveryRecord = _recoveryStreamOut.tellp();
        // Erase everything from this point on
        if (currentEndOfRecoveryRecord > _newDB.Files[i].RecoveryRecordPos
            && _newDB.Files[i].RecoveryRecordPos > 0)
        {
          _recoveryStreamOut.seekp(_newDB.Files[i].RecoveryRecordPos);
          size_t emptySize = (size_t)(currentEndOfRecoveryRecord - _newDB.Files[i].RecoveryRecordPos);
          char* emptyChars = (char*)malloc(emptySize);
          memset(emptyChars, 0, emptySize);
          _recoveryStreamOut.write(emptyChars, emptySize);
          _recoveryStreamOut.flush();
          _recoveryStreamOut.seekp(_newDB.Files[i].RecoveryRecordPos);
        }
	  }

      return S_OK;
	}
  }
  return S_FALSE;
  COM_TRY_END
}

bool CHandler::IsItemFiltered(UString& item, CObjectVector<UString> &filterDirs)
{
  for (int filterIndex = 0; filterIndex < filterDirs.Size(); ++filterIndex)
  {
    if (item.Left(filterDirs[filterIndex].Length()) == filterDirs[filterIndex])
      return true;
  }

  return false;
}

void CHandler::ReadRecoveryStartPosData(std::ifstream& recoveryStream, CUInt64DefVector& startPos)
{
  int startPosSize = 0;
  recoveryStream.read(reinterpret_cast <char*> (&startPosSize), sizeof(startPosSize));
  for (int currStartPos = 0; currStartPos < startPosSize; ++currStartPos)
  {
    bool startPosDefined;
    UInt64 startPosValue;

    recoveryStream.read(reinterpret_cast <char*> (&startPosDefined), sizeof(startPosDefined));
    recoveryStream.read(reinterpret_cast <char*> (&startPosValue), sizeof(startPosValue));

    startPos.ReserveDown();
    startPos.SetItem(currStartPos, startPosDefined, startPosValue);
  }
}

void CHandler::ReadRecoveryCTimeData(std::ifstream& recoveryStream, CUInt64DefVector& cTime)
{
  int cTimeSize = 0;
  recoveryStream.read(reinterpret_cast <char*> (&cTimeSize), sizeof(cTimeSize));
  for (int currCTime = 0; currCTime < cTimeSize; ++currCTime)
  {
    bool cTimeDefined;
    UInt64 cTimeValue;

    recoveryStream.read(reinterpret_cast <char*> (&cTimeDefined), sizeof(cTimeDefined));
    recoveryStream.read(reinterpret_cast <char*> (&cTimeValue), sizeof(cTimeValue));

    cTime.ReserveDown();
    cTime.SetItem(currCTime, cTimeDefined, cTimeValue);
  }
}

void CHandler::ReadRecoveryMTimeData(std::ifstream& recoveryStream, CUInt64DefVector& mTime)
{
  int mTimeSize = 0;
  recoveryStream.read(reinterpret_cast <char*> (&mTimeSize), sizeof(mTimeSize));
  for (int currMTime = 0; currMTime < mTimeSize; ++currMTime)
  {
    bool mTimeDefined;
    UInt64 mTimeValue;

    recoveryStream.read(reinterpret_cast <char*> (&mTimeDefined), sizeof(mTimeDefined));
    recoveryStream.read(reinterpret_cast <char*> (&mTimeValue), sizeof(mTimeValue));

    mTime.ReserveDown();
    mTime.SetItem(currMTime, mTimeDefined, mTimeValue);
  }
}

void CHandler::ReadRecoveryATimeData(std::ifstream& recoveryStream, CUInt64DefVector& aTime)
{
  int aTimeSize = 0;
  recoveryStream.read(reinterpret_cast <char*> (&aTimeSize), sizeof(aTimeSize));
  for (int currATime = 0; currATime < aTimeSize; ++currATime)
  {
    bool aTimeDefined;
    UInt64 aTimeValue;

    recoveryStream.read(reinterpret_cast <char*> (&aTimeDefined), sizeof(aTimeDefined));
    recoveryStream.read(reinterpret_cast <char*> (&aTimeValue), sizeof(aTimeValue));

    aTime.ReserveDown();
    aTime.SetItem(currATime, aTimeDefined, aTimeValue);
  }
}

void CHandler::ReadRecoveryPackCRCsDefinedData(std::ifstream& recoveryStream, CRecordVector<bool>& packCRCsDefined)
{
  int packCRCsDefinedSize = 0;
  recoveryStream.read(reinterpret_cast <char*> (&packCRCsDefinedSize), sizeof(packCRCsDefinedSize));
  for (int currPackCRCsDefined = 0; currPackCRCsDefined < packCRCsDefinedSize; ++currPackCRCsDefined)
  {
    bool packCRCsDefinedValue;

    recoveryStream.read(reinterpret_cast <char*> (&packCRCsDefinedValue), sizeof(packCRCsDefinedValue));

    packCRCsDefined.Add(packCRCsDefinedValue);
  }
}

void CHandler::ReadRecoveryPackCRCsData(std::ifstream& recoveryStream, CRecordVector<UInt32>& packCRCs)
{
  int packCRCsSize = 0;
  recoveryStream.read(reinterpret_cast <char*> (&packCRCsSize), sizeof(packCRCsSize));
  for (int currPackCRCs = 0; currPackCRCs < packCRCsSize; ++currPackCRCs)
  {
    UInt32 packCRCsValue;

    recoveryStream.read(reinterpret_cast <char*> (&packCRCsValue), sizeof(packCRCsValue));

    packCRCs.Add(packCRCsValue);
  }
}

void CHandler::ReadRecoveryPackSizesData(std::ifstream& recoveryStream, CRecordVector<UInt64>& packSizes)
{
  int packSizesSize = 0;
  recoveryStream.read(reinterpret_cast <char*> (&packSizesSize), sizeof(packSizesSize));
  for (int currPackSize = 0; currPackSize < packSizesSize; ++currPackSize)
  {
    UInt64 packSizesValue;

    recoveryStream.read(reinterpret_cast <char*> (&packSizesValue), sizeof(packSizesValue));

    packSizes.Add(packSizesValue);
  }
}

void CHandler::ReadRecoveryNumUnpackStreamsVectorData(std::ifstream& recoveryStream, CRecordVector<CNum>& numUnpackStreamsVector)
{
  int numUnpackStreamsVectorSize = 0;
  recoveryStream.read(reinterpret_cast <char*> (&numUnpackStreamsVectorSize), sizeof(numUnpackStreamsVectorSize));
  for (int currNumUnpackStreamsVector = 0; currNumUnpackStreamsVector < numUnpackStreamsVectorSize; ++currNumUnpackStreamsVector)
  {
    CNum numUnpackStreamsVectorValue;

    recoveryStream.read(reinterpret_cast <char*> (&numUnpackStreamsVectorValue), sizeof(numUnpackStreamsVectorValue));

    numUnpackStreamsVector.Add(numUnpackStreamsVectorValue);
  }
}

void CHandler::ReadRecoveryIsAntiData(std::ifstream& recoveryStream, CRecordVector<bool>& isAnti)
{
  int isAntiSize = 0;
  recoveryStream.read(reinterpret_cast <char*> (&isAntiSize), sizeof(isAntiSize));
  for (int currIsAnti = 0; currIsAnti < isAntiSize; ++currIsAnti)
  {
    bool isAntiValue;

    recoveryStream.read(reinterpret_cast <char*> (&isAntiValue), sizeof(isAntiValue));

    isAnti.Add(isAntiValue);
  }
}

void CHandler::ReadRecoveryFilesData(std::ifstream& recoveryStream, CObjectVector<CFileItem>& files)
{
  int filesSize = 0;
  recoveryStream.read(reinterpret_cast <char*> (&filesSize), sizeof(filesSize));
  for (int currFile = 0; currFile < filesSize; ++currFile)
  {
    CFileItem fileItem;
      
    //    Attrib
    recoveryStream.read(reinterpret_cast <char*> (&fileItem.Attrib), sizeof(fileItem.Attrib));
    //    AttribDefined
    recoveryStream.read(reinterpret_cast <char*> (&fileItem.AttribDefined), sizeof(fileItem.AttribDefined));
    //    Crc
    recoveryStream.read(reinterpret_cast <char*> (&fileItem.Crc), sizeof(fileItem.Crc));
    //    CrcDefined
    recoveryStream.read(reinterpret_cast <char*> (&fileItem.CrcDefined), sizeof(fileItem.CrcDefined));
    //    HasStream
    recoveryStream.read(reinterpret_cast <char*> (&fileItem.HasStream), sizeof(fileItem.HasStream));
    //    IsDir
    recoveryStream.read(reinterpret_cast <char*> (&fileItem.IsDir), sizeof(fileItem.IsDir));
    //    Length of name including null terminated character
    UInt32 nameLength = 0;
    recoveryStream.read(reinterpret_cast <char*> (&nameLength), sizeof(nameLength));
    //    Name - UString - wchar_t*
    UString name;
    wchar_t* nameBuffer = name.GetBuffer(nameLength);
    recoveryStream.read(reinterpret_cast <char*> (nameBuffer), sizeof(wchar_t) * nameLength);
    fileItem.Name = nameBuffer;
    //    Size
    recoveryStream.read(reinterpret_cast <char*> (&fileItem.Size), sizeof(fileItem.Size));

    files.Add(fileItem);
  }
}

void CHandler::ReadRecoveryFoldersData(std::ifstream& recoveryStream, CObjectVector<CFolder>& folders)
{
  int foldersSize = 0;
  recoveryStream.read(reinterpret_cast <char*> (&foldersSize), sizeof(foldersSize));
  for (int currFolder = 0; currFolder < foldersSize; ++currFolder)
  {
    CFolder folderItem;

    //     BindPairs
    // =============================================
    int bindPairsSize = 0;
    recoveryStream.read(reinterpret_cast <char*> (&bindPairsSize), sizeof(bindPairsSize));
    for(int currBindPair = 0; currBindPair < bindPairsSize; ++currBindPair)
    {
      CBindPair bindPair;
      //     InIndex
      recoveryStream.read(reinterpret_cast <char*> (&bindPair.InIndex), sizeof(bindPair.InIndex));
      //     OutIdex
      recoveryStream.read(reinterpret_cast <char*> (&bindPair.OutIndex), sizeof(bindPair.OutIndex));

      folderItem.BindPairs.Add(bindPair);
    }
    //    Coders
    // =============================================
    int codersSize = 0;
    recoveryStream.read(reinterpret_cast <char*> (&codersSize), sizeof(codersSize));
    for(int currCoder = 0; currCoder < codersSize; ++currCoder)
    {
      CCoderInfo coder;

      //      MethodID
      recoveryStream.read(reinterpret_cast <char*> (&coder.MethodID), sizeof(coder.MethodID));
      //      NumInStreams
      recoveryStream.read(reinterpret_cast <char*> (&coder.NumInStreams), sizeof(coder.NumInStreams));
      //      NumOutStreams
      recoveryStream.read(reinterpret_cast <char*> (&coder.NumOutStreams), sizeof(coder.NumOutStreams));
      //      Props
      int bufferCapacity = 0;
      recoveryStream.read(reinterpret_cast <char*> (&bufferCapacity), sizeof(bufferCapacity));
      coder.Props.SetCapacity(bufferCapacity);
      if (bufferCapacity != 0)
        recoveryStream.read(reinterpret_cast <char*> (&coder.Props[0]), bufferCapacity);

      folderItem.Coders.Add(coder);
    }
    //    PackStreams
    int packStreamsSize = 0;
    recoveryStream.read(reinterpret_cast <char*> (&packStreamsSize), sizeof(packStreamsSize));
    for(int currPackStream = 0; currPackStream < packStreamsSize; ++currPackStream)
    {
      CNum packStream;
      recoveryStream.read(reinterpret_cast <char*> (&packStream), sizeof(packStream));

      folderItem.PackStreams.Add(packStream);
    }
    //    UnpackCRC
    recoveryStream.read(reinterpret_cast <char*> (&folderItem.UnpackCRC), sizeof(folderItem.UnpackCRC));
    //    UnpackCRCDefined
    recoveryStream.read(reinterpret_cast <char*> (&folderItem.UnpackCRCDefined), sizeof(folderItem.UnpackCRCDefined));
    //    UnpackSizes
    int unpackSizesSize = 0;
    recoveryStream.read(reinterpret_cast <char*> (&unpackSizesSize), sizeof(unpackSizesSize));
    for(int currUnpackSize = 0; currUnpackSize < unpackSizesSize; ++currUnpackSize)
    {
      UInt64 unpackSize;
      recoveryStream.read(reinterpret_cast <char*> (&unpackSize), sizeof(unpackSize));

      folderItem.UnpackSizes.Add(unpackSize);
    }

    folders.Add(folderItem);
  }
}

void CHandler::WriteRecoveryStartPosData()
{
  int startPosSize = _newDB.StartPos.Defined.Size() - _recoveryIndex.lastRecoveryStartPosIndexToUpdate;
  _recoveryStreamOut.write(reinterpret_cast <const char*> (&startPosSize), sizeof(startPosSize));
  for (int currStartPos = _recoveryIndex.lastRecoveryStartPosIndexToUpdate; currStartPos < _newDB.StartPos.Defined.Size(); ++currStartPos)
  {
    _recoveryStreamOut.write(reinterpret_cast <const char*> (&_newDB.StartPos.Defined[currStartPos]), sizeof(_newDB.StartPos.Defined[currStartPos]));
    if (_newDB.StartPos.Defined[currStartPos])
    {
      _recoveryStreamOut.write(reinterpret_cast <const char*> (&_newDB.StartPos.Values[currStartPos]), sizeof(_newDB.StartPos.Values[currStartPos]));
    }
    else
    {
      UInt64 noValue = 0;
      _recoveryStreamOut.write(reinterpret_cast <const char*> (&noValue), sizeof(noValue));
    }
  }

  _recoveryIndex.lastRecoveryStartPosIndexToUpdate = _newDB.StartPos.Defined.Size();
}

void CHandler::WriteRecoveryCTimeData()
{
  int cTimeSize = _newDB.CTime.Defined.Size() - _recoveryIndex.lastRecoveryCTimeIndexToUpdate;
  _recoveryStreamOut.write(reinterpret_cast <const char*> (&cTimeSize), sizeof(cTimeSize));
  for (int currCTime = _recoveryIndex.lastRecoveryCTimeIndexToUpdate; currCTime < _newDB.CTime.Defined.Size(); ++currCTime)
  {
    _recoveryStreamOut.write(reinterpret_cast <const char*> (&_newDB.CTime.Defined[currCTime]), sizeof(_newDB.CTime.Defined[currCTime]));
    if (_newDB.CTime.Defined[currCTime])
    {
      _recoveryStreamOut.write(reinterpret_cast <const char*> (&_newDB.CTime.Values[currCTime]), sizeof(_newDB.CTime.Values[currCTime]));
    }
    else
    {
      UInt64 noValue = 0;
      _recoveryStreamOut.write(reinterpret_cast <const char*> (&noValue), sizeof(noValue));
    }
  }

  _recoveryIndex.lastRecoveryCTimeIndexToUpdate = _newDB.CTime.Defined.Size();
}

void CHandler::WriteRecoveryMTimeData()
{
  int mTimeSize = _newDB.MTime.Defined.Size() - _recoveryIndex.lastRecoveryMTimeIndexToUpdate;
  _recoveryStreamOut.write(reinterpret_cast <const char*> (&mTimeSize), sizeof(mTimeSize));
  for (int currMTime = _recoveryIndex.lastRecoveryMTimeIndexToUpdate; currMTime < _newDB.MTime.Defined.Size(); ++currMTime)
  {
    _recoveryStreamOut.write(reinterpret_cast <const char*> (&_newDB.MTime.Defined[currMTime]), sizeof(_newDB.MTime.Defined[currMTime]));
    if (_newDB.MTime.Defined[currMTime])
    {
      _recoveryStreamOut.write(reinterpret_cast <const char*> (&_newDB.MTime.Values[currMTime]), sizeof(_newDB.MTime.Values[currMTime]));
    }
    else
    {
      UInt64 noValue = 0;
      _recoveryStreamOut.write(reinterpret_cast <const char*> (&noValue), sizeof(noValue));
    }
  }

  _recoveryIndex.lastRecoveryMTimeIndexToUpdate = _newDB.MTime.Defined.Size();
}

void CHandler::WriteRecoveryATimeData()
{
  int aTimeSize = _newDB.ATime.Defined.Size() - _recoveryIndex.lastRecoveryATimeIndexToUpdate;
  _recoveryStreamOut.write(reinterpret_cast <const char*> (&aTimeSize), sizeof(aTimeSize));
  for (int currATime = _recoveryIndex.lastRecoveryATimeIndexToUpdate; currATime < _newDB.ATime.Defined.Size(); ++currATime)
  {
    _recoveryStreamOut.write(reinterpret_cast <const char*> (&_newDB.ATime.Defined[currATime]), sizeof(_newDB.ATime.Defined[currATime]));
    if (_newDB.ATime.Defined[currATime])
    {
      _recoveryStreamOut.write(reinterpret_cast <const char*> (&_newDB.ATime.Values[currATime]), sizeof(_newDB.ATime.Values[currATime]));
    }
    else
    {
      UInt64 noValue = 0;
      _recoveryStreamOut.write(reinterpret_cast <const char*> (&noValue), sizeof(noValue));
    }
  }

  _recoveryIndex.lastRecoveryATimeIndexToUpdate = _newDB.ATime.Defined.Size();
}

void CHandler::WriteRecoveryPackCRCsDefinedData()
{
  int packCRCsDefinedSize = _newDB.PackCRCsDefined.Size() - _recoveryIndex.lastRecoveryPackCRCsDefinedIndexToUpdate;
  _recoveryStreamOut.write(reinterpret_cast <const char*> (&packCRCsDefinedSize), sizeof(packCRCsDefinedSize));
  for (int currPackCRCsDefined = _recoveryIndex.lastRecoveryPackCRCsDefinedIndexToUpdate; currPackCRCsDefined < _newDB.PackCRCsDefined.Size(); ++currPackCRCsDefined)
  {
    _recoveryStreamOut.write(reinterpret_cast <const char*> (&_newDB.PackCRCsDefined[currPackCRCsDefined]), sizeof(_newDB.PackCRCsDefined[currPackCRCsDefined]));
  }

  _recoveryIndex.lastRecoveryPackCRCsDefinedIndexToUpdate = _newDB.PackCRCsDefined.Size();
}

void CHandler::WriteRecoveryPackCRCsData()
{
  int packCRCsSize = _newDB.PackCRCs.Size() - _recoveryIndex.lastRecoveryPackCRCsIndexToUpdate;
  _recoveryStreamOut.write(reinterpret_cast <const char*> (&packCRCsSize), sizeof(packCRCsSize));
  for (int currPackCRCs = _recoveryIndex.lastRecoveryPackCRCsIndexToUpdate; currPackCRCs < _newDB.PackCRCs.Size(); ++currPackCRCs)
  {
    _recoveryStreamOut.write(reinterpret_cast <const char*> (&_newDB.PackCRCs[currPackCRCs]), sizeof(_newDB.PackCRCs[currPackCRCs]));
  }

  _recoveryIndex.lastRecoveryPackCRCsIndexToUpdate = _newDB.PackCRCs.Size();
}

void CHandler::WriteRecoveryPackSizesData()
{
  int packSizesSize = _newDB.PackSizes.Size() - _recoveryIndex.lastRecoveryPackSizesIndexToUpdate;
  _recoveryStreamOut.write(reinterpret_cast <const char*> (&packSizesSize), sizeof(packSizesSize));
  for (int currPackSize = _recoveryIndex.lastRecoveryPackSizesIndexToUpdate; currPackSize < _newDB.PackSizes.Size(); ++currPackSize)
  {
    _recoveryStreamOut.write(reinterpret_cast <const char*> (&_newDB.PackSizes[currPackSize]), sizeof(_newDB.PackSizes[currPackSize]));
    _totalPackSize += _newDB.PackSizes[currPackSize];
  }

  _recoveryIndex.lastRecoveryPackSizesIndexToUpdate = _newDB.PackSizes.Size();
}

void CHandler::WriteRecoveryNumUnpackStreamsVectorData()
{
  int numUnpackStreamsVectorSize = _newDB.NumUnpackStreamsVector.Size() - _recoveryIndex.lastRecoveryNumUnpackStreamsVectorIndexToUpdate;
  _recoveryStreamOut.write(reinterpret_cast <const char*> (&numUnpackStreamsVectorSize), sizeof(numUnpackStreamsVectorSize));
  for (int currNumUnpackStreamsVector = _recoveryIndex.lastRecoveryNumUnpackStreamsVectorIndexToUpdate; currNumUnpackStreamsVector < _newDB.NumUnpackStreamsVector.Size(); ++currNumUnpackStreamsVector)
  {
    _recoveryStreamOut.write(reinterpret_cast <const char*> (&_newDB.NumUnpackStreamsVector[currNumUnpackStreamsVector]), sizeof(_newDB.NumUnpackStreamsVector[currNumUnpackStreamsVector]));
  }

  _recoveryIndex.lastRecoveryNumUnpackStreamsVectorIndexToUpdate = _newDB.NumUnpackStreamsVector.Size();
}

void CHandler::WriteRecoveryIsAntiData()
{
  int isAntiSize = _newDB.IsAnti.Size() - _recoveryIndex.lastRecoveryIsAntiIndexToUpdate;
  _recoveryStreamOut.write(reinterpret_cast <const char*> (&isAntiSize), sizeof(isAntiSize));
  for (int currIsAnti = _recoveryIndex.lastRecoveryIsAntiIndexToUpdate; currIsAnti < _newDB.IsAnti.Size(); ++currIsAnti)
  {
    _recoveryStreamOut.write(reinterpret_cast <const char*> (&_newDB.IsAnti[currIsAnti]), sizeof(_newDB.IsAnti[currIsAnti]));
  }

  _recoveryIndex.lastRecoveryIsAntiIndexToUpdate = _newDB.IsAnti.Size();
}

void CHandler::WriteRecoveryFilesData()
{
  int filesSize = _newDB.Files.Size() - _recoveryIndex.lastRecoveryFilesIndexToUpdate;
  _recoveryStreamOut.write(reinterpret_cast <const char*> (&filesSize), sizeof(filesSize));
  for (int currFile = _recoveryIndex.lastRecoveryFilesIndexToUpdate; currFile < _newDB.Files.Size(); ++currFile)
  {
    //    Attrib
    _recoveryStreamOut.write(reinterpret_cast <const char*> (&_newDB.Files[currFile].Attrib), sizeof(_newDB.Files[currFile].Attrib));
    //    AttribDefined
    _recoveryStreamOut.write(reinterpret_cast <const char*> (&_newDB.Files[currFile].AttribDefined), sizeof(_newDB.Files[currFile].AttribDefined));
    //    Crc
    _recoveryStreamOut.write(reinterpret_cast <const char*> (&_newDB.Files[currFile].Crc), sizeof(_newDB.Files[currFile].Crc));
    //    CrcDefined
    _recoveryStreamOut.write(reinterpret_cast <const char*> (&_newDB.Files[currFile].CrcDefined), sizeof(_newDB.Files[currFile].CrcDefined));
    //    HasStream
    _recoveryStreamOut.write(reinterpret_cast <const char*> (&_newDB.Files[currFile].HasStream), sizeof(_newDB.Files[currFile].HasStream));
    //    IsDir
    _recoveryStreamOut.write(reinterpret_cast <const char*> (&_newDB.Files[currFile].IsDir), sizeof(_newDB.Files[currFile].IsDir));
    //    Length of name including null terminated character
    UInt32 nameLength = _newDB.Files[currFile].Name.Length() + 1;
    _recoveryStreamOut.write(reinterpret_cast <const char*> (&nameLength), sizeof(nameLength));
    //    Name - UString - wchar_t*
    _recoveryStreamOut.write(reinterpret_cast <const char*> (&_newDB.Files[currFile].Name[0]), sizeof(wchar_t) * nameLength);
    //    Size
    _recoveryStreamOut.write(reinterpret_cast <const char*> (&_newDB.Files[currFile].Size), sizeof(_newDB.Files[currFile].Size));
  }

  _recoveryIndex.lastRecoveryFilesIndexToUpdate = _newDB.Files.Size();
}

void CHandler::WriteRecoveryFoldersData()
{
  int foldersSize = _newDB.Folders.Size() - _recoveryIndex.lastRecoveryFoldersIndexToUpdate;
  _recoveryStreamOut.write(reinterpret_cast <const char*> (&foldersSize), sizeof(foldersSize));
  for (int currFolder = _recoveryIndex.lastRecoveryFoldersIndexToUpdate; currFolder < _newDB.Folders.Size(); ++currFolder)
  {
    //     BindPairs
    int bindPairSize = _newDB.Folders[currFolder].BindPairs.Size();
    _recoveryStreamOut.write(reinterpret_cast <const char*> (&bindPairSize), sizeof(bindPairSize));
    for(int currBindPair = 0; currBindPair < bindPairSize; ++currBindPair)
    {
      //      InIndex
      _recoveryStreamOut.write(reinterpret_cast <const char*> (&_newDB.Folders[currFolder].BindPairs[currBindPair].InIndex), sizeof(_newDB.Folders[currFolder].BindPairs[currBindPair].InIndex));
	  //      OutIdex
      _recoveryStreamOut.write(reinterpret_cast <const char*> (&_newDB.Folders[currFolder].BindPairs[currBindPair].OutIndex), sizeof(_newDB.Folders[currFolder].BindPairs[currBindPair].OutIndex));
    }

    //    Coders
    int coderSize = _newDB.Folders[currFolder].Coders.Size();
    _recoveryStreamOut.write(reinterpret_cast <const char*> (&coderSize), sizeof(coderSize));
    for(int currCoder = 0; currCoder < coderSize; ++currCoder)
    {
      //      MethodID
      _recoveryStreamOut.write(reinterpret_cast <const char*> (&_newDB.Folders[currFolder].Coders[currCoder].MethodID), sizeof(_newDB.Folders[currFolder].Coders[currCoder].MethodID));
      //      NumInStreams
      _recoveryStreamOut.write(reinterpret_cast <const char*> (&_newDB.Folders[currFolder].Coders[currCoder].NumInStreams), sizeof(_newDB.Folders[currFolder].Coders[currCoder].NumInStreams));
      //      NumOutStreams
      _recoveryStreamOut.write(reinterpret_cast <const char*> (&_newDB.Folders[currFolder].Coders[currCoder].NumOutStreams), sizeof(_newDB.Folders[currFolder].Coders[currCoder].NumOutStreams));
      //      Props
      int bufferCapacity = _newDB.Folders[currFolder].Coders[currCoder].Props.GetCapacity();
      _recoveryStreamOut.write(reinterpret_cast <const char*> (&bufferCapacity), sizeof(bufferCapacity));
      _recoveryStreamOut.write(reinterpret_cast <const char*> (&_newDB.Folders[currFolder].Coders[currCoder].Props[0]), bufferCapacity);
    }

    //    PackStreams
    int packStreamsSize = _newDB.Folders[currFolder].PackStreams.Size();
    _recoveryStreamOut.write(reinterpret_cast <const char*> (&packStreamsSize), sizeof(packStreamsSize));
    for(int currPackStream = 0; currPackStream < packStreamsSize; ++currPackStream)
    {
      _recoveryStreamOut.write(reinterpret_cast <const char*> (&_newDB.Folders[currFolder].PackStreams[currPackStream]), sizeof(_newDB.Folders[currFolder].PackStreams[currPackStream]));
    }

    //    UnpackCRC
    _recoveryStreamOut.write(reinterpret_cast <const char*> (&_newDB.Folders[currFolder].UnpackCRC), sizeof(_newDB.Folders[currFolder].UnpackCRC));

    //    UnpackCRCDefined
    _recoveryStreamOut.write(reinterpret_cast <const char*> (&_newDB.Folders[currFolder].UnpackCRCDefined), sizeof(_newDB.Folders[currFolder].UnpackCRCDefined));

	//    UnpackSizes
    int unpackSizesSize = _newDB.Folders[currFolder].UnpackSizes.Size();
    _recoveryStreamOut.write(reinterpret_cast <const char*> (&unpackSizesSize), sizeof(unpackSizesSize));
    for(int currUnpackSize = 0; currUnpackSize < unpackSizesSize; ++currUnpackSize)
    {
      _recoveryStreamOut.write(reinterpret_cast <const char*> (&_newDB.Folders[currFolder].UnpackSizes[currUnpackSize]), sizeof(_newDB.Folders[currFolder].UnpackSizes[currUnpackSize]));
    }
  }

  _recoveryIndex.lastRecoveryFoldersIndexToUpdate = _newDB.Folders.Size();
}

HRESULT CHandler::UpdateRecoveryData()
{
  COM_TRY_BEGIN

  if (!_recoveryStreamOut.is_open())
    return S_FALSE;

  // Update start of recovery position for the item in the recovery file
  if (_recoveryIndex.lastRecoveryFilesIndexToUpdate < _newDB.Files.Size())
  {
    _newDB.Files[_recoveryIndex.lastRecoveryFilesIndexToUpdate].RecoveryRecordPos = _recoveryStreamOut.tellp();
  }

  WriteRecoveryStartPosData();
  WriteRecoveryCTimeData();
  WriteRecoveryMTimeData();
  WriteRecoveryATimeData();
  WriteRecoveryPackCRCsDefinedData();
  WriteRecoveryPackCRCsData();
  WriteRecoveryPackSizesData();
  WriteRecoveryNumUnpackStreamsVectorData();
  WriteRecoveryIsAntiData();
  WriteRecoveryFilesData();
  WriteRecoveryFoldersData();

  _recoveryStreamOut.flush();

  return S_OK;
  COM_TRY_END
}

HRESULT CHandler::OpenWithRecoveryData(UString& recoveryFileName,
    COutMultiVolStream *outStream,
	CObjectVector<UString> &filterDirs,
	UString& itemStatFilter,
	UString& cocEntryFilter)
{
  COM_TRY_BEGIN

  _recoveredFileCount = 0;
  _recoveredUncompressedFileSize = 0;

  if (NULL != _archive.SeqStream)
	  return S_FALSE;

  std::ifstream recoveryStream(recoveryFileName, std::ios::binary);
  if (!recoveryStream.is_open())
    return E_FAIL;

  // Last coc index is used to exclude all items
  // that were written without a valid Coc entry
  int lastCocStartPosSize = 0;
  int lastCocCTimeSize = 0;
  int lastCocMTimeSize = 0;
  int lastCocATimeSize = 0;
  int lastCocPackCRCsDefinedSize = 0;
  int lastCocPackCRCsSize = 0;
  int lastCocFilesSize = 0;
  int lastCocFoldersSize = 0;
  int lastCocPackSizesSize = 0;
  int lastCocNumUnpackStreamsVectorSize = 0;
  int lastCocIsAntiSize = 0;

  // 7-ZIP RECOVERY SIGNATURE ( 4 Bytes )
  // =============================================
  int signatureLength = wcslen(kRecoverySignature) + 1;
  UString signature;
  recoveryStream.read(reinterpret_cast <char*> (signature.GetBuffer(signatureLength)), signatureLength * sizeof(wchar_t));

  // If signature does not match, we can't recover
  if (signature != kRecoverySignature)
  {
    recoveryStream.close();
	return E_FAIL;
  }

  fpos_t validRecoveryStreamEndPos = recoveryStream.tellg().seekpos();

  // We will clear all of the existing DB state and try to recover it
  _newDB.Clear();

  while(true)
  {
    // StartPos
    // =============================================
    CUInt64DefVector startPos;
    ReadRecoveryStartPosData(recoveryStream, startPos);

    // CTime
    // =============================================
    CUInt64DefVector cTime;
    ReadRecoveryCTimeData(recoveryStream, cTime);

    // MTime
    // =============================================
    CUInt64DefVector mTime;
    ReadRecoveryMTimeData(recoveryStream, mTime);
  
    // ATime
    // =============================================
    CUInt64DefVector aTime;
    ReadRecoveryATimeData(recoveryStream, aTime);

    // PackCRCsDefined
    // =============================================
    CRecordVector<bool> packCRCsDefined;
    ReadRecoveryPackCRCsDefinedData(recoveryStream, packCRCsDefined);

    // PackCRCs
    // =============================================
    CRecordVector<UInt32> packCRCs;
    ReadRecoveryPackCRCsData(recoveryStream, packCRCs);

    // PackSizes
    // =============================================
    CRecordVector<UInt64> packSizes;
    ReadRecoveryPackSizesData(recoveryStream, packSizes);

   // NumUnpackStreamVector
   // =============================================
    CRecordVector<CNum> numUnpackStreamsVector;
    ReadRecoveryNumUnpackStreamsVectorData(recoveryStream, numUnpackStreamsVector);

    // IsAnti
    // =============================================
    CRecordVector<bool> isAnti;
    ReadRecoveryIsAntiData(recoveryStream, isAnti);

    // Files
    // =============================================
    CObjectVector<CFileItem> files;
    ReadRecoveryFilesData(recoveryStream, files);

    // !!!!!Will remove all items from a found item since all streams are sequential!!!!!
    bool itemFiltered = false;
    for (int currentFileItem = 0; currentFileItem < files.Size(); ++currentFileItem)
    {
      if (IsItemFiltered(files[currentFileItem].Name, filterDirs))
      {
        itemFiltered = true;
        break;
      }
    }
    if(itemFiltered)
      break;

    // Folders
    // =============================================
    CObjectVector<CFolder> folders;
    ReadRecoveryFoldersData(recoveryStream, folders);

    // We only add full records
    if (recoveryStream.eof())
      break;

	// Count recovered stats info
    for (int currentFileItem = 0; currentFileItem < files.Size(); ++currentFileItem)
    {
      if (files[currentFileItem].Name.Left(itemStatFilter.Length()) == itemStatFilter)
      {
        ++_recoveredFileCount;

        for (int currFolder = 0; currFolder < folders.Size(); ++currFolder)
        {
          _recoveredUncompressedFileSize += folders[currFolder].GetUnpackSize();
        }
      }
    }

    // Add new record
    for (int currStartPos = 0; currStartPos < startPos.Defined.Size(); ++currStartPos)
    {
      _newDB.StartPos.Defined.Add(startPos.Defined[currStartPos]);
      if (currStartPos < startPos.Values.Size())
        _newDB.StartPos.Values.Add(startPos.Values[currStartPos]);
    }
    for (int currCTime = 0; currCTime < cTime.Defined.Size(); ++currCTime)
    {
      _newDB.CTime.Defined.Add(cTime.Defined[currCTime]);
      if (currCTime < cTime.Values.Size())
        _newDB.CTime.Values.Add(cTime.Values[currCTime]);
    }
    for (int currMTime = 0; currMTime < mTime.Defined.Size(); ++currMTime)
    {
      _newDB.MTime.Defined.Add(mTime.Defined[currMTime]);
      if (currMTime < mTime.Values.Size())
        _newDB.MTime.Values.Add(mTime.Values[currMTime]);
    }
    for (int currATime = 0; currATime < aTime.Defined.Size(); ++currATime)
    {
      _newDB.ATime.Defined.Add(aTime.Defined[currATime]);
      if (currATime < aTime.Values.Size())
        _newDB.ATime.Values.Add(aTime.Values[currATime]);
    }
    for (int currPackCRCsDefined = 0; currPackCRCsDefined < packCRCsDefined.Size(); ++currPackCRCsDefined)
      _newDB.PackCRCsDefined.Add(packCRCsDefined[currPackCRCsDefined]);
    for (int currPackCRCs = 0; currPackCRCs < packCRCs.Size(); ++currPackCRCs)
      _newDB.PackCRCs.Add(packCRCs[currPackCRCs]);
    for (int currPackSizes = 0; currPackSizes < packSizes.Size(); ++currPackSizes)
      _newDB.PackSizes.Add(packSizes[currPackSizes]);
    for (int currNumUnpackStreamsVector = 0; currNumUnpackStreamsVector < numUnpackStreamsVector.Size(); ++currNumUnpackStreamsVector)
      _newDB.NumUnpackStreamsVector.Add(numUnpackStreamsVector[currNumUnpackStreamsVector]);
    for (int currIsAnti = 0; currIsAnti < isAnti.Size(); ++currIsAnti)
      _newDB.IsAnti.Add(isAnti[currIsAnti]);
    for (int currFiles = 0; currFiles < files.Size(); ++currFiles)
      _newDB.Files.Add(files[currFiles]);
    for (int currFolders = 0; currFolders < folders.Size(); ++currFolders)
      _newDB.Folders.Add(folders[currFolders]);

    // See if we have a coc entry in which case, we have a good non-corrupted entry
    // and only in that case update the indexes and recovery stream end positions
    if (files.Size() && 
        files[files.Size() - 1].Name.Left(cocEntryFilter.Length()) == cocEntryFilter)
    {
      lastCocStartPosSize = _newDB.StartPos.Defined.Size();
      lastCocCTimeSize = _newDB.CTime.Defined.Size();
      lastCocMTimeSize = _newDB.MTime.Defined.Size();
      lastCocATimeSize = _newDB.ATime.Defined.Size();
      lastCocPackCRCsDefinedSize = _newDB.PackCRCsDefined.Size();
      lastCocPackCRCsSize = _newDB.PackCRCs.Size();
      lastCocFilesSize = _newDB.Files.Size();
      lastCocFoldersSize = _newDB.Folders.Size();
      lastCocPackSizesSize = _newDB.PackSizes.Size();
      lastCocNumUnpackStreamsVectorSize = _newDB.NumUnpackStreamsVector.Size();
      lastCocIsAntiSize = _newDB.IsAnti.Size();

      validRecoveryStreamEndPos = recoveryStream.tellg().seekpos();
    }
  }

  // Now remove everything from _newDB that wasn't followed by a COC entry which means
  // that an operation has been interrupted and therefore can not be considered complete
  _newDB.StartPos.Defined.DeleteFrom(lastCocStartPosSize);
  _newDB.CTime.Defined.DeleteFrom(lastCocCTimeSize);
  _newDB.MTime.Defined.DeleteFrom(lastCocMTimeSize);
  _newDB.ATime.Defined.DeleteFrom(lastCocATimeSize);
  _newDB.PackCRCsDefined.DeleteFrom(lastCocPackCRCsDefinedSize);
  _newDB.PackCRCs.DeleteFrom(lastCocPackCRCsSize);
  _newDB.Files.DeleteFrom(lastCocFilesSize);
  _newDB.Folders.DeleteFrom(lastCocFoldersSize);
  _newDB.PackSizes.DeleteFrom(lastCocPackSizesSize);
  _newDB.NumUnpackStreamsVector.DeleteFrom(lastCocNumUnpackStreamsVectorSize);
  _newDB.IsAnti.DeleteFrom(lastCocIsAntiSize);

  recoveryStream.close();
  _recoveryFileName = recoveryFileName;
  _recoveryStreamOut.open(recoveryFileName, std::ios_base::binary | std::ios_base::out | std::ios_base::in | std::ios_base::ate);

  if (!_recoveryStreamOut.is_open())
    return S_FALSE;

  // Set file point to eof for new records to be written
  // NOTE (cast): position from get of istream for some reason different in type
  _recoveryStreamOut.seekp((std::streamoff)validRecoveryStreamEndPos);
  if (NULL == _archive.SeqStream)
    RINOK(_archive.Create(outStream, false));

  // Set correct processed size and reset file pointer
  UInt64 packedSize = 0;
  for (int i = 0; i < _newDB.PackSizes.Size(); i++)
    packedSize += _newDB.PackSizes[i];
  outStream->SetSize(packedSize + 32);
  UInt64 newPos = 0;

  outStream->Seek(packedSize + 32, 0, &newPos);

  _recoveryIndex.lastRecoveryStartPosIndexToUpdate = _newDB.StartPos.Defined.Size();
  _recoveryIndex.lastRecoveryCTimeIndexToUpdate = _newDB.CTime.Defined.Size();
  _recoveryIndex.lastRecoveryMTimeIndexToUpdate = _newDB.MTime.Defined.Size();
  _recoveryIndex.lastRecoveryATimeIndexToUpdate = _newDB.ATime.Defined.Size();
  _recoveryIndex.lastRecoveryPackCRCsDefinedIndexToUpdate = _newDB.PackCRCsDefined.Size();
  _recoveryIndex.lastRecoveryPackCRCsIndexToUpdate = _newDB.PackCRCs.Size();
  _recoveryIndex.lastRecoveryFilesIndexToUpdate = _newDB.Files.Size();
  _recoveryIndex.lastRecoveryFoldersIndexToUpdate = _newDB.Folders.Size();
  _recoveryIndex.lastRecoveryPackSizesIndexToUpdate = _newDB.PackSizes.Size();
  _recoveryIndex.lastRecoveryNumUnpackStreamsVectorIndexToUpdate = _newDB.NumUnpackStreamsVector.Size();
  _recoveryIndex.lastRecoveryIsAntiIndexToUpdate = _newDB.IsAnti.Size();

  _totalPackSize = packedSize;

  return S_OK;

  COM_TRY_END
}

HRESULT CHandler::SetRecoveryOption(UString &recoveryFileName)
{
  if (_recoveryStreamOut.is_open())
    return S_OK;

  _recoveryFileName = recoveryFileName;
  _recoveryStreamOut.open(recoveryFileName, std::ios_base::binary | std::ios_base::out | std::ios_base::ate);

  if (_recoveryStreamOut.is_open())
  {
    // 7-ZIP RECOVERY SIGNATURE ( 4 Bytes )
    // =============================================
    int signatureLength = wcslen(kRecoverySignature) + 1;

    _recoveryStreamOut.write(reinterpret_cast <const char*> (kRecoverySignature), signatureLength * sizeof(wchar_t));
    _recoveryStreamOut.flush();
    _recoveryIndex.Init();

	return S_OK;
  }

  return E_FAIL;
}

unsigned long long CHandler::GetFileCount()
{
  return _newDB.Files.Size();
}

unsigned long long CHandler::GetTotalPackSize()
{
  return _totalPackSize;
}

unsigned long long CHandler::GetRecoveredFileCount()
{
  return _recoveredFileCount;
}

unsigned long long CHandler::GetRecoveredUncompressedFileSize()
{
  return _recoveredUncompressedFileSize;
}

STDMETHODIMP CHandler::UpdateItems(ISequentialOutStream *outStream, UInt32 numItems,
    IArchiveUpdateCallback *updateCallback)
{
  COM_TRY_BEGIN

  const CArchiveDatabaseEx *db = 0;
  #ifdef _7Z_VOL
  if(_volumes.Size() > 1)
    return E_FAIL;
  const CVolume *volume = 0;
  if (_volumes.Size() == 1)
  {
    volume = &_volumes.Front();
    db = &volume->Database;
  }
  #else
  if (_inStream != 0)
    db = &_db;
  #endif

  CObjectVector<CUpdateItem> updateItems;
  
  for (UInt32 i = 0; i < numItems; i++)
  {
    Int32 newData;
    Int32 newProperties;
    UInt32 indexInArchive;
    if (!updateCallback)
      return E_FAIL;
    RINOK(updateCallback->GetUpdateItemInfo(i, &newData, &newProperties, &indexInArchive));
    CUpdateItem ui;
    ui.NewProperties = IntToBool(newProperties);
    ui.NewData = IntToBool(newData);
    ui.IndexInArchive = indexInArchive;
    ui.IndexInClient = i;
    ui.IsAnti = false;
    ui.Size = 0;

    if (ui.IndexInArchive != -1)
    {
      const CFileItem &fi = db->Files[ui.IndexInArchive];
      ui.Name = fi.Name;
      ui.IsDir = fi.IsDir;
      ui.Size = fi.Size;
      ui.IsAnti = db->IsItemAnti(ui.IndexInArchive);
      
      ui.CTimeDefined = db->CTime.GetItem(ui.IndexInArchive, ui.CTime);
      ui.ATimeDefined = db->ATime.GetItem(ui.IndexInArchive, ui.ATime);
      ui.MTimeDefined = db->MTime.GetItem(ui.IndexInArchive, ui.MTime);
    }

    if (ui.NewProperties)
    {
      bool nameIsDefined;
      bool folderStatusIsDefined;
      {
        NCOM::CPropVariant prop;
        RINOK(updateCallback->GetProperty(i, kpidAttrib, &prop));
        if (prop.vt == VT_EMPTY)
          ui.AttribDefined = false;
        else if (prop.vt != VT_UI4)
          return E_INVALIDARG;
        else
        {
          ui.Attrib = prop.ulVal;
          ui.AttribDefined = true;
        }
      }
      
      // we need MTime to sort files.
      RINOK(GetTime(updateCallback, i, WriteCTime, kpidCTime, ui.CTime, ui.CTimeDefined));
      RINOK(GetTime(updateCallback, i, WriteATime, kpidATime, ui.ATime, ui.ATimeDefined));
      RINOK(GetTime(updateCallback, i, true,       kpidMTime, ui.MTime, ui.MTimeDefined));

      {
        NCOM::CPropVariant prop;
        RINOK(updateCallback->GetProperty(i, kpidPath, &prop));
        if (prop.vt == VT_EMPTY)
          nameIsDefined = false;
        else if (prop.vt != VT_BSTR)
          return E_INVALIDARG;
        else
        {
          ui.Name = NItemName::MakeLegalName(prop.bstrVal);
          nameIsDefined = true;
        }
      }
      {
        NCOM::CPropVariant prop;
        RINOK(updateCallback->GetProperty(i, kpidIsDir, &prop));
        if (prop.vt == VT_EMPTY)
          folderStatusIsDefined = false;
        else if (prop.vt != VT_BOOL)
          return E_INVALIDARG;
        else
        {
          ui.IsDir = (prop.boolVal != VARIANT_FALSE);
          folderStatusIsDefined = true;
        }
      }

      {
        NCOM::CPropVariant prop;
        RINOK(updateCallback->GetProperty(i, kpidIsAnti, &prop));
        if (prop.vt == VT_EMPTY)
          ui.IsAnti = false;
        else if (prop.vt != VT_BOOL)
          return E_INVALIDARG;
        else
          ui.IsAnti = (prop.boolVal != VARIANT_FALSE);
      }

      if (ui.IsAnti)
      {
        ui.AttribDefined = false;

        ui.CTimeDefined = false;
        ui.ATimeDefined = false;
        ui.MTimeDefined = false;
        
        ui.Size = 0;
      }

      if (!folderStatusIsDefined && ui.AttribDefined)
        ui.SetDirStatusFromAttrib();
    }

    if (ui.NewData)
    {
      NCOM::CPropVariant prop;
      RINOK(updateCallback->GetProperty(i, kpidSize, &prop));
      if (prop.vt != VT_UI8)
        return E_INVALIDARG;
      ui.Size = (UInt64)prop.uhVal.QuadPart;
      if (ui.Size != 0 && ui.IsAnti)
        return E_INVALIDARG;
    }
    updateItems.Add(ui);
  }

  CCompressionMethodMode methodMode, headerMethod;
  RINOK(SetCompressionMethod(methodMode, headerMethod));
  #ifdef COMPRESS_MT
  methodMode.NumThreads = _numThreads;
  headerMethod.NumThreads = 1;
  #endif

  RINOK(SetPassword(methodMode, updateCallback));

  bool compressMainHeader = _compressHeaders;  // check it

  bool encryptHeaders = false;

  if (methodMode.PasswordIsDefined)
  {
    if (_encryptHeadersSpecified)
      encryptHeaders = _encryptHeaders;
    #ifndef _NO_CRYPTO
    else
      encryptHeaders = _passwordIsDefined;
    #endif
    compressMainHeader = true;
    if(encryptHeaders)
      RINOK(SetPassword(headerMethod, updateCallback));
  }

  if (numItems < 2)
    compressMainHeader = false;

  CUpdateOptions options;
  options.Method = &methodMode;
  options.HeaderMethod = (_compressHeaders || encryptHeaders) ? &headerMethod : 0;
  options.UseFilters = _level != 0 && _autoFilter;
  options.MaxFilter = _level >= 8;

  options.HeaderOptions.CompressMainHeader = compressMainHeader;
  options.HeaderOptions.WriteCTime = WriteCTime;
  options.HeaderOptions.WriteATime = WriteATime;
  options.HeaderOptions.WriteMTime = WriteMTime;
  
  options.NumSolidFiles = _numSolidFiles;
  options.NumSolidBytes = _numSolidBytes;
  options.SolidExtension = _solidExtension;
  options.RemoveSfxBlock = _removeSfxBlock;
  options.VolumeMode = _volumeMode;

  HRESULT res = Update(
      EXTERNAL_CODECS_VARS
      #ifdef _7Z_VOL
      volume ? volume->Stream: 0,
      volume ? db : 0,
      #else
      _inStream,
      db,
      #endif
      updateItems,
      _archive, _newDB, outStream, updateCallback, options);

  RINOK(res);

  updateItems.ClearAndFree();

  if (0 == numItems) // Close archive
  {
    res = _archive.WriteDatabase(EXTERNAL_CODECS_VARS
      _newDB, options.HeaderMethod, options.HeaderOptions);

    if (_recoveryStreamOut.is_open())
      _recoveryStreamOut.close();

    return res;
  }

  //
  // Only add recovery record if the operation was not aborted
  // which we figure out by comparing expected size with streamed size
  //
  {
	  NWindows::NCOM::CPropVariant prop;
	  updateCallback->GetProperty(0, kpidSize, &prop);

	  int index = _newDB.Files.Size() - 1;
	  if (index >= 0 && index < _newDB.Files.Size() )
	  {
        _newDB.Files[index].RecoveryRecordPos = 0;
        if (prop.uhVal.QuadPart == _newDB.Files[index].Size)
          UpdateRecoveryData();
	  }
  }
  
  return res;

  COM_TRY_END
}

static HRESULT GetBindInfoPart(UString &srcString, UInt32 &coder, UInt32 &stream)
{
  stream = 0;
  int index = ParseStringToUInt32(srcString, coder);
  if (index == 0)
    return E_INVALIDARG;
  srcString.Delete(0, index);
  if (srcString[0] == 'S')
  {
    srcString.Delete(0);
    int index = ParseStringToUInt32(srcString, stream);
    if (index == 0)
      return E_INVALIDARG;
    srcString.Delete(0, index);
  }
  return S_OK;
}

static HRESULT GetBindInfo(UString &srcString, CBind &bind)
{
  RINOK(GetBindInfoPart(srcString, bind.OutCoder, bind.OutStream));
  if (srcString[0] != ':')
    return E_INVALIDARG;
  srcString.Delete(0);
  RINOK(GetBindInfoPart(srcString, bind.InCoder, bind.InStream));
  if (!srcString.IsEmpty())
    return E_INVALIDARG;
  return S_OK;
}

STDMETHODIMP CHandler::SetProperties(const wchar_t **names, const PROPVARIANT *values, Int32 numProperties)
{
  COM_TRY_BEGIN
  _binds.Clear();
  BeforeSetProperty();

  for (int i = 0; i < numProperties; i++)
  {
    UString name = names[i];
    name.MakeUpper();
    if (name.IsEmpty())
      return E_INVALIDARG;

    const PROPVARIANT &value = values[i];

    if (name[0] == 'B')
    {
      name.Delete(0);
      CBind bind;
      RINOK(GetBindInfo(name, bind));
      _binds.Add(bind);
      continue;
    }

    RINOK(SetProperty(name, value));
  }

  return S_OK;
  COM_TRY_END
}

}}
