#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <libgen.h>
#include <string.h>

#include <unistd.h>

#include <vmci_sockets.h>

#include "sock-portable.h"

BOOL DGRAM = FALSE;

enum {
    BUFSIZE = 4096,
};


char *program_name;

void print_usage_exit(void)
{
    fprintf(stderr, "Usage: %s [-d] <SERVER_CID:PORT>\n", program_name);
    fprintf(stderr, "  -d: use DATAGRAM mode instead of STREAM mode\n");
    exit(2);
}

int main(int argc, char **argv)
{
    struct sockaddr_vm their_addr = {0};
    int vmci_address_family;
    unsigned int server_cid = 0, server_port = 0;
    int sockfd = -1;
    char *p;

    program_name = basename(argv[0]);

    int option;
    while((option = getopt(argc, argv, ":dh")) != -1){ 
        switch(option){
        case 'd':
            DGRAM = TRUE;
            break;
        case 'h':
        case '?':
            print_usage_exit();
            break;
        }
    }

    for(; optind < argc; optind++){ //when some extra arguments are passed
        server_cid = (unsigned int)strtoul(argv[optind], &p, 0);
        server_port = (unsigned int)strtoul(p+1, &p, 0);
    }
     
    if (!server_cid || !server_port)
            print_usage_exit();
    
    socket_startup();

    if ((vmci_address_family = VMCISock_GetAFValue()) < 0) {
        fprintf(stderr, "VMCISock_GetAFValue failed: %d. You probably need root privileges\n", vmci_address_family);
        goto cleanup;
    }

    int socket_type;
    socket_type = DGRAM ? SOCK_DGRAM : SOCK_STREAM;

    if ((sockfd = socket(vmci_address_family, socket_type, 0)) == -1) {
        perror("socket");
        goto cleanup;
    }

    their_addr.svm_family = vmci_address_family;
    their_addr.svm_cid = server_cid; // in native byte order, i.e. little endian for the platforms that VMWare runs on
    their_addr.svm_port = server_port;
    fprintf(stderr, "connecting to %u:%u at %s mode\n", server_cid, server_port, DGRAM ? "DATAGRAM" : "STREAM");
    if (connect(sockfd, (struct sockaddr *) &their_addr, sizeof(their_addr)) == -1) {
        perror("connect");
        goto close;
    }

 	int numbytes;
    numbytes = send(sockfd, "hello\n", 6, 0);

    if (numbytes == -1) {
        perror("send");
        goto close;
    }

    fprintf(stderr, "sended %u bytes to %u:%u\n", numbytes, server_cid, server_port);
close:
    if (sockfd > 0)
        socket_close(sockfd);
cleanup:
    return 0;
}
