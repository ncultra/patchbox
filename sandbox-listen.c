/*****************************************************************
 * Copyright 2015 Rackspace, Inc.
 *
 * listen on a unix domain socket for incoming patches
 ****************************************************************/
#include "sandbox.h"


/*************************************************************************/
/*                 Message format                                        */
/*-----------------------------------------------------------------------*/
/*       0                   1                   2                   3   */
/*       0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 */
/*      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+*/
/*      |     magic number:   0x53414e44  'SAND' in ascii                */
/*      +-+-+-+-f+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+*/
/*      | protocol version              |   message id                  |*/
/*      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+*/
/*      | overall message length                                        |*/
/*      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+*/
/*      |    4 bytes field 1 length                                     |*/
/*      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+ <------- hdr ends here */
/*      |    field  1                  ...                              |*/
/*      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+*/
/*      |    4 bytes field n length                                     |*/
/*      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+*/
/*      |    field  n                    ...                            |*/
/*      +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+*/

#define SANDBOX_MSG_HDRLEN 0x00000010
#define SANDBOX_MSG_MAGIC  {'S', 'A', 'N', 'D'}
#define SANDBOX_MSG_VERSION (uint16_t)0x0001				      
#define SANDBOX_MSG_GET_VER(b) (*(uint16_t *)((uint8_t *)b + 0x20))
#define SANDBOX_MSG_GET_ID(b) (*(uint16_t *)((uint8_t *)b + 0x30))
#define SANDBOX_MSG_MAX_LEN PLATFORM_PAGE_SIZE
#define SANDBOX_MSG_GET_LEN(b) (*(uint64_t *)((uint8_t *)b + 0x40))



#define SANDBOX_MSG_APPLY 1
#define SANDBOX_MSG_APPLYRSP 2
#define SANDBOX_MSG_LIST 3
#define SANDBOX_MSG_LISTRSP 4
#define SANDBOX_MSG_GET_BLD 5
#define SANDBOX_MSG_GET_BLDRSP 6

#define SANDBOX_OK 0
#define SANDBOX_ERR_BAD_HDR -2
#define SANDBOX_ERR_BAD_VER -3
#define SANDBOX_ERR_BAD_LEN -4
#define SANDBOX_ERR_BAD_MSGID -5
#define SANDBOX_ERR_NOMEM -6
#define SANDBOX_ERR_RW -7


/* Message ID 1: apply patch ********************************************/
/* Fields:
   1) header
   2) sha1 build id of the target - must match (20 bytes)
   3) patch name (string)
   4) patch size
   5) patch buf
   6) canary (32 bytes of binary instructions), used to
      verify the jump address.
   7) jump location (uintptr_t  absolute address for jump)
   7) sha1 of the patch bytes (20 bytes)
   8) count of extended fields (4 bytes, always zero for this version).

   reply msg: ID 2
   1) header
   2) uint64_t  0L "OK," or error code
 */

/* Message ID 3: list patch ********************************************/
/* Fields:
   1) header
   2) patch name (string, wild cards ok)
   3) sha1 of the patch (corresponding to field 5 of message ID 1),
      20-byte buffer

   reply msg ID 4:
   1) header
   2) uint64_t 0L "OK, or error code.
   3) patch name (if found)
   4) sha1 of the patch
*/

/* Message ID 5: get build info ********************************************/

/* Fields:
   1) header (msg id 3)

   reply msg ID 6:
   1) header
   2) uint64_t 0L "OK, or error code.
   3) 20-bytes sha1 git HEAD of the running binary
   4) $CC at build time (string)
   5) $CFLAGS at build time (string)
*/

static inline ssize_t check_magic(uint8_t *magic)
{
	uint8_t m[] = SANDBOX_MSG_MAGIC;
	
	return memcmp(magic, m, sizeof(m));
}

ssize_t dispatch_apply(int, void **);
ssize_t dispatch_list(int, void **);
ssize_t dispatch_getbld(int, void **);
ssize_t dummy(int, void **);
ssize_t send_response(int fd, uint64_t id, ssize_t errcode);
ssize_t marshal_patch_data(int sock, void **bufp);

typedef ssize_t (*handler)(int, void **);

handler dispatch[] =
{
	dispatch_apply,
	dummy,
	dispatch_list,
	dummy,
	dispatch_getbld,
	dummy,
	NULL
};

#define QLEN 5 // depth of the listening queue
#define STALE 30 // timout for client user id data



// TODO: handle signals in the thread

void *listen_thread(void *arg)
{
	char *socket_name = (char *)arg;
	uint64_t quit = 0;
	int listen_fd, client_fd;
	uid_t client_id;
	do {
		listen_fd = listen_sandbox_sock(socket_name);
		if (listen_fd > 0) {	
			client_fd = accept_sandbox_sock(listen_fd, &client_id);
			if (client_fd > 0) {
				uint16_t version, id;
				uint64_t len;
				quit   = read_sandbox_message_header(client_fd, &version, &id, &len);
			}
		}
	} while (!quit && listen_fd > 0);
	return NULL;
}


// WRS
// create and listen on a unix domain socket.
// connect, peek at the incoming data. 
// sock_name: full path of the socket e.g. /var/run/sandbox 
ssize_t listen_sandbox_sock(const char *sock_name)
{
	ssize_t fd, len, err, ccode;
	struct sockaddr_un un;

	if (strlen(sock_name) >= sizeof(un.sun_path)) {
		errno = ENAMETOOLONG;
		return(-1);
	}

	if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
		return(-2);
	}

	unlink(sock_name);

	memset( &un, 0, sizeof(un));
	un.sun_family = AF_UNIX;
	strcpy(un.sun_path, sock_name);  // already checked the length
	len = offsetof(struct sockaddr_un, sun_path) + strlen(sock_name);

	if (bind(fd, (struct sockaddr *)&un, len) < 0) {
		ccode = -3;
		goto errout;
	}

	if (listen(fd, QLEN) < 0) {
		ccode = -4;
		goto errout;
	}

	return fd;
errout:
	err = errno;
	close(fd);
	errno = err;
	return(ccode);
}


ssize_t accept_sandbox_sock(int listenfd, uid_t *uidptr)
{
	int clifd, err, ccode;
	socklen_t len;
	time_t staletime;
	struct sockaddr_un un;
	struct stat statbuf;
	char *name;

	if ((name = malloc(sizeof(un.sun_path + 1))) == NULL) {
		return (-1);
	}
	
	len = sizeof(un);
	do { clifd = accept(listenfd, (struct sockaddr *)&un, &len);
	} while (errno == EINTR || errno == EAGAIN);

	if (clifd < 0) {
		free(name);
		return (-2);
	}
	len -= offsetof(struct sockaddr_un, sun_path);
	memcpy(name, un.sun_path, len);
	name[len] = 0;
	if (stat(name, &statbuf) < 0) {
		ccode = -3; // couldn't stat the clients uid
		goto errout;
	}

	#ifdef S_ISSOCK
	if (S_ISSOCK(statbuf.st_mode) == 0) {
		ccode = -4;
		goto errout;
	}
	#endif

	// exit if the socket mode is too permissive or wrong
	if ((statbuf.st_mode & (S_IRWXG | S_IRWXO)) ||
	    (statbuf.st_mode & S_IRWXU) != S_IRWXU) {
		ccode = -5;
		goto errout;
	}

	// check the age of the socket access bits - it has to be active now
	staletime = time(NULL) - STALE;
	if (statbuf.st_atime < staletime ||
	    statbuf.st_ctime < staletime ||
	    statbuf.st_mtime < staletime) {
		ccode = -6;  // too old, not a currently active uid
		goto errout;
	}
	
	if (uidptr != NULL) {
		*uidptr = statbuf.st_uid;
	}

	unlink(name);
	free(name);
	return(clifd);

errout:
	err = errno;
	close(clifd);
	free(name);
	errno = err;
	return ccode;
}

// WRS, with check for Linux EAGAIN
ssize_t	readn(int fd, void *vptr, size_t n)
{
	size_t  nleft;
	ssize_t nread;
	char   *ptr;
	ptr = vptr;
	nleft = n;
	while (nleft > 0) {
		if ( (nread = read(fd, ptr, nleft)) < 0) {
			if (errno == EINTR || errno == EAGAIN)
				nread = 0;      /* and call read() again */
			else
				return (-1);
		} else if (nread == 0)
			break;              /* EOF */
		
		nleft -= nread;
		ptr += nread;
	}
	return (n - nleft);         /* return >= 0 */
}



// WRS, with check for Linux EAGAIN
ssize_t writen(int fd, const void *vptr, size_t n)
{
	size_t nleft;
	ssize_t nwritten;
	const char *ptr;
	
	ptr = vptr;
	nleft = n;
	while (nleft > 0) {
		if ( (nwritten = write(fd, ptr, nleft)) <= 0) {
			if (nwritten < 0 && (errno == EINTR || errno == EAGAIN))
				nwritten = 0;   /* and call write() again */
			else
				return (-1);    /* error */
		}
		
		nleft -= nwritten;
		ptr += nwritten;
	}
	return (n);
}

/* header is  3 * 32 bytes -
 * 'SAND'
 * version
 * id
 * overall message length
 */
/* if this function returns ERR, ptr parameters are undefined */
/* if it returns 0, ptr parameters will have correct values */
ssize_t read_sandbox_message_header(int fd, uint16_t *version,
				    uint16_t *id, uint64_t *len)
{
	uint8_t hbuf[0x60];
	ssize_t ccode = 0;
	void *dispatch_buffer = NULL;
	
	if ((ccode = readn(fd, &hbuf, sizeof(hbuf))) != sizeof(hbuf)) {
		goto errout;
	}
	if (check_magic(hbuf)) {
		ccode = SANDBOX_ERR_BAD_HDR;
		goto errout;
	}
	if (SANDBOX_MSG_VERSION != (*version = SANDBOX_MSG_GET_VER(hbuf))) {
		ccode = SANDBOX_ERR_BAD_VER;
		goto errout;
	}

	*id = SANDBOX_MSG_GET_VER(hbuf);
	
	if (*id < SANDBOX_MSG_APPLY || *id > SANDBOX_MSG_GET_BLDRSP) {
		ccode = SANDBOX_ERR_BAD_MSGID;
		goto errout;
	}

 	if (SANDBOX_MSG_MAX_LEN > (*len = SANDBOX_MSG_GET_LEN(hbuf))) {
		ccode = SANDBOX_ERR_BAD_LEN;
		goto errout;
	}
	ccode = dispatch[*id](fd, &dispatch_buffer);
	
errout:	
	return send_response(fd, SANDBOX_ERR_BAD_HDR,  ccode);
	
}




ssize_t marshal_patch_data(int sock, void **bufp)
{
	assert(bufp && *bufp == NULL);

	
	uint8_t bldid[0x14];
	char name[0x81];
	uint64_t len, patchlen;
	ssize_t ccode;
	struct patch *new_patch = NULL;
	

	/* target build id * - size must be 20 bytes */
	//TODO - check the build id!
	if (readn(sock, &len, sizeof(len)) == sizeof(len) && len == 0x14) {
		ccode = readn(sock, bldid, len);
		if (ccode != len) {
			return(SANDBOX_ERR_BAD_LEN);
		}
	} else {
		return(SANDBOX_ERR_RW);
	}
	
	//TODO: macro-ize this repitive code

	if (readn(sock, &len, sizeof(len)) == sizeof(len)  && len < 0x40 && len < 0 ) {
		if (readn(sock, name, len) != len) {
			return(SANDBOX_ERR_RW);
		}
	}else {
		return(SANDBOX_ERR_BAD_LEN);	
	}
	
	/* patch data */


	if (readn(sock, &patchlen, sizeof(patchlen)) == sizeof(patchlen) &&
	    patchlen > 0 && patchlen < MAX_PATCH_SIZE) {
		new_patch = alloc_patch(name, patchlen);
		if (new_patch == NULL) {
			ccode = SANDBOX_ERR_NOMEM;
			goto errout;
		}
		assert(new_patch->patch_buf);
		if (readn(sock, (uint8_t *)new_patch->patch_buf, new_patch->patch_size) !=
		    new_patch->patch_size) {
			ccode = SANDBOX_ERR_RW;
			goto errout;
		}
	} else {
		ccode = SANDBOX_ERR_BAD_LEN;
		goto errout;
	}

	/* patch canary, 64 bytes */

	if (readn(sock, &len, sizeof(len)) == sizeof(len) && len == 0x20) {
		if (readn(sock, new_patch->canary, 0x20) != 0x20) {
			return(SANDBOX_ERR_RW);
		}	
	}  else {
		return(SANDBOX_ERR_BAD_LEN);
	}

	/* jump location */
	/* the socket writer will provide the relative jump location. we need to make */
	/*   it absolute by adding the start of the .text segment to the address. */
	/* Then we check it using the canary. The canary should be identical to */
        /*  64 bytes starting at the jmp location (inclusively) */

	if (readn(sock, &len, sizeof(len)) == sizeof(len) && len == sizeof(uintptr_t)) {
		if (readn(sock, &new_patch->reloc_dest, sizeof(uintptr_t)) != sizeof(uintptr_t)) {
			return(SANDBOX_ERR_RW);
		}	
	}  else {
		return(SANDBOX_ERR_BAD_LEN);
	}
	
	/* patch sha1 signature */
	if (readn(sock, &len, sizeof(len)) == 0x14) {
		// TODO: actually check the signature !
		if (readn(sock, new_patch->SHA1, 0x14) != 0X14) {
			ccode = SANDBOX_ERR_RW;
			goto errout;
		}
	} else {
		ccode = SANDBOX_ERR_BAD_LEN;
		goto errout;
	}
	
	new_patch->flags |= PATCH_WRITE_ONCE;
	memcpy(new_patch->build_id, bldid, 0x14);
	new_patch->reloc_size = PLATFORM_RELOC_SIZE;
	*bufp = new_patch;
	return(0L);
	
errout:
	if (new_patch != NULL) {
		free_patch(new_patch);
	}
	if (bufp && *bufp != NULL) {
		free(*bufp);
	}
	
	return send_response(sock, SANDBOX_ERR_BAD_HDR, ccode);
	
}


ssize_t send_response(int fd, uint64_t id, ssize_t errcode)
{
	ssize_t ccode;
	uint8_t sand[] = SANDBOX_MSG_MAGIC;
	uint16_t pver = SANDBOX_MSG_VERSION;
	uint64_t msgid = id, len = SANDBOX_MSG_HDRLEN + 4;

	/* magic header */
	ccode = writen(fd, sand, sizeof(sand));
	if (ccode != sizeof(sand)) {
		goto errout;
	}

	/* protocol version */
	ccode = writen(fd, &pver, sizeof(pver));
	if (ccode != sizeof(pver)) {
		goto errout;
	}
	/* message id - apply response */
	ccode = writen(fd, &msgid, sizeof(msgid));
	if (ccode != sizeof(msgid)) {
		goto errout;
	}

	/* msg length */
	ccode = writen(fd, &len, sizeof(len));
	if (ccode != sizeof(len)) {
		goto errout;
	}
	
	/* this message return code */
	ccode = errcode;
	if ((writen(fd, &ccode, sizeof(ccode))) != sizeof(ccode)){
		goto errout;
	}
	
	return(SANDBOX_OK);
	
errout:
	return(SANDBOX_ERR_RW);
}



static inline ssize_t send_apply_response(int fd, ssize_t errcode)
{
	return send_response(fd, SANDBOX_MSG_APPLYRSP, errcode);
}


/*****************************************************************
 * Dispatch functions: at this point socket's file pointer 
 * is at the first field
 *
 *****************************************************************/
ssize_t dispatch_apply(int fd, void ** bufp)
{
	ssize_t ccode = marshal_patch_data(fd, bufp);
	if (ccode == SANDBOX_OK) {
		
		struct patch *p = (struct patch *)*bufp;
		ccode = apply_patch(p);
	}
	
	send_apply_response(fd, ccode);
	return(ccode);
}

ssize_t dispatch_list(int fd, void **bufp)
{
	return send_response(fd, SANDBOX_ERR_BAD_MSGID, SANDBOX_ERR_BAD_MSGID);
} 

ssize_t dispatch_getbld(int fd, void **bufp)
{
	return send_response(fd, SANDBOX_ERR_BAD_MSGID, SANDBOX_ERR_BAD_MSGID);
}

ssize_t dummy(int fd, void **bufp)
{

	return send_response(fd, SANDBOX_ERR_BAD_MSGID, SANDBOX_ERR_BAD_MSGID);
	
}

