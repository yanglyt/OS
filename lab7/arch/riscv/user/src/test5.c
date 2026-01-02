#include "fs.h"
#include "getchar.h"
#include "mm.h"
#include "proc.h"
#include "stdio.h"

int getchar_until_valid();
int strcmp(const char *a, const char *b);
void strcpy(char *a, const char *b);

void path_add_entry(char *path, const char *entry);

int main() {
  char input[64];
  int n = 0, ch;

  char *path = 0x0;
  mmap(0, 1024, PTE_V | PTE_U | PTE_R | PTE_W, 0, 0, 0);
  path[0] = '/';
  path[1] = '\0';

  char tmp[10][28];
  char *filename[10];
  for (int i = 0; i < 10; i++)
    filename[i] = tmp[i];

  char *copy = (char *)0x10000;
  mmap(copy, 1024, PTE_V | PTE_U | PTE_R | PTE_W, 0, 0, 0);

  char * content = (char *)0x1000;
  mmap(content, 0x1000, PTE_V | PTE_R | PTE_W | PTE_U, 0, 0, 0);

  printf("fssh support: \n");
  printf("> ls\n");
  printf("> cd (only support): cd .   cd ..   cd filename\n");
  printf("> exit\n");
  printf("> cat filename\n");
  printf("> pwd\n");
  printf("> echo filename content\n");
  printf("> echo dir/filename content\n");

  for (;;) {
    n = 0;

    printf("lab7@oslab: %s $ ", path);
    for (;;) {
      ch = getchar_until_valid();
      printf("%c", ch);

      switch (ch) {
      case '\r' /* enter */:
        // note that: enter_key --> \r\n
        printf("\n");
        goto input_end;
      case 127 /* delete */:
        if (n) {
          n--;
          printf("\b \b");
        }
        break;
      default:
        input[n++] = ch;
      }
    }
  input_end:
    input[n] = '\0';

    if (strcmp(input, "ls") == 0) {
      int len = sfs_get_files(path, filename);
      if (len < 0) {
        printf("ls failed");
        while(1);
      }
      for (int i = 0; i < len; i++)
        printf("%s ", filename[i]);
      printf("\n");
    }

    if (strcmp(input, "pwd") == 0) {
      printf("%s\n", path);
    }

    if (input[0] == 'c' && input[1] == 'd') {
      // 检查目标是否是目录
      char check_path[64];
      strcpy(check_path, path);
      path_add_entry(check_path, input + 3);
      
      // 尝试获取文件列表来检查是否是目录
      int len = sfs_get_files(check_path, filename);
      if (len < 0) {
          printf("cd: %s: not a directory\n", input + 3);
      } 
      else {
          // 是目录，更新路径
          path_add_entry(path, input + 3);
      }
    }

    if (strcmp(input, "exit") == 0) {
      exit(0);
    }

    if (input[0] == 'c' && input[1] == 'a' && input[2] == 't') {
      strcpy(copy, path);
      path_add_entry(copy, input + 4);
      int fd = sfs_open(copy, SFS_FLAG_READ);
      if (fd < 0) {
        printf("%s not found\n", copy);
        continue;
      }
      int len = sfs_read(fd, content, 20);
      for (int i = 0; i < len; i++) {
        printf("%c", content[i]);
      }
      printf("\n");
      sfs_close(fd);
    }

    if (input[0] == 'e' && input[1] == 'c' && input[2] == 'h' && input[3] == 'o') {
      int content_start = 0;
      for (int i = 5; i < n; i++) {
        if (input[i] == ' ') {
          input[i] = '\0';
          content_start = i + 1;
          break;
        }
      }
      strcpy(copy, path);
      path_add_entry(copy, input + 5);
      int fd = sfs_open(copy, SFS_FLAG_WRITE | SFS_FLAG_READ);
      if (fd < 0) {
        printf("%s not found\n", copy);
        continue;
      }
      strcpy(content, input + content_start);
      int len = sfs_write(fd, content, n - content_start);
      for (int i = 0; i < len; i++) {
        printf("%c", content[i]);
      }
      printf("\n");
      sfs_close(fd);
    }
  }
  return 0;
}

void path_add_entry(char *path, const char *entry) {
    // 如果entry为空或只有空格，直接返回
    if (entry == 0 || *entry == '\0') {
        return;
    }
    
    // 处理 . 和 ..
    if (strcmp(entry, ".") == 0) {
        return;  // 当前目录，不做任何事
    }
    
    if (strcmp(entry, "..") == 0) {
        // 回退到上一级目录
        int len = 0;
        while (path[len] != '\0') len++;
        
        if (len <= 1) {  // 已经是根目录 "/"
            return;
        }
        
        // 找到最后一个斜杠
        int last_slash = -1;
        for (int i = len - 1; i >= 0; i--) {
            if (path[i] == '/') {
                last_slash = i;
                break;
            }
        }
        
        if (last_slash >= 0) {
            // 如果最后一个斜杠后面还有字符，截断到该斜杠
            // 如果最后一个斜杠就是根目录的斜杠，保留为"/"
            if (last_slash == 0) {
                path[1] = '\0';
            } else {
                path[last_slash] = '\0';
            }
        }
        return;
    }
    
    // 普通文件/目录
    int len = 0;
    while (path[len] != '\0') len++;
    
    // 添加分隔符（如果当前路径不是以/结尾且不是根目录）
    if (len > 1 && path[len-1] != '/') {
        path[len] = '/';
        len++;
    } else if (len == 1 && path[0] == '/') {
        // 根目录情况，不需要添加额外的'/'
        // len保持为1
    } else if (path[len-1] == '/') {
        // 已经以/结尾，不需要添加
    } else {
        path[len] = '/';
        len++;
    }
    
    // 添加新条目
    int i = 0;
    while (entry[i] != '\0') {
        path[len + i] = entry[i];
        i++;
    }
    path[len + i] = '\0';
}

int strcmp(const char *a, const char *b) {
  while (*a && *b) {
    if (*a < *b)
      return -1;
    if (*a > *b)
      return 1;
    a++;
    b++;
  }
  if (*a && !*b)
    return 1;
  if (*b && !*a)
    return -1;
  return 0;
}

void strcpy(char *a, const char *b) {
  while (*b) {
    *a++ = *b++;
  }
  *a = '\0';
}

int getchar_until_valid() {
  int ch;
  do {
    ch = getchar();
  } while (ch <= 0);
  return ch;
}