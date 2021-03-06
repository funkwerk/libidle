#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

int main()
{
  int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  int yesReuseAddr = true;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yesReuseAddr, sizeof(int));
  struct sockaddr_in sa = {
    .sin_family = AF_INET,
    .sin_port = htons(12345),
    .sin_addr.s_addr = htonl(INADDR_ANY),
  };
  if (bind(fd, (struct sockaddr*) &sa, sizeof(sa)) == -1)
  {
    perror("bind");
    exit(EXIT_FAILURE);
  }
  if (listen(fd, 1) == -1)
  {
    perror("listen");
    exit(EXIT_FAILURE);
  }
  accept(fd, NULL, NULL);
}
