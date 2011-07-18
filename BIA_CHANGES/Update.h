#ifndef __UPDATE_H
#define __UPDATE_H

// Copied from ../UI/Common/Update.cpp

#include "../../Common/MyCom.h"
#include "../../Common/MyString.h"
#include "../../Common/MyVector.h"
#include "../Archive/IArchive.h"
#include "../Common/FileStreams.h"
#include "../UI/Common/TempFiles.h"

class COutMultiVolStream:
  public IOutStream,
  public CMyUnknownImp
{
  int _streamIndex; // required stream
  unsigned long long _offsetPos; // offset from start of _streamIndex index
  unsigned long long _absPos;

protected:
  unsigned long long _length;

  struct CSubStreamInfo
  {
    COutFileStream *StreamSpec;
    CMyComPtr<IOutStream> Stream;
    UString Name;
    unsigned long long Pos;
    unsigned long long RealSize;
  };
  CObjectVector<CSubStreamInfo> Streams;

  virtual bool OnNewPart(int partNumber, UString name, bool open = false);
  virtual void DeleteFileAlways(UString name);
  virtual bool IsValidNonExistentPart(int partNumber);
public:
  virtual ~COutMultiVolStream() {}
  // CMyComPtr<IArchiveUpdateCallback2> VolumeCallback;
  CRecordVector<unsigned long long> Sizes;
  UString Prefix;

  void Init()
  {
    _streamIndex = 0;
    _offsetPos = 0;
    _absPos = 0;
    _length = 0;
  }

  HRESULT Close();
  HRESULT Open();

  MY_UNKNOWN_IMP1(IOutStream)

  STDMETHOD(Write)(const void *data, UInt32 size, UInt32 *processedSize);
  STDMETHOD(Seek)(Int64 offset, UInt32 seekOrigin, UInt64 *newPosition);
  STDMETHOD(SetSize)(Int64 newSize);
};

#endif