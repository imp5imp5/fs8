#include "../library/fs8.h"
#include "../library/fs8.cpp"


static bool is_dir_exist(const string& path)
{
#if defined(_WIN32)
  wstring wName = string_to_wstring(path);
  struct _stat64 info;
  if (_wstat64(wName.c_str(), &info) != 0)
  {
    return false;
  }
  return (info.st_mode & _S_IFDIR) != 0;
#else 
  struct stat info;
  if (stat(path.c_str(), &info) != 0)
  {
    return false;
  }
  return (info.st_mode & S_IFDIR) != 0;
#endif
}

static bool make_path(string path)
{
  if (is_dir_exist(path))
    return true;

  for (auto & ch : path)
    if (ch == '\\')
      ch = '/';

  if (path.back() == '/')
    path.pop_back();

#if defined(_WIN32)
  wstring wName = string_to_wstring(path);
  int ret = _wmkdir(wName.c_str());
#else
  mode_t mode = 0755;
  int ret = mkdir(path.c_str(), mode);
#endif
  if (ret == 0)
    return true;

  switch (errno)
  {
  case ENOENT:
  // parent didn't exist, try to create it
  {
    size_t pos = path.find_last_of('/');
    if (pos == std::string::npos)
      return false;
    if (!make_path(path.substr(0, pos) ))
      return false;
  }
  // now, try to create again
#if defined(_WIN32)
  wName = string_to_wstring(path);
  return _wmkdir(wName.c_str()) == 0;
#else 
  return mkdir(path.c_str(), mode) == 0;
#endif

  case EEXIST:
    return is_dir_exist(path);

  default:
    return false;
  }
}



void usage()
{
  printf("Usage: fs8extract <archive.fs8> [--list:list-of-files.txt] [--dir:extract-to-dir] [--all] [--size-limit:limit] [--just-show-files] [file-name1] [file-name2]\n"
    "\n"
    "List of files - just list of file names in archive, each file on the new line.\n"
    "\n"
  );
}


int main(int argc, char ** argv)
{
  char * filesListFn = nullptr;
  string extractToDir = ".";
  bool extractAll = false;
  bool justShowFiles = false;
  int64_t sizeLimit = -1;

  vector<const char *> arg;
  vector<string> fileNames;

  for (int i = 1; i < argc; i++)
    if (argv[i][0] != '-')
      arg.push_back(argv[i]);
    else if (!strcmp(argv[i], "--all"))
      extractAll = true;
    else if (!strcmp(argv[i], "--just-show-files"))
      justShowFiles = true;
    else if (!strncmp(argv[i], "--list:", 7))
      filesListFn = argv[i] + 7;
    else if (!strncmp(argv[i], "--dir:", 6))
      extractToDir = argv[i] + 6;
    else if (!strncmp(argv[i], "--size-limit:", 16))
      sizeLimit = atoi(argv[i] + 16);
    else
    {
      printf("ERROR: Unknown argument %s\n", argv[i]);
      return 1;
    }

  if (arg.size() < 1)
  {
    usage();
    return 1;
  }

  const char * archiveFileName = arg[0];
  Fs8FileSystem fs;
  if (!fs.initalizeFromFile(archiveFileName))
    return 1;

  if (!make_path(extractToDir))
  {
    printf("ERROR: Cannot create directory %s\n", extractToDir.c_str());
    return 1;
  }

  if (justShowFiles)
  {
    fs.getAllFileNames(fileNames);
    sort(fileNames.begin(), fileNames.end());
    for (auto & n : fileNames)
      printf("%s\n", n.c_str());
    return 0;
  }

  if (extractAll)
  {
    fs.getAllFileNames(fileNames);
    if (fileNames.empty())
    {
      printf("ERROR: Archive '%s' is empty\n\n", archiveFileName);
      return 1;
    }
  }
  else if (filesListFn)
  {
    FILE * listf = FS_FOPEN(filesListFn, "rt");
    if (!listf)
    {
      printf("ERROR: Cannot open file %s\n", filesListFn);
      return 1;
    }

    char buf[1024] = { 0 };

    while (fgets(buf, sizeof(buf) - 1, listf))
    {
      if (char * p = strchr(buf, '\n'))
        *p = 0;
      if (char * p = strchr(buf, '\r'))
        *p = 0;
      if (char * p = strchr(buf, ' '))
        *p = 0;
      if (strlen(buf) > 0)
        fileNames.push_back(string(buf));
    }

    fclose(listf);
  }

  if (fileNames.empty())
  {
    printf("ERROR: Expected '--all' or file names to extract\n\n");
    return 1;
  }

  sort(fileNames.begin(), fileNames.end());
  string prevDirectory;
  int64_t sizeSum = 0;
  for (auto & n : fileNames)
  {
    size_t pos = n.find_last_of('/');
    string directory = (pos == std::string::npos) ? string() : n.substr(0, pos);
    if (directory != prevDirectory)
    {
      prevDirectory = directory;
      if (!directory.empty())
        if (!make_path(extractToDir + "/" + directory))
        {
          printf("ERROR: Cannot create directory %s\n", extractToDir.c_str());
          return 1;
        }
    }

    int64_t size = fs.getFileSize(n.c_str());
    sizeSum += size;
    if (sizeLimit > 0 && sizeSum > sizeLimit)
    {
      printf("ERROR: Total size of extracted files is out of limit\n");
      return 1;
    }

    std::vector<char> bytes;
    if (!fs.getFileBytes(n.c_str(), bytes, false))
    {
      printf("ERROR: Cannot extract file %s\n", n.c_str());
      return 1;
    }
    
    string fullName = string(extractToDir) + "/" + n;
    FILE * savef = FS_FOPEN(fullName.c_str(), "wb");
    if (!savef)
    {
      printf("ERROR: Cannot create file %s\n", fullName.c_str());
      return 1;
    }

    if (bytes.size() > 0 && fwrite(&bytes[0], bytes.size(), 1, savef) != 1)
    {
      printf("ERROR: Cannot write to file %s\n", fullName.c_str());
      fclose(savef);
      return 1;
    }

    fclose(savef);
  }
  
  printf("Extracted %d file(s)\n", int(fileNames.size()));

  return 0;
}
