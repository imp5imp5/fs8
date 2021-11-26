#pragma once

#include <stdint.h>
#include <vector>

struct Fs8Partition;

typedef void (* Fs8ErrorLogCallback)(const char *);

struct Fs8FileSystem
{
  static Fs8ErrorLogCallback errorLogCallback; // printf by default

  Fs8FileSystem();
  ~Fs8FileSystem();

  static bool checkFs8FileSystemSignatures(const char * fs8_file_name_utf8);

  static bool createFs8FromFiles(const char * dir_, const std::vector<std::string> & file_names,
    const char * out_file_name_utf8_, int compression_level = 1, bool write_as_hex32 = false,
    std::vector<std::string> * ignore_list = nullptr);

  // list of pairs (original file name, archive file name)
  static bool createFs8FromFiles(const char * dir_, const std::vector<std::pair<std::string, std::string>> & file_names,
    const char * out_file_name_utf8_, int compression_level = 1, bool write_as_hex32 = false,
    std::vector<std::string> * ignore_list = nullptr);

  bool initalizeFromFile(const char * fs8_file_name_utf8);
  bool initalizeFromMemory(void * data, int64_t size = -1);
  void getAllFileNames(std::vector<std::string> & out_file_names);
  bool isFileExists(const char * file_name);
  int64_t getFileSize(const char * file_name);
  bool getFileBytes(const char * file_name, std::vector<char> & out_file_bytes, bool addFinalZero = false);
  bool getFileBytes(const char * file_name, void * to_buffer, int64_t buffer_size);

  static void act();

private:
  Fs8Partition * partition = nullptr;
};
