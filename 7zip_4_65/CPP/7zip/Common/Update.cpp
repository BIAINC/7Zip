#include "StdAfx.h"
#include "Update.h"
#include "../../Common/IntToString.h"
#include "../../Windows/FileDir.h"
#include "../../Windows/FileFind.h"


// Copied from ../UI/Common/Update.cpp

using namespace NWindows;
using namespace NFile;



HRESULT COutMultiVolStream::Close()
{
  HRESULT res = S_OK;
  for (int i = 0; i < Streams.Size(); i++)
  {
    CSubStreamInfo &s = Streams[i];
    if (s.StreamSpec)
    {
      HRESULT res2 = s.StreamSpec->Close();
      if (res2 != S_OK)
        res = res2;
    }
  }
  return res;
}

HRESULT COutMultiVolStream::Open()
{
  if (Prefix.Length() <= 0)
    return E_FAIL;

  // Because volume sizes are expected to be the same for mid parts,
  // in case the mid part does not exist, use the size of the previous
  // stream for its size
  UInt64 previousSubStreamSize = 0;

  // Create all of the substreams
  int currentIndex = 0;
  while(true)
  {
    // In case we know that part exists but no file is found,
    // we still would like to open it because we can add and remove
    // items from the original container.  So, just create an empty substream
    // which in case is used would produce and error anyway.  First and last
    // are required to exist though.
    bool validNonExistentPart = this->IsValidNonExistentPart(++currentIndex);

    CSubStreamInfo subStream;

    wchar_t temp[32];
    ConvertUInt64ToString(currentIndex, temp);

    UString res = temp;
    while (res.Length() < 3)
      res = UString(L'0') + res;
    UString name = Prefix + res;
    subStream.StreamSpec = new COutFileStream;
    subStream.Stream = subStream.StreamSpec;
    if(!subStream.StreamSpec->Open(name, OPEN_EXISTING)
      && !validNonExistentPart)
    {
      break;
    }

    if (!OnNewPart(currentIndex, name, true))
	{
        return E_FAIL;
    }

	UInt64 subStreamSize = 0;
    if (!validNonExistentPart)
    {
	  RINOK(subStream.StreamSpec->Seek(0, STREAM_SEEK_END, &subStreamSize));
      subStream.Pos = subStreamSize;
      subStream.RealSize = subStreamSize;
      previousSubStreamSize = subStreamSize;
    }
    else
    {
      subStream.Pos = 0;
      subStream.RealSize = previousSubStreamSize;
    }
    
    subStream.Name = name;
    Streams.Add(subStream);
  }

  return Streams.Size() == 0 ? S_FALSE : S_OK;
}

STDMETHODIMP COutMultiVolStream::Write(const void *data, UInt32 size, UInt32 *processedSize)
{
  if(processedSize != NULL)
    *processedSize = 0;
  while(size > 0)
  {
    if (_streamIndex >= Streams.Size())
    {
      CSubStreamInfo subStream;

      wchar_t temp[32];
      ConvertUInt64ToString(_streamIndex + 1, temp);
      UString res = temp;
      while (res.Length() < 3)
        res = UString(L'0') + res;
      UString name = Prefix + res;

      if (!OnNewPart(_streamIndex + 1, name))
      {
        return E_FAIL;
      }

      subStream.StreamSpec = new COutFileStream;
      subStream.Stream = subStream.StreamSpec;
      if(!subStream.StreamSpec->Create(name, false))
        return ::GetLastError();

      subStream.Pos = 0;
      subStream.RealSize = 0;
      subStream.Name = name;
      Streams.Add(subStream);
      continue;
    }
    CSubStreamInfo &subStream = Streams[_streamIndex];

    int index = _streamIndex;
    if (index >= Sizes.Size())
      index = Sizes.Size() - 1;
    UInt64 volSize = Sizes[index];

    if (_offsetPos >= volSize)
    {
      _offsetPos -= volSize;
      _streamIndex++;
      continue;
    }
    if (_offsetPos != subStream.Pos)
    {
      // CMyComPtr<IOutStream> outStream;
      // RINOK(subStream.Stream.QueryInterface(IID_IOutStream, &outStream));
      RINOK(subStream.Stream->Seek(_offsetPos, STREAM_SEEK_SET, NULL));
      subStream.Pos = _offsetPos;
    }

    UInt32 curSize = (UInt32)MyMin((UInt64)size, volSize - subStream.Pos);
    UInt32 realProcessed;
    RINOK(subStream.Stream->Write(data, curSize, &realProcessed));
    data = (void *)((Byte *)data + realProcessed);
    size -= realProcessed;
    subStream.Pos += realProcessed;
    _offsetPos += realProcessed;
    _absPos += realProcessed;
    if (_absPos > _length)
      _length = _absPos;
    if (_offsetPos > subStream.RealSize)
      subStream.RealSize = _offsetPos;
    if(processedSize != NULL)
      *processedSize += realProcessed;
    if (subStream.Pos == volSize)
    {
      _streamIndex++;
      _offsetPos = 0;
    }
    if (realProcessed == 0 && curSize != 0)
      return E_FAIL;
    break;
  }
  return S_OK;
}

STDMETHODIMP COutMultiVolStream::Seek(Int64 offset, UInt32 seekOrigin, UInt64 *newPosition)
{
  if(seekOrigin >= 3)
    return STG_E_INVALIDFUNCTION;
  switch(seekOrigin)
  {
    case STREAM_SEEK_SET:
      _absPos = offset;
      break;
    case STREAM_SEEK_CUR:
      _absPos += offset;
      break;
    case STREAM_SEEK_END:
      _absPos = _length + offset;
      break;
  }
  _offsetPos = _absPos;
  if (newPosition != NULL)
    *newPosition = _absPos;
  _streamIndex = 0;
  return S_OK;
}

STDMETHODIMP COutMultiVolStream::SetSize(Int64 newSize)
{
  if (newSize < 0)
    return E_INVALIDARG;
  int i = 0;
  while (i < Streams.Size())
  {
    CSubStreamInfo &subStream = Streams[i++];
    if ((UInt64)newSize < subStream.RealSize)
    {
      RINOK(subStream.Stream->SetSize(newSize));
      subStream.RealSize = newSize;
      break;
    }
    newSize -= subStream.RealSize;
  }
  while (i < Streams.Size())
  {
    {
      CSubStreamInfo &subStream = Streams.Back();
      subStream.Stream.Release();
      DeleteFileAlways(Streams.Size(), subStream.Name);
    }
    Streams.DeleteBack();
  }
  _offsetPos = _absPos;
  _streamIndex = 0;
  _length = newSize;
  return S_OK;
}

bool COutMultiVolStream::OnNewPart(int /*partNumber*/, UString /*name*/, bool /*open = false*/)
{
  return true;
}

void COutMultiVolStream::DeleteFileAlways(int /*partNumber*/, UString name)
{
  NDirectory::DeleteFileAlways(name);
}

bool COutMultiVolStream::IsValidNonExistentPart(int /*partNumber*/)
{
  return false;
}