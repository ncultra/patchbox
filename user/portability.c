/***************************************************************
* Sandbox allows a user-space process to live-patch itself.
* Patches are placed in the "sandbox," which is a area in the
* .text segment
* 
* Copyright 2015-16 Rackspace, Inc.
***************************************************************/

#include "../sandbox.h"
#include "portability.h"

int copy_from_guest(void *dest, XEN_GUEST_HANDLE(fd), int size)
{
    return readn(fd, dest, (size_t)size);
}

int copy_to_guest(XEN_GUEST_HANDLE(fd), void *src, int size)
{
    return writen(fd, src, (size_t)size);
    
}

static char sockname[PATH_MAX];
static int sockfd;

/*******************************************************************
 * sandbox_name, AKA sockname, defines the path to the domain socket
 * that provides the other end of the live patching interface.
 * each QEMU instance will have a unique sandbox_name comprised of 
 * the path to the socket, and the owners process id
 ******************************************************************/

char *get_sandbox_name(void)
{
    return strdup(sockname);
    
}

void set_sandbox_name(char *name)
{
    strncpy(sockname, name, PATH_MAX);
}


/* use a wrapper function so we can eventually support other media beyond */
/* a domain socket, eg sysfs file */
int connect_to_sandbox(char *sandbox_name)
{
	return client_func(sandbox_name);	
}



int open_xc(xc_interface_t *xch)
{

    if (sockfd <= 0) {
        sockfd = connect_to_sandbox(sockname);
    }
    sockfd = connect_to_sandbox(sockname);

    *xch = sockfd;
    if (sockfd < 0) {
        printf("xc_interface_open failed\n");    
        return -1;
    }   
    return 0;
}

/* TODO: conditionally compile i raxlpxs.c, remove from this file 
 * right now its only a stub
 */
int do_xen_hypercall(xc_interface_t xc, void *buf)
{
    return 0;
}


/* return: < 0 for error; zero if patch not applied; one if patch applied */
/* if sha1 is NULL return all applied patches
 * return an array of xenlp_patch_info structs
 */
int __find_patch(int fd, uint8_t sha1[20], struct xenlp_patch_info **info)
{
    uint32_t *count = NULL, i = 0, ccode = SANDBOX_OK;
	struct  xenlp_patch_info *response;
	char *rbuf = NULL;
	
	
	/* return buffer format:*/
        /* uint32_t count;
         * struct struct xenlp_patch_info[count];
	 * buffer needs to be freed by caller 
	*/
	count = (uint32_t *)sandbox_list_patches(fd);
        DMSG("list path response buf %p\n", count);
        if (count == NULL) {
            DMSG("sandbox_list_patches returned a NULL address\n");
            return -1;
        }
        
        dump_sandbox(count, 32);
        
	if (*count == 0) {
		LMSG("currently there are no applied patches\n");
                ccode = 0;
		goto exit;
	}

        if (count && info) {
            if (sha1 == NULL) {
                /* "find" every patch - aka list */
                /* realloc behaves as malloc when ptr is null */
                *info = realloc(*info, sizeof(struct xenlp_patch_info) * (*count));
            } else {
                /* will return at most 1 struct xenlp_patch_info */
                *info = realloc(*info, sizeof(struct xenlp_patch_info));    
            }
        }

        if (info && *info == NULL) {
            ccode = -1;
            DMSG("unable to (re)allocate memory in _find_patch\n");
            goto exit;
        }
        
	LMSG("%d applied patches...\n", *count);
	rbuf = (char *)count;
	rbuf += sizeof(uint32_t);
	
	response = (struct xenlp_patch_info *)rbuf;
	dump_sandbox(response, 32);
	
	for (i = 0, ccode = 0; i < *count; i++) {
            if (sha1 == NULL) {
                char sha1str[41];
                DMSG("extracting sha1\n");
                dump_sandbox(response[i].sha1, 20);
		
                bin2hex(response[i].sha1, sizeof(response[i].sha1),
                        sha1str, sizeof(sha1str));
                LMSG("%s\n", sha1str);
                memcpy(&*info[i], &response[i], sizeof(**info));
                ccode = 1;
            } else if (memcmp(sha1, response[i].sha1, 20) == 0) {
                memcpy(*info, &response[i], sizeof(**info));
                ccode = 1;
                goto exit;
            }
            if (ccode == 0) {
                DMSG("no matching applied live patches\n");
                if (info && *info) {
                    free(*info);
                    *info = NULL;
                }
            }
	}
exit:
	free(count);
	return ccode;
}


/* return zero for success, -1 on failure */
/* TODO: allow for list (call with NULL sha1, return a list of all applied patches.
 */
int find_patch(xc_interface_t xch, unsigned char *sha1, size_t sha1_size,
               struct xenlp_patch_info **patch) 
{


    return SANDBOX_OK;
    
}
