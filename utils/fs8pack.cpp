#include "../library/fs8.h"
#include "../library/fs8.cpp"

static bool hex_output = false;
static int compression_level = 1;

void usage()
{
  printf("Usage: fs8pack [--hex] [--level:N] <initial-directory> <list-of-files.txt> <out-file-name.fs8>\n"
    "\n"
    "List of files - just list of <file-name> or <file-name> <file-name-in-archive>, each file on the new line.\n"
    "--hex - output as ASCII array of integers.\n"
    "--level:N - zstd compression level (1 by default).\n"
    "\n"
  );
}

int main(int argc, char ** argv)
{
  vector<const char *> arg;

  for (int i = 1; i < argc; i++)
    if (argv[i][0] != '-')
      arg.push_back(argv[i]);
    else if (!strcmp(argv[i], "--hex"))
      hex_output = true;
    else if (!strncmp(argv[i], "--level:", 8))
      compression_level = atoi(argv[i] + 8);

  if (arg.size() != 3)
  {
    usage();
    return 1;
  }

  const char * initialDir = arg[0];
  const char * listOfFiles = arg[1];
  const char * outFileName = arg[2];

  FILE * listf = FS_FOPEN(listOfFiles, "rt");
  if (!listf)
  {
    printf("ERROR: cannot open file %s\n", listOfFiles);
    return 1;
  }

  vector<pair<string, string>> fileNames;

  char buf[1024] = { 0 };

  while (fgets(buf, sizeof(buf) - 1, listf))
  {
    if (char * p = strchr(buf, '\n'))
      *p = 0;
    if (char * p = strchr(buf, '\r'))
      *p = 0;
    if (strlen(buf) > 0)
    {
      if (char * p = strchr(buf, ' '))
      {
        while (*p == ' ')
        {
          *p = 0;
          *p++;
        }

        if (char * sp = strchr(p, ' '))
          *sp = 0;

        fileNames.push_back(make_pair(string(buf), string(p)));
      }
      else
      {
        if (char * sp = strchr(buf, ' '))
          *sp = 0;

        fileNames.push_back(make_pair(string(buf), string()));
      }
    }
  }

  fclose(listf);

  if (!Fs8FileSystem::createFs8FromFiles(initialDir, fileNames, outFileName, compression_level, hex_output))
    return 1;

  printf("%d file(s) packed with compression level %d\n", int(fileNames.size()), compression_level);

  return 0;
}
