#define _CRT_SECURE_NO_WARNINGS
#define _FILE_OFFSET_BITS 64
#include <zstd.h>
#include <zstd_errors.h>
#include <stdio.h>
#include <stdlib.h>
#include <unordered_map>
#include <cctype>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <codecvt>
#include <thread>
#include "fs8.h"


#define FS_MAX_FILENAMES_BINARY_SIZE (64 << 20) // max file table = 16 MB (~320000 files)
#define FS_KEEP_IN_MEMORY_THRESHOLD (64 << 10)  // small files (< 64 KB) will be cached in memory
#define FS_MAX_PARTITION 100
#define FS_UNLOCK_FILE_AFTER_MS 500             // call fclose after 500 msec after last access

using namespace std;

wstring string_to_wstring(const string& utf8String)
{
  wstring_convert<codecvt_utf8<wchar_t>> convert;
  return convert.from_bytes(utf8String);
}

string wstring_to_string(const wstring & wideString)
{
  wstring_convert<codecvt_utf8<wchar_t>> convert;
  return convert.to_bytes(wideString);
}


#ifdef _WIN32

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
string get_absolute_file_name(const char * file_name_utf8)
{
  if (!file_name_utf8)
    return string();
  wstring wName = string_to_wstring(file_name_utf8);
  wchar_t buf[512] = { 0 };
  if (GetFullPathNameW(wName.c_str(), 512, buf, nullptr) == 0)
    return string(file_name_utf8);
  return wstring_to_string(buf);
}

#else
#include <unistd.h>

string get_absolute_file_name(const char * file_name_utf8)
{
  if (!file_name_utf8)
    return string();
  char resolvedPath[512] = { 0 };
  realpath(file_name_utf8, resolvedPath);
  return string(resolvedPath);
}

#endif



#ifdef _WIN32
  #define FS_FSEEK _fseeki64
  #define FS_FTELL _ftelli64

  static FILE * FS_FOPEN(const char * file_name_utf8, const char * mode)
  {
    if (!file_name_utf8 || !mode)
      return nullptr;
    wstring wName = string_to_wstring(file_name_utf8);
    wstring wMode = string_to_wstring(mode);
    return _wfsopen(wName.c_str(), wMode.c_str(), _SH_DENYWR);
  }

  static uint64_t get_file_time(const char * file_name_utf8)
  {
    if (!file_name_utf8)
      return 0;
    wstring wName = string_to_wstring(file_name_utf8);
    struct _stati64 buf;
    if (!_wstati64(wName.c_str(), &buf))
      return uint64_t(buf.st_mtime);
    return 0;
  }

  void FS_UNLINK(const char * file_name_utf8)
  {
    wstring wName = string_to_wstring(file_name_utf8);
    DeleteFileW(wName.c_str());
  }

  void FS_RENAME(const char * file_from_utf8, const char * file_dest_utf8)
  {
    wstring wNameFrom = string_to_wstring(file_from_utf8);
    wstring wNameTo = string_to_wstring(file_dest_utf8);
    MoveFileW(wNameFrom.c_str(), wNameTo.c_str());
  }

#else
  #define FS_FSEEK fseeko64
  #define FS_FTELL fseeko64
  static FILE * FS_FOPEN(const char * file_name_utf8, const char * mode)
  {
    if (!file_name_utf8 || !mode)
      return nullptr;
    FILE * f = fopen64(file_name_utf8, mode);
    // ? flock(f, LOCK_EX);
    return f;
  }

  static uint64_t get_file_time(const char * file_name)
  {
    if (!file_name)
      return 0;
    struct stat buf;
    if (!stat(file_name, &buf))
      return uint64_t(buf.st_mtime);
    return 0;
  }

  void FS_UNLINK(const char * file_name_utf8)
  {
    unlink(file_name_utf8);
  }

  void FS_RENAME(const char * file_from_utf8, const char * file_dest_utf8)
  {
    rename(file_from_utf8, file_dest_utf8);
  }

#endif

static void sleep_msec(int milliseconds)
{
#ifdef _WIN32
  Sleep(milliseconds);
#else
  usleep(milliseconds * 1000);
#endif
}



struct ZstdCompressContext
{
  ZSTD_CCtx * ctx = nullptr;
  ~ZstdCompressContext() { ZSTD_freeCCtx(ctx); }

  ZSTD_CCtx * get()
  {
    if (ctx)
      return ctx;
    ctx = ZSTD_createCCtx();
    return ctx;
  }
};

struct ZstdDecompressContext
{
  ZSTD_DCtx * ctx = nullptr;
  ~ZstdDecompressContext() { ZSTD_freeDCtx(ctx); }

  ZSTD_DCtx * get()
  {
    if (ctx)
      return ctx;
    ctx = ZSTD_createDCtx();
    return ctx;
  }
};


static thread_local ZstdCompressContext zstd_compress_context;
static thread_local ZstdDecompressContext zstd_decompress_context;
static recursive_mutex partitions_lock;


struct Fs8FileInfo
{
  // only these 3 fields will be saved to the .fs8
  //////////////////////////////////
  int64_t offsetInFile = 0;
  int64_t compressedSize = 0;
  int64_t decompressedSize = 0;
  //////////////////////////////////

  void resetPtr()
  {
    decompressedPtr = nullptr;
  }

  void * getDecompressedPtr()
  {
    return decompressedPtr;
  }

  void setDecompressedPtr(void * ptr)
  {
    decompressedPtr = ptr;
  }

private:
  void * decompressedPtr = nullptr;
};

using FileInfosMap = unordered_map<string, Fs8FileInfo>;

static void append_bytes(vector<char> & bytes, const void * ptr, size_t size)
{
  bytes.insert(bytes.end(), (const char *)ptr, (const char *)ptr + size);
}

static bool read_bytes(const char ** cursor, int & bytes_left, void * ptr, size_t size)
{
  if (bytes_left < size)
    return false;

  memcpy(ptr, *cursor, size);
  bytes_left -= int(size);
  *cursor += size;
  return true;
}


// 4 size of serialized file infos - 4
// [
//    2  name length
//    ?  lower case name without \0
//    ?  Fs8FileInfo
// ]

int serialize_fs_file_infos(const FileInfosMap & fs_file_infos, vector<char> & bytes)
{
  bytes.clear();
  bytes.resize(sizeof(uint32_t));
  for (auto & f : fs_file_infos)
  {
    string lowerCaseName = f.first;
    for (auto & ch : lowerCaseName)
    {
      ch = tolower(ch);
      if (ch == '\\')
        ch = '/';
    }

    int16_t fileNameLength = int16_t(lowerCaseName.length());
    append_bytes(bytes, &fileNameLength, sizeof(fileNameLength));
    append_bytes(bytes, lowerCaseName.c_str(), lowerCaseName.length());
    append_bytes(bytes, &f.second, sizeof(int64_t) * 3);
  }

  uint32_t size = uint32_t(bytes.size()) - 4;
  memcpy(&bytes[0], &size, sizeof(size));

  return int(bytes.size());
}



int64_t check_header_get_file_names_offset(const char buf[24])
{
  if (strncmp(buf, "FS8.", 4) != 0)
    return 0;

  char version[] = "....";
  memcpy(version, buf + 4, 4);
  int v = atoi(version);
  if (v != 1)
    return 0;

  int64_t fileNamesOffset = 0;
  memcpy(&fileNamesOffset, buf + 8, sizeof(fileNamesOffset));

  return fileNamesOffset;
}

int64_t check_header_get_sign_offset(const char buf[24])
{
  if (strncmp(buf, "FS8.", 4) != 0)
    return 0;

  int64_t signaturesOffset = 0;
  memcpy(&signaturesOffset, buf + 8 + 8, sizeof(signaturesOffset));

  return signaturesOffset;
}


const char * read_whole_file(const char * file_name_utf8, size_t & size)
{
  size = 0;
  if (!file_name_utf8)
    return nullptr;
  FILE * f = FS_FOPEN(file_name_utf8, "rb");
  if (!f)
    return nullptr;

  if (FS_FSEEK(f, 0, SEEK_END) != 0)
  {
    fclose(f);
    return nullptr;
  }

  size = FS_FTELL(f);

  if (FS_FSEEK(f, 0, SEEK_SET) != 0)
  {
    fclose(f);
    size = 0;
    return nullptr;
  }

  char * data = new char[size];
  if (size > 0)
    if (fread(data, size, 1, f) != 1)
    {
      delete[] data;
      size = 0;
      fclose(f);
      return nullptr;
    }

  fclose(f);
  return data;
}


static void fhash_block(uint32_t * block, int size_in_bytes, uint32_t & hash)
{
  uint32_t res = hash;
  int size = size_in_bytes / sizeof(uint32_t);
  for (int i = 0; i < size; i++)
    res += block[i] + res * 33 + 1 + (res >> 6);
  hash = res;
}

bool sign_file_fhash(const string & file_name_utf8)
{
  vector<char> data(65536 * 2);
  FILE * f = FS_FOPEN(file_name_utf8.c_str(), "rb+");
  if (!f)
    return false;

  uint32_t hash = 0;
  size_t readBytes = 0;

  while ((readBytes = fread(&data[0], 1, data.size(), f)) > 0)
    fhash_block((uint32_t *)&data[0], int(readBytes), hash);

  uint32_t signSize = 4 + 4 + 4;
  uint32_t signType = 1;
  fwrite(&signSize, 4, 1, f);
  fwrite(&signType, 4, 1, f);
  if (fwrite(&hash, 4, 1, f) != 1)
  {
    fclose(f);
    return false;
  }
  fclose(f);
  return true;
}

bool convert_file_to_hex32(const string & file_name_utf8)
{
  vector<char> data(65536);
  FILE * f = FS_FOPEN(file_name_utf8.c_str(), "rb");
  if (!f)
    return false;

  FILE * hexf = FS_FOPEN((file_name_utf8 + ".hex.tmp").c_str(), "wt");
  if (!hexf)
    return false;

  uint32_t hash = 0;
  size_t readBytes = 0;

  while ((readBytes = fread(&data[0], 1, data.size(), f)) > 0)
  {
    readBytes = int(((readBytes - 1) | (sizeof(uint32_t) - 1)) + 1);
    int cnt = int(readBytes / sizeof(uint32_t));
    const uint32_t * p = (const uint32_t *)&data[0];
    for (int i = 0; i < cnt; i++, p++)
    {
      fprintf(hexf, "0x%X,", *p);
      if ((i & 15) == 15 || (*p & 0xFF) == '.')
        fprintf(hexf, "\n");
    }
  }

  fclose(hexf);
  fclose(f);

  FS_UNLINK(file_name_utf8.c_str());
  FS_RENAME((file_name_utf8 + ".hex.tmp").c_str(), file_name_utf8.c_str());

  return true;
}


struct Fs8Partition
{
  bool isInMemory = false;
  string fileName;
  FILE * fileDescriptor = nullptr;
  uint64_t fileTime = 0;
  chrono::time_point<chrono::steady_clock> lastAccessTime;

  const char * inMemoryDataPtr = nullptr;
  int64_t inMemorySize = 0;
  int useCount = 0;
  FileInfosMap fileInfos;
  recursive_mutex decompression_lock;

  ~Fs8Partition()
  {
    lock_guard<recursive_mutex> lock(decompression_lock);
    for (auto inf : fileInfos)
    {
      char * decompressedFile = (char *)inf.second.getDecompressedPtr();
      delete[] decompressedFile;
      inf.second.resetPtr();
    }

    if (fileDescriptor)
      fclose(fileDescriptor);
  }
};



static struct PartitionsContainer
{
  vector<Fs8Partition *> partitions;

  ~PartitionsContainer()
  {
    lock_guard<recursive_mutex> lock(partitions_lock);

    for (auto & p : partitions)
    {
      {
        lock_guard<recursive_mutex> lock(p->decompression_lock);
        for (auto & info : p->fileInfos)
        {
          char * ptr = (char *)info.second.getDecompressedPtr();
          delete[] ptr;
          info.second.resetPtr();
        }
      }
      delete p;
      p = nullptr;
    }
    partitions.clear();
  }


  bool deserializeFileInfos(FileInfosMap & fs_file_infos, const vector<char> & bytes)
  {
    fs_file_infos.clear();
    if (bytes.empty())
      return false;
    const char * cursor = &bytes[4]; // skip size
    int bytes_left = int(bytes.size()) - 4;

    uint16_t fileNameLength = 0;

    while (bytes_left > 0)
    {
      if (!read_bytes(&cursor, bytes_left, &fileNameLength, sizeof(fileNameLength)))
      {
        Fs8FileSystem::errorLogCallback("Corrupted file (cannot read fileNameLength)");
        return false;
      }

      if (fileNameLength > 512)
      {
        Fs8FileSystem::errorLogCallback("Corrupted file (fileNameLength > 512)");
        return false;
      }

      string s;
      s.resize(fileNameLength);

      if (!read_bytes(&cursor, bytes_left, &s[0], fileNameLength))
      {
        Fs8FileSystem::errorLogCallback("Corrupted file (cannot read fileName)");
        return false;
      }

      Fs8FileInfo fileInfo;

      if (!read_bytes(&cursor, bytes_left, &fileInfo, sizeof(int64_t) * 3))
      {
        Fs8FileSystem::errorLogCallback("Corrupted file (cannot read fileInfo)");
        return false;
      }

      fileInfo.resetPtr();

      fs_file_infos[s] = fileInfo;
    }

    return bytes_left == 0;
  }



  // will increment use counter
  Fs8Partition * findOrInitializePartitionFn(const char * fs8_file_name_utf8)
  {
    if (!fs8_file_name_utf8 || !fs8_file_name_utf8[0])
    {
      Fs8FileSystem::errorLogCallback("Empty file name");
      return nullptr;
    }

    lock_guard<recursive_mutex> lock(partitions_lock);

    Fs8Partition * recreatePartition = nullptr;

    string fname = fs8_file_name_utf8;
    for (Fs8Partition * p : partitions)
      if (fname == p->fileName)
      {
        if (!p->fileDescriptor)
        {
          uint64_t curFileTime = get_file_time(fs8_file_name_utf8);
          if (p->fileTime != curFileTime)
          {
            recreatePartition = p;
            break;
          }

          p->fileDescriptor = FS_FOPEN(fs8_file_name_utf8, "rb");
          if (!p->fileDescriptor)
          {
            Fs8FileSystem::errorLogCallback((string("Cannot open file ") + fs8_file_name_utf8).c_str());
            return nullptr;
          }
        }

        p->lastAccessTime = chrono::steady_clock::now();
        p->useCount++;
        return p;
      }

    recursive_mutex tmp;
    lock_guard<recursive_mutex> decompressionLock(recreatePartition ? recreatePartition->decompression_lock : tmp);


    FILE * f = FS_FOPEN(fs8_file_name_utf8, "rb");
    if (!f)
    {
      Fs8FileSystem::errorLogCallback((string("Cannot open file ") + fs8_file_name_utf8).c_str());
      return nullptr;
    }

    char buf[24] = { 0 };
    if (fread(buf, 24, 1, f) != 1)
    {
      Fs8FileSystem::errorLogCallback((string("Cannot read file ") + fs8_file_name_utf8).c_str());
      fclose(f);
      return nullptr;
    }

    int64_t fileNamesOffset = check_header_get_file_names_offset(buf);
    if (fileNamesOffset <= 0)
    {
      Fs8FileSystem::errorLogCallback((string("Not FS8 file ") + fs8_file_name_utf8).c_str());
      fclose(f);
      return nullptr;
    }

    if (FS_FSEEK(f, fileNamesOffset, SEEK_SET) != 0)
    {
      Fs8FileSystem::errorLogCallback((string("Corrupted file ") + fs8_file_name_utf8).c_str());
      fclose(f);
      return nullptr;
    }

    uint32_t fnlen = 0;
    if (FS_FSEEK(f, fileNamesOffset, SEEK_SET) != 0 || fread(&fnlen, sizeof(fnlen), 1, f) != 1 || fnlen > FS_MAX_FILENAMES_BINARY_SIZE)
    {
      Fs8FileSystem::errorLogCallback((string("Corrupted file ") + fs8_file_name_utf8).c_str());
      fclose(f);
      return nullptr;
    }

    FS_FSEEK(f, -4, SEEK_CUR);

    vector<char> fileNamesData;
    fileNamesData.resize(fnlen + 4);
    if (fread(&fileNamesData[0], fnlen + 4, 1, f) != 1)
    {
      Fs8FileSystem::errorLogCallback((string("Corrupted file ") + fs8_file_name_utf8).c_str());
      fclose(f);
      return nullptr;
    }

    Fs8Partition * partition = recreatePartition ? recreatePartition : new Fs8Partition;
    partition->fileName = fname;
    partition->isInMemory = false;
    partition->fileDescriptor = f;
    partition->useCount++;


    if (!deserializeFileInfos(partition->fileInfos, fileNamesData))
    {
      if (!recreatePartition)
        delete partition;
      Fs8FileSystem::errorLogCallback((string("Corrupted file ") + fs8_file_name_utf8).c_str());
      fclose(f);
      return nullptr;
    }

    if (!recreatePartition)
    {
      if (partitions.empty())
      {
        partitions.reserve(FS_MAX_PARTITION);
      }
      partitions.push_back(partition);
    }

    return partition;
  }


  Fs8Partition * findOrInitializePartitionMem(const void * mem, int64_t size = -1)
  {
    if (!mem)
    {
      Fs8FileSystem::errorLogCallback("Invalid pointer");
      return nullptr;
    }

    lock_guard<recursive_mutex> lock(partitions_lock);

    for (Fs8Partition * p : partitions)
      if (mem == p->inMemoryDataPtr)
      {
        p->useCount++;
        return p;
      }

    int64_t fileNamesOffset = check_header_get_file_names_offset((const char *)mem);
    if (fileNamesOffset <= 0)
    {
      Fs8FileSystem::errorLogCallback("Not FS8 file");
      return nullptr;
    }

    if (size > 0 && fileNamesOffset + 4 > size)
    {
      Fs8FileSystem::errorLogCallback("Invalid file format");
      return nullptr;
    }

    uint32_t fnlen = 0;
    memcpy(&fnlen, (const char *)mem + fileNamesOffset, sizeof(fnlen));

    if (size > 0 && fileNamesOffset + 4 + fnlen > size)
    {
      Fs8FileSystem::errorLogCallback("Invalid file format");
      return nullptr;
    }

    vector<char> fileNamesData((const char *)mem + fileNamesOffset, (const char *)mem + fileNamesOffset + fnlen);

    Fs8Partition * partition = new Fs8Partition;
    partition->isInMemory = true;
    partition->fileDescriptor = nullptr;
    partition->inMemorySize = size;
    partition->inMemoryDataPtr = (const char *)mem;

    if (!deserializeFileInfos(partition->fileInfos, fileNamesData))
    {
      delete partition;
      Fs8FileSystem::errorLogCallback("Invalid file format");
      return nullptr;
    }

    if (partitions.empty())
      partitions.reserve(FS_MAX_PARTITION);
    partitions.push_back(partition);
    return partition;
  }


  void unusePartition(Fs8Partition * partition)
  {
    if (!partition)
      return;

    lock_guard<recursive_mutex> lock(partitions_lock);

    partition->useCount--;
    if (partition->useCount < 0)
      Fs8FileSystem::errorLogCallback("Internal error (partition->useCount < 0)");

    if (partition->useCount <= 0 && partition->fileDescriptor)
    {
      fclose(partition->fileDescriptor);
      partition->fileDescriptor = nullptr;
    }
  }

  static void checkPartionFileTime(Fs8Partition *& partition)
  {
    if (partition && partition->fileDescriptor)
    {
      auto elapsedTime = chrono::steady_clock::now() - partition->lastAccessTime;
      int64_t msecAfterLastAccess = abs(chrono::duration_cast<chrono::milliseconds>(elapsedTime).count());
      if (msecAfterLastAccess > FS_UNLOCK_FILE_AFTER_MS)
      {
        lock_guard<recursive_mutex> lock(partitions_lock);
        uint64_t curFileTime = get_file_time(partition->fileName.c_str());
        if (partition->fileTime != curFileTime)
        {
          fclose(partition->fileDescriptor);
          partition->fileDescriptor = nullptr;
        }
      }
    }
  }


  chrono::time_point<chrono::steady_clock> msecRef;

  void act()
  {
    auto now = chrono::steady_clock::now();
    auto elapsedTime = now - msecRef;
    int64_t dt = abs(chrono::duration_cast<chrono::milliseconds>(elapsedTime).count());
    if (dt > 100)
    {
      msecRef = now;
      lock_guard<recursive_mutex> lock(partitions_lock);
      bool hasFileDescriptors = false;

      for (auto & p : partitions)
        if (p->fileDescriptor)
        {
          PartitionsContainer::checkPartionFileTime(p);
          if (p->fileDescriptor)
            hasFileDescriptors = true;
        }
    }
  }

} file_systems_container;


static void normalize_file_name(string & name)
{
  for (char & ch : name)
  {
    ch = tolower(ch);
    if (ch == '\\')
      ch = '/';
  }
}


void Fs8FileSystem::act()
{
  file_systems_container.act();
}


Fs8FileSystem::Fs8FileSystem()
{
}

bool Fs8FileSystem::checkFs8FileSystemSignatures(const char * fs8_file_name_utf8)
{
  vector<char> data(65536 * 2);
  FILE * f = FS_FOPEN(fs8_file_name_utf8, "rb");
  if (!f)
    return false;

  if (fread(&data[0], 32, 1, f) != 1)
  {
    fclose(f);
    return false;
  }

  fread(&data[0], 32, 1, f);
  int64_t pos = check_header_get_sign_offset(&data[0]);

  if (pos <= 0)
  {
    fclose(f);
    return false;
  }

  FS_FSEEK(f, pos, SEEK_SET);
  struct
  {
    uint32_t size;
    uint32_t type;
    uint32_t hash;
  } s;

  if (!fread(&s, 8, 1, f) != 1)
  {
    fclose(f);
    return false;
  }

  if (s.type == 1 && fread(&s.hash, sizeof(s.hash), 1, f) == 1)
  {
    FS_FSEEK(f, 0, SEEK_SET);
    size_t readBytes = 0;
    uint32_t hash = 0;
    int64_t p = pos;
    while (p > 0 && (readBytes = fread(&data[0], 1, min(int64_t(data.size()), p), f)) > 0)
    {
      fhash_block((uint32_t *)&data[0], int(readBytes), hash);
      p -= int(data.size());
    }

    return hash == s.hash;
  }

  return false;
}


bool Fs8FileSystem::createFs8FromFiles(const char * dir_, const vector<string> & file_names,
  const char * out_file_name_utf8_, int compression_level, bool write_as_hex32)
{
  vector<pair<string, string>> namePairs;
  for (const auto & n : file_names)
    namePairs.emplace_back(make_pair(n, string()));
  return createFs8FromFiles(dir_, namePairs, out_file_name_utf8_, compression_level, write_as_hex32);
}


bool Fs8FileSystem::createFs8FromFiles(const char * dir_, const vector<pair<string, string>> & file_names,
  const char * out_file_name_utf8_, int compression_level, bool write_as_hex32)
{
  string dir(dir_);
  string out_file_name_utf8(out_file_name_utf8_);

  FileInfosMap fs_file_infos;

  if (!dir.empty() && dir.back() == '\\' || dir.back() == '/')
    dir.pop_back();

  FILE * outf = FS_FOPEN(out_file_name_utf8.c_str(), "wb");
  if (!outf)
  {
    Fs8FileSystem::errorLogCallback((string("Cannot open file for write ") + out_file_name_utf8).c_str());
    return false;
  }

  // ID: 4,  ver: 4,  file_table_offet: 8,  sigrantures_offset: 8
  const char * header = "FS8.1   ********XXXXXXXX";
  if (fwrite(header, 16, 1, outf) != 1)
  {
    Fs8FileSystem::errorLogCallback((string("Cannot write to file ") + out_file_name_utf8).c_str());
    fclose(outf);
    return false;
  }

  for (auto & namePair : file_names)
  {
    string name = namePair.first;
    string archiveName = namePair.second.empty() ? name : namePair.second;

    string fullName = dir.empty() ? name : dir + "/" + name;
    size_t fileSize = 0;
    const char * fileData = read_whole_file(fullName.c_str(), fileSize);
    if (!fileData)
    {
      Fs8FileSystem::errorLogCallback((string("Cannot read file ") + fullName).c_str());
      fclose(outf);
      return false;
    }

    size_t compressBounds = ZSTD_compressBound(fileSize);
    vector<char> compressedData(compressBounds);
    size_t compressedSize = ZSTD_compressCCtx(zstd_compress_context.get(), &compressedData[0], compressBounds,
      fileData, fileSize, compression_level);

    delete[] fileData;
    fileData = nullptr;

    Fs8FileInfo info;
    info.compressedSize = int64_t(compressedSize);
    info.decompressedSize = fileSize;
    info.offsetInFile = FS_FTELL(outf);

    if (info.compressedSize > 0)
      if (fwrite(&compressedData[0], compressedSize, 1, outf) != 1)
      {
        Fs8FileSystem::errorLogCallback((string("Cannot write to file ") + out_file_name_utf8).c_str());
        fclose(outf);
        return false;
      }

    fs_file_infos[archiveName] = info;
  }

  int64_t fnamesPos = FS_FTELL(outf);

  vector<char> fnames;
  if (!serialize_fs_file_infos(fs_file_infos, fnames) || fwrite(&fnames[0], fnames.size(), 1, outf) != 1)
  {
    Fs8FileSystem::errorLogCallback((string("Cannot write to file ") + out_file_name_utf8).c_str());
    fclose(outf);
    return false;
  }

  int64_t signaturesPos = FS_FTELL(outf);
  if (signaturesPos % 8 != 0)
  {
    fwrite(&signaturesPos, 8 - signaturesPos % 8, 1, outf);
    signaturesPos = FS_FTELL(outf);
  }

  FS_FSEEK(outf, 8, SEEK_SET);
  fwrite(&fnamesPos, sizeof(fnamesPos), 1, outf);
  fwrite(&signaturesPos, sizeof(signaturesPos), 1, outf);
  fclose(outf);

  if (!sign_file_fhash(out_file_name_utf8.c_str()))
  {
    Fs8FileSystem::errorLogCallback((string("Cannot sign file ") + out_file_name_utf8).c_str());
    return false;
  }

  if (write_as_hex32)
    if (!convert_file_to_hex32(out_file_name_utf8.c_str()))
    {
      Fs8FileSystem::errorLogCallback((string("Cannot convert file to hex32 ") + out_file_name_utf8).c_str());
      return false;
    }

  return true;
}


bool Fs8FileSystem::initalizeFromFile(const char * fs8_file_name_utf8)
{
  string fullName = get_absolute_file_name(fs8_file_name_utf8);

  lock_guard<recursive_mutex> lock(partitions_lock);
  if (partition)
    file_systems_container.unusePartition(partition);
  partition = file_systems_container.findOrInitializePartitionFn(fullName.c_str());
  return partition != nullptr;
}

bool Fs8FileSystem::initalizeFromMemory(void * data, int64_t size)
{
  lock_guard<recursive_mutex> lock(partitions_lock);
  if (partition)
    file_systems_container.unusePartition(partition);
  partition = file_systems_container.findOrInitializePartitionMem(data, size);
  return partition != nullptr;
}

bool Fs8FileSystem::isFileExists(const char * file_name)
{
  if (!partition || !file_name)
    return false;
  string fname(file_name);
  normalize_file_name(fname);
  partition->lastAccessTime = chrono::steady_clock::now();
  lock_guard<recursive_mutex> lock(partition->decompression_lock);
  return partition->fileInfos.find(fname) != partition->fileInfos.end();
}

int64_t Fs8FileSystem::getFileSize(const char * file_name)
{
  if (!partition || !file_name)
    return false;
  string fname(file_name);  
  normalize_file_name(fname);
  partition->lastAccessTime = chrono::steady_clock::now();
  lock_guard<recursive_mutex> lock(partition->decompression_lock);
  auto it = partition->fileInfos.find(fname);
  if (it != partition->fileInfos.end())
    return it->second.decompressedSize;
  else
    return 0;
}

bool Fs8FileSystem::getFileBytes(const char * file_name, void * to_buffer, int64_t buffer_size)
{
  if (to_buffer == 0)
    return false;

  if (!partition)
  {
    Fs8FileSystem::errorLogCallback("Internal error (partition == null, createFs8 was not called ?)");
    return false;
  }

  if (!file_name)
    return false;

  string fname(file_name);
  normalize_file_name(fname);
  partition->lastAccessTime = chrono::steady_clock::now();

  auto it = partition->fileInfos.find(fname);
  if (it == partition->fileInfos.end() || it->second.decompressedSize > buffer_size)
    return false;

  Fs8FileInfo & info = it->second;

  lock_guard<recursive_mutex> lock(partition->decompression_lock);
  void * p = info.getDecompressedPtr();
  if (p)
  {
    memcpy(to_buffer, p, info.decompressedSize);
    return true;
  }

  if (info.decompressedSize == 0)
  {
    return true;
  }

  if (partition->isInMemory)
  {
    if (partition->inMemorySize > 0 && info.offsetInFile + info.compressedSize > partition->inMemorySize)
    {
      Fs8FileSystem::errorLogCallback("Internal error (invalid partition->inMemorySize)");
      return false;
    }

    size_t res = ZSTD_decompressDCtx(zstd_decompress_context.get(), to_buffer, info.decompressedSize,
      partition->inMemoryDataPtr + info.offsetInFile, info.compressedSize);

    if (ZSTD_isError(res))
    {
      Fs8FileSystem::errorLogCallback("ZTDS decompression error (1)");
      return false;
    }

    if (info.decompressedSize < FS_KEEP_IN_MEMORY_THRESHOLD)
    {
      char * ptr = new char[info.decompressedSize];
      memcpy(ptr, to_buffer, info.decompressedSize);
      info.setDecompressedPtr(ptr);
    }

    return true;
  }
  else
  {
    if (!partition->fileDescriptor)
    {
      Fs8FileSystem::errorLogCallback("partition->fileDescriptor is closed");
      return false;
    }

    FS_FSEEK(partition->fileDescriptor, info.offsetInFile, SEEK_SET);
    vector<char> compressedData;
    compressedData.resize(info.compressedSize);
    if (fread(&compressedData[0], info.compressedSize, 1, partition->fileDescriptor) != 1)
    {
      Fs8FileSystem::errorLogCallback("Cannot read from file");
      return false;
    }

    size_t res = ZSTD_decompressDCtx(zstd_decompress_context.get(), to_buffer, info.decompressedSize,
      &compressedData[0], info.compressedSize);

    if (ZSTD_isError(res))
    {
      Fs8FileSystem::errorLogCallback("ZTDS decompression error (2)");
      return false;
    }

    if (info.decompressedSize < FS_KEEP_IN_MEMORY_THRESHOLD)
    {
      char * ptr = new char[info.decompressedSize];
      memcpy(ptr, to_buffer, info.decompressedSize);
      info.setDecompressedPtr(ptr);
    }

    return true;
  }
}


bool Fs8FileSystem::getFileBytes(const char * file_name, vector<char> & out_file_bytes, bool addFinalZero)
{
  if (!partition)
  {
    Fs8FileSystem::errorLogCallback("Internal error (partition == null, createFs8 was not called ?)");
    return false;
  }

  if (!file_name)
    return false;

  lock_guard<recursive_mutex> lock(partition->decompression_lock);
  int64_t fileSize = getFileSize(file_name);
  if (addFinalZero)
  {
    out_file_bytes.resize(fileSize + 1);
    out_file_bytes.back() = 0;
  }
  else
  {
    out_file_bytes.resize(fileSize + 1);
  }
  bool res = getFileBytes(file_name, &out_file_bytes[0], fileSize);
  if (!res)
    out_file_bytes.clear();
  return res;
}

Fs8FileSystem::~Fs8FileSystem()
{
  file_systems_container.unusePartition(partition);
  partition = nullptr;
}


static void default_log_error(const char * log_string)
{
  printf("\nFS8: %s\n", log_string);
}

Fs8ErrorLogCallback Fs8FileSystem::errorLogCallback = default_log_error;

