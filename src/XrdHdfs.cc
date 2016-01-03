/******************************************************************************/
/*                                                                            */
/*                       X r d H d f s . c c                                  */
/*                                                                            */
/* (c) 2004 by the Board of Trustees of the Leland Stanford, Jr., University  */
/*       All Rights Reserved. See XrdInfo.cc for complete License Terms       */
/*   Produced by Andrew Hanushevsky for Stanford University under contract    */
/*               DE-AC03-76-SFO0515 with the Deprtment of Energy              */
/******************************************************************************/

const char *XrdHdfsSVNID = "$Id$";

#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <memory.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/param.h>
#include <sys/stat.h>

#include "XrdVersion.hh"
#include "XrdSys/XrdSysError.hh"
#include "XrdSys/XrdSysHeaders.hh"
#include "XrdSys/XrdSysLogger.hh"
#include "XrdOuc/XrdOucTrace.hh"
#include "XrdOuc/XrdOucEnv.hh"
#include "XrdSys/XrdSysPlugin.hh"
#include "XrdSys/XrdSysPthread.hh"
#include "XrdSec/XrdSecInterface.hh"
#include "XrdHdfs.hh"

#ifdef AIX
#include <sys/mode.h>
#endif

namespace
{
#if HADOOP_VERSION < 20
   const char *hdfs_19_groups[] = { "nobody" };
#endif
   hdfsFS hadoop_connect(const char* a, int b, const char* c)
   {
#if HADOOP_VERSION < 20
      return hdfsConnectAsUser(a, b, c, hdfs_19_groups, 1);
#else
      return hdfsConnectAsUserNewInstance(a, b, c);
#endif
   }

const char* ExtractAuthName(const XrdOucEnv& client) {

    const XrdSecEntity *sec = const_cast<XrdOucEnv&>(client).secEnv();
    if (sec && sec->name)
    {
        return sec->name;
    }
    else
    {
        return "nobody";
    }
}

}

/******************************************************************************/
/*       O S   D i r e c t o r y   H a n d l i n g   I n t e r f a c e        */
/******************************************************************************/

#ifndef S_IAMB
#define S_IAMB  0x1FF
#endif

/******************************************************************************/
/*                  E r r o r   R o u t i n g   O b j e c t                   */
/******************************************************************************/

namespace XrdHdfs
{

XrdSysError HdfsEroute(0, "hdfs_");

XrdOucTrace OssTrace(&HdfsEroute);

static XrdHdfsSys XrdHdfsSS;

}

using namespace std;
using namespace XrdHdfs;

/******************************************************************************/
/*                         G e t F i l e S y s t e m                          */
/******************************************************************************/
extern "C"
{
XrdOss *XrdOssGetStorageSystem(XrdOss       *native_oss,
                               XrdSysLogger *Logger,
                         const char         *config_fn,
                         const char         *parms)
{
   return (XrdHdfsSS.Init(Logger, config_fn) ? 0 : (XrdOss *)&XrdHdfsSS);
}
}

/******************************************************************************/
/*           D i r e c t o r y   O b j e c t   I n t e r f a c e s            */
/******************************************************************************/
/******************************************************************************/
/*                                  O p e n d i r                             */
/******************************************************************************/
XrdHdfsDirectory::XrdHdfsDirectory(const char *tid) : XrdOssDF()
{
   fs = NULL;
   dh = (hdfsFileInfo*)NULL;
   numEntries = 0;
   dirPos = 0;
   isopen = 0;
   fname = 0;
}

int XrdHdfsDirectory::Opendir(const char *dir_path, XrdOucEnv & client)
/*
  Function: Open the directory `path' and prepare for reading.

  Input:    path      - The fully qualified name of the directory to open.

  Output:   Returns XrdOssOK upon success, otherwise (-errno).
*/
{
   int retc;

// Return an error if we have already opened
   if (isopen) return -EINVAL;

// Get the security name, and connect with it
   const char* sec_name = ExtractAuthName(client);
   fs = hadoop_connect("default", 0, sec_name);


// Set up values for this directory object
//
   if (XrdHdfsSS.the_N2N) {
       char actual_path[XrdHdfsMAX_PATH_LEN+1];
       if ((retc = (XrdHdfsSS.the_N2N)->lfn2pfn(dir_path, actual_path, sizeof(actual_path)))) {
          (XrdHdfsSS.eDest)->Say("Cannot find a N2N mapping for ", dir_path, "; using path directly.");
          fname = strdup(dir_path);
       }
          else fname = strdup(actual_path);
   } else {
       fname = strdup(dir_path);
   }
   dirPos = 0;

// Open the directory and get it's id
//
   if (!(dh = hdfsListDirectory(fs, fname, &numEntries))) {
      isopen = 0;
      return (errno <= 0) ? -1 : -errno;
   }
   isopen = 1;

// All done
//
   return XrdOssOK;
}

/******************************************************************************/
/*                             R e a d d i r                                  */
/******************************************************************************/

int XrdHdfsDirectory::Readdir(char * buff, int blen)
/*
  Function: Read the next directory entry.

  Input:    None.

  Output:   Upon success, returns the contents of the next directory entry as
            a null terminated string. Returns a null pointer upon EOF or an
            error. To differentiate the two cases, getErrorInfo will return
            0 upon EOF and an actual error code (i.e., not 0) on error.
*/
{
#ifndef NODEBUG
    static const char *epname = "Readdir";
#endif

  if (!isopen) return -EBADF;

// Lock the direcrtory and do any required tracing
//
  if (!dh)  {
     XrdHdfsSys::Emsg(epname,error,EBADF,"read directory",fname);
     return -EBADF;
  }

// Check if we are at EOF (once there we stay there)
//
   if (dirPos == numEntries) {
     *buff = '\0';
     return 0;
   }

// Read the next directory entry
//
   hdfsFileInfo fileInfo = dh[dirPos];
   dirPos++;

// Return the actual entry
//
   std::string full_name = fileInfo.mName;
   full_name.erase(0, full_name.rfind("/"));
   strlcpy(buff, full_name.c_str(), blen);
   return XrdOssOK;
}

/******************************************************************************/
/*                                 C l o s e                                  */
/******************************************************************************/
  
int XrdHdfsDirectory::Close(long long *retsz)
/*
  Function: Close the directory object.

  Input:    cred       - Authentication credentials, if any.

  Output:   Returns XrdOssOK upon success and -EBADF upon failure.
*/
{

   if (!isopen) return -EBADF;

// Release the handle
//
   if (dh != NULL && numEntries >= 0) {
      hdfsFreeFileInfo(dh, numEntries);
   }

// Do some clean-up
//
   if (fname) {
     free(fname);
     fname = NULL;
   }
   dh = (hdfsFileInfo *)0; 
   numEntries = 0;
   dirPos = 0;
   isopen = 0;
   return 0;
}

XrdHdfsDirectory::~XrdHdfsDirectory()
{
  if (dh != NULL && numEntries >= 0) {
    hdfsFreeFileInfo(dh, numEntries);
  }
#if HADOOP_VERSION >= 20
  if (fs != NULL) {
    hdfsDisconnect(fs);
  }
#endif
  if (fname) {
    free(fname);
  }
}

/******************************************************************************/
/*                F i l e   O b j e c t   I n t e r f a c e s                 */
/******************************************************************************/


/******************************************************************************/
/*                          C o n s t r u c t o r                             */
/******************************************************************************/
XrdHdfsFile::XrdHdfsFile(const char *user) : XrdOssDF(), m_fs(NULL), fh(NULL), fname(NULL), m_nextoff(0),
    readbuf(NULL), readbuf_size(0), readbuf_offset(0), readbuf_len(0),
    readbuf_bypassed(0), readbuf_misses(0), readbuf_hits(0), readbuf_partial_hits(0),
    readbuf_bytes_used(0), readbuf_bytes_loaded(0)
{
}


/******************************************************************************/
/*                              C o n n e c t                                 */
/******************************************************************************/
bool XrdHdfsFile::Connect(const XrdOucEnv &client)
{
    if (m_fs)
    {
        hdfsDisconnect(m_fs);
        m_fs = NULL;
    }
    const XrdSecEntity *sec = const_cast<XrdOucEnv&>(client).secEnv();
    if (sec && sec->name)
    {
        m_fs = hdfsConnectAsUserNewInstance("default", 0, sec->name);
    }
    else
    {
        m_fs = hdfsConnectAsUserNewInstance("default", 0, "nobody");
    }
    return m_fs;
}

/******************************************************************************/
/*                                  O p e n                                   */
/******************************************************************************/

int XrdHdfsFile::Open(const char               *path,      // In
                            int                 openMode,  // In
                            mode_t              createMode,// In
                            XrdOucEnv          &client)    // In
/*
  Function: Open the file `path' in the mode indicated by `openMode'.  

  Input:    path      - The fully qualified name of the file to open.
            openMode  - One of the following flag values:
                        SFS_O_RDONLY - Open file for reading.
                        SFS_O_WRONLY - Open file for writing.
                        SFS_O_RDWR   - Open file for update
                        SFS_O_CREAT  - Create the file open in RDWR mode
                        SFS_O_TRUNC  - Trunc  the file open in RDWR mode
            Mode      - The Posix access mode bits to be assigned to the file.
                        These bits correspond to the standard Unix permission
                        bits (e.g., 744 == "rwxr--r--"). Mode may also conatin
                        SFS_O_MKPTH is the full path is to be created. The
                        agument is ignored unless openMode = O_CREAT.
            client    - Authentication credentials, if any.
            info      - Opaque information to be used as seen fit.

  Output:   Returns OOSS_OK upon success, otherwise -errno is returned.
*/
{
#ifndef NODEBUG
   static const char *epname = "open";
#endif
   //const int AMode = S_IRWXU|S_IRWXG|S_IROTH|S_IXOTH; // 775
   char *opname;
   int open_flag = 0;

// Verify that this object is not already associated with an open file
//
   if (fh != NULL)
      return -EINVAL;

   int retc;
   if (XrdHdfsSS.the_N2N) {
       char actual_path[XrdHdfsMAX_PATH_LEN+1];
       if ((retc = (XrdHdfsSS.the_N2N)->lfn2pfn(path, actual_path, sizeof(actual_path)))) {
          (XrdHdfsSS.eDest)->Say("Cannot find a N2N mapping for ", path, "; using path directly.");
          fname = strdup(path);
       }
       else fname = strdup(actual_path);
   } else {
       fname = strdup(path);
   }

   (XrdHdfsSS.eDest)->Say("File we will access: ", fname);

// Allocate readbuf
//
   XrdSysMutexHelper readbuf_lock(readbuf_mutex);

   if( !readbuf ) {
       readbuf_size = 32768;
       readbuf = (char *)malloc(readbuf_size);
       if( !readbuf ) {
           readbuf_size = 0;
           (XrdHdfsSS.eDest)->Say("Insufficient memory to allocate read-ahead buffer for ", path);
       }
   }

// Invalidate contents of readbuf, if any
//
   readbuf_offset = 0;
   readbuf_len = 0;

   readbuf_bypassed = 0;
   readbuf_misses = 0;
   readbuf_hits = 0;
   readbuf_partial_hits = 0;
   readbuf_bytes_used = 0;
   readbuf_bytes_loaded = 0;

   readbuf_lock.UnLock();

// Set the actual open mode
//
   switch(openMode & (O_RDONLY | O_WRONLY | O_RDWR))
   {
   case O_RDONLY: open_flag = O_RDONLY; break;
   case O_WRONLY: open_flag = O_WRONLY; break;
   case O_RDWR:   open_flag = O_WRONLY;   break;
   default:           open_flag = O_RDONLY; break;
   }

   // HDFS does not support read-write mode.
/*
   if (openMode & O_RDWR) {
       return XrdHdfsSys::Emsg(epname,error,ENOTSUP, "Read-write mode not"
           " supported by HDFS.",path);
   }
*/
// Prepare to create or open the file, as needed
//
   if (openMode & O_CREAT) {
       opname = (char *)"create";
   } else if (openMode & O_TRUNC) {
         open_flag  = O_TRUNC;
                 opname = (char *)"truncate";
      } else opname = (char *)"open";

// Setup a new filesystem instance.
   if (!Connect(client))
   {
       return XrdHdfsSys::Emsg(epname, error, EIO, "Failed to connect to HDFS");
   }

// Open the file and make sure it is a file
//

   int err_code = 0;

   if ((fh = hdfsOpenFile(m_fs, fname, open_flag, 0, 0, 0)) == NULL) {
       err_code = errno;
       hdfsFileInfo * fileInfo = hdfsGetPathInfo(m_fs, fname);
       if (fileInfo != NULL) {
           if (fileInfo->mKind == kObjectKindDirectory) {
                   err_code = EISDIR;
           } else {
               err_code = EEXIST;
           }
           hdfsFreeFileInfo(fileInfo, 1);
       } else { 
           err_code = ENOENT;
       }
   }

// All done.
//
   if (err_code != 0)
       return (err_code > 0) ? -err_code : err_code;

   return XrdOssOK;
}

/******************************************************************************/
/*                                 C l o s e                                  */
/******************************************************************************/

int XrdHdfsFile::Close(long long int *)
/*
  Function: Close the file object.

  Input:    None

  Output:   Returns XrdOssOK upon success and -errno upon failure.
*/
{
   static const char *epname = "close";

// Release the handle and return
//
   int ret = XrdOssOK;
   if (fh != NULL  && hdfsCloseFile(m_fs, fh) != 0) {
      ret = XrdHdfsSys::Emsg(epname, error, errno, "close", fname);
   }
   fh = NULL;
#if HADOOP_VERSION >= 20
   if (m_fs != NULL && hdfsDisconnect(m_fs) != 0) {
      ret = XrdHdfsSys::Emsg(epname, error, errno, "close", fname); 
   }
#endif
   m_fs = NULL;

   XrdSysMutexHelper readbuf_lock(readbuf_mutex);

   if (readbuf) {
       char stats[300];
       float pct_buf_used = 0;
       if( readbuf_bytes_loaded > 0 ) {
           pct_buf_used = 100.0*readbuf_bytes_used/readbuf_bytes_loaded;
       }
       snprintf(stats,sizeof(stats),"%u misses, %u hits, %u partial hits, %u unbuffered, %lu buffered bytes used of %lu read (%.2f%%)",
                readbuf_misses,readbuf_hits,readbuf_partial_hits,readbuf_bypassed,
                readbuf_bytes_used,readbuf_bytes_loaded,
                pct_buf_used);
       (XrdHdfsSS.eDest)->Say("Readahead buffer stats for ",fname," : ",stats);

      free(readbuf);
      readbuf = 0;
      readbuf_size = 0;
      readbuf_offset = 0;
      readbuf_len = 0;
   }
   readbuf_lock.UnLock();

   if (fname) {
      free(fname);
      fname = 0;
   }
   return ret;
}

XrdHdfsFile::~XrdHdfsFile()
{
   if (m_fs && fh) {hdfsCloseFile(m_fs, fh);}
#if HADOOP_VERSION >= 20
   if (m_fs) {hdfsDisconnect(m_fs);}
#endif
   if (fname) {free(fname);}
   if (readbuf) {free(readbuf);}
}
  
/******************************************************************************/
/*                                  R e a d                                   */
/******************************************************************************/

ssize_t XrdHdfsFile::Read(void   *buff,      // Out
                          off_t   offset,    // In
                          size_t  blen)      // In
/*
  Function: Read `blen' bytes at `offset' into 'buff' and return the actual
            number of bytes read.

  Input:    offset    - The absolute byte offset at which to start the read.
            buff      - Address of the buffer in which to place the data.
            blen      - The size of the buffer. This is the maximum number
                        of bytes that will be read from 'fd'.

  Output:   Returns the number of bytes read upon success and -errno o/w.
*/
{
#ifndef NODEBUG
   static const char *epname = "Read";
#endif
   size_t nbytes;

   XrdSysMutexHelper readbuf_lock(readbuf_mutex);
   // There are multiple exit points from this function,
   // so we rely on the fact that readbuf_lock will unlock
   // when it goes out of scope.

   if( blen > readbuf_size ) {
       // request is larger than readbuf, so bypass readbuf and read
       // directly into caller's buffer
      nbytes = hdfsPread(m_fs, fh, (off_t)offset, (void *)buff, (size_t)blen);

      readbuf_bypassed++;
   }
   else if( (offset >= readbuf_offset) && (offset + blen <= readbuf_offset + readbuf_len) ) {
       // satisfy request from read buffer
       off_t offset_in_readbuf = offset - readbuf_offset;
       nbytes = blen;
       memcpy(buff,readbuf + offset_in_readbuf,nbytes);

       readbuf_hits++;
       readbuf_bytes_used += nbytes;
   }
   else {
       // satisfy as much of request from the read buffer as possible
       if( (offset >= readbuf_offset) && (offset < readbuf_offset + static_cast<off_t>(readbuf_len)) ) {
           off_t offset_in_readbuf = offset - readbuf_offset;
           nbytes = readbuf_len - offset_in_readbuf;
           memcpy(buff,readbuf + offset_in_readbuf,nbytes);

           // shift request past end of readbuf
           blen -= nbytes;
           offset += nbytes;
           buff = ((char *)buff) + nbytes;

           readbuf_partial_hits++;
           readbuf_bytes_used += nbytes;
       }
       else {
           nbytes = 0;
           readbuf_misses++;
       }

       // read into readbuf
       readbuf_offset = offset;
       readbuf_len = 0;
       // loop in case of short reads
       while( readbuf_len < readbuf_size ) {
           int n = hdfsPread(m_fs, fh, offset + readbuf_len, (void *)(readbuf + readbuf_len), readbuf_size - readbuf_len);
           if( n < 0 && errno == EINTR ) {
               continue;
           }
           else if( n < 0 ) {
               return XrdHdfsSys::Emsg(epname, error, errno, "read", fname);
           }
           else if( n == 0 ) {
               break;
           }
           readbuf_len += n;
       }

	   size_t bytes_to_copy;
       if( readbuf_len < blen ) {
           bytes_to_copy = readbuf_len;
       }
       else {
           bytes_to_copy = blen;
       }
       memcpy(buff,readbuf,bytes_to_copy);

       readbuf_bytes_loaded += readbuf_len - bytes_to_copy; // extra bytes read
	   nbytes += bytes_to_copy;
   }

   if (nbytes  < 0)
      return XrdHdfsSys::Emsg(epname, error, errno, "read", fname);

// Return number of bytes read
//
   return nbytes;
}
  
/******************************************************************************/
/*                              R e a d   A I O                               */
/******************************************************************************/
  
int XrdHdfsFile::Read(XrdSfsAio *aiop)
{

// Execute this request in a synchronous fashion
//
   aiop->Result = this->Read((void *)aiop->sfsAio.aio_buf, aiop->sfsAio.aio_offset,
                             aiop->sfsAio.aio_nbytes);
   aiop->doneRead();
   return 0;
}

/******************************************************************************/
/*                                 W r i t e                                  */
/******************************************************************************/

ssize_t XrdHdfsFile::Write(const void *buff,    // In
                                 off_t offset,  // In
                                 size_t blen)   // In
/*
  Function: Write `blen' bytes at `offset' from 'buff' and return the actual
            number of bytes written.

  Input:    offset    - The absolute byte offset at which to start the write.
            buff      - Address of the buffer from which to get the data.
            blen      - The size of the buffer. This is the maximum number
                        of bytes that will be written to 'fd'.

  Output:   Returns the number of bytes written upon success and -errno o/w.

  Notes:    An error return may be delayed until the next write(), close(), or
            sync() call.

*/
{
   static const char *epname = "write";

// Make sure the offset is not too large
//
#if _FILE_OFFSET_BITS!=64
   if (offset >  0x000000007fffffff)
      return XrdHdfsSys::Emsg(epname, error, EFBIG, "write", fname);
#endif

    if (offset != m_nextoff)
    {
        return XrdHdfsSys::Emsg(epname, error, ENOTSUP, "Out-of-order writes not"
            " supported by HDFS.", fname);
    }

    ssize_t result = hdfsWrite(m_fs, fh, buff, blen);
    if (result >= 0) {m_nextoff += result;}

   return result;
}

/******************************************************************************/
/*                             W r i t e   A I O                              */
/******************************************************************************/
  
int XrdHdfsFile::Write(XrdSfsAio *aiop)
{

// Execute this request in a synchronous fashion
//
   aiop->Result = this->Write((const char *)aiop->sfsAio.aio_buf, aiop->sfsAio.aio_offset,
                              aiop->sfsAio.aio_nbytes);
   aiop->doneWrite();
   return 0;
}
  
/******************************************************************************/
/*                                F s t a t                                   */
/******************************************************************************/

int XrdHdfsFile::Fstat(struct stat     *buf)         // Out
/*
  Function: Return file status information

  Input:    buf         - The stat structiure to hold the results

  Output:   Returns XrdOssOK upon success and -errno upon failure.
*/
{
   static const char *epname = "stat";

   hdfsFileInfo * fileInfo = hdfsGetPathInfo(m_fs, fname);
// Execute the function
//
   if (fileInfo == NULL)
      return XrdHdfsSys::Emsg(epname, error, errno, "stat", fname);
   buf->st_mode = fileInfo->mPermissions;
   buf->st_mode |= (fileInfo->mKind == kObjectKindDirectory) ? S_IFDIR : S_IFREG;
   buf->st_nlink = (fileInfo->mKind == kObjectKindDirectory) ? 0 : 1;
   buf->st_uid = 1;
   buf->st_gid = 1;
   buf->st_size = (fileInfo->mKind == kObjectKindDirectory) ? 4096 : \
      fileInfo->mSize;
   buf->st_mtime    = fileInfo->mLastMod;
   buf->st_atime    = fileInfo->mLastMod;
   buf->st_ctime    = fileInfo->mLastMod;
   buf->st_dev      = 0;
   buf->st_ino      = 0;

   hdfsFreeFileInfo(fileInfo, 1);

// All went well
//
   return XrdOssOK;
}

/******************************************************************************/
/*         F i l e   S y s t e m   O b j e c t   I n t e r f a c e s          */
/******************************************************************************/
 
/******************************************************************************/
/*                                  I n i t                                   */
/******************************************************************************/

/*
  Function: Initialize HDFS system.

  Input:    None

  Output:   Returns zero upon success otherwise (-errno).
*/
int XrdHdfsSys::Init(XrdSysLogger *lp, const char *configfn)
{
   int NoGo;
   const char *tmp;

// Do the herald thing
//
   eDest = &HdfsEroute;
   eDest->logger(lp);
   eDest->Say("Copr. 2009, Brian Bockelman, Hdfs Version ");
   eDest->Emsg("Config", "Copr. 2009, Brian Bockelman, Hdfs Version ");

// Initialize the subsystems
//
   N2N_Lib=NULL;
   the_N2N=NULL;
   tmp = ((NoGo=Configure(configfn)) ? "failed." : "completed.");
   eDest->Say("------ HDFS storage system initialization ", tmp);
   eDest->Emsg("HDFS storage system initialization.", tmp);

// All done.
//
   return NoGo;
}
 
/******************************************************************************/
/*                            g e t V e r s i o n                             */
/******************************************************************************/

const char *XrdHdfsSys::getVersion() {return XrdVERSION;}

/******************************************************************************/
/*                                  S t a t                                   */
/******************************************************************************/

int XrdHdfsSys::Stat(const  char    *path,    // In
                     struct stat    *buf,     // Out
                     int,                     // In
                     XrdOucEnv* client)
/*
  Function: Get info on 'path'.

  Input:    path        - Is the fully qualified name of the file to be tested.
            buf         - The stat structiure to hold the results
            error       - Error information object holding the details.
            client      - Authentication credentials, if any.
            info        - Opaque information, if any.

  Output:   Returns XrdOssOK upon success and -errno upon failure.
*/
{
   static const char *epname = "stat";
   int retc = XrdOssOK;
   char * fname;
   hdfsFileInfo * fileInfo = NULL;

   if (XrdHdfsSS.the_N2N) {
       char actual_path[XrdHdfsMAX_PATH_LEN+1];
       if ((XrdHdfsSS.the_N2N)->lfn2pfn(path, actual_path, sizeof(actual_path))) {
          (XrdHdfsSS.eDest)->Say("Cannot find a N2N mapping for ", path, "; using path directly.");
          fname = strdup(path);
       } else {
          fname = strdup(actual_path);
       }
   } else {
       fname = strdup(path);
   }

// Get the security name, and connect with it
//   When the cmsd uses this class, client is NULL.  Within the cmsd
//   network, things should act as the superuser -- but only for 'stat'
//   (not Open or OpenDir - those should remain 'nobody'!).
   const char* sec_name = client ? ExtractAuthName(*client) : "root";
   hdfsFS fs = hadoop_connect("default", 0, sec_name);
   if (fs == NULL) {
      retc = XrdHdfsSys::Emsg(epname, error, EIO, "stat", fname);
      goto cleanup;
   }

   fileInfo = hdfsGetPathInfo(fs, fname);

// Execute the function
//
   if (fileInfo == NULL) {
      retc = XrdHdfsSys::Emsg(epname, error, errno, "stat", fname);
      goto cleanup;
   }

   buf->st_mode = fileInfo->mPermissions;
   buf->st_mode |= (fileInfo->mKind == kObjectKindDirectory) ? S_IFDIR : S_IFREG;
   buf->st_nlink = (fileInfo->mKind == kObjectKindDirectory) ? 0 : 1;
   buf->st_uid = 1;
   buf->st_gid = 1;
   buf->st_size = (fileInfo->mKind == kObjectKindDirectory) ? 4096 : \
      fileInfo->mSize;
   buf->st_mtime    = fileInfo->mLastMod;
   buf->st_atime    = fileInfo->mLastMod;
   buf->st_ctime    = fileInfo->mLastMod;
   buf->st_dev      = 0;
   buf->st_ino      = 0;

   hdfsFreeFileInfo(fileInfo, 1);

// All went well
//
cleanup:
#if HADOOP_VERSION >= 20
   if (fs)
      hdfsDisconnect(fs);
#endif
   if (fname)
      free(fname);
   return retc;
}


/******************************************************************************/
/*                                  E m s g                                   */
/******************************************************************************/

int XrdHdfsSys::Emsg(const char    *pfx,    // Message prefix value
                       XrdOucErrInfo &einfo,  // Place to put text & error code
                       int            ecode,  // The error code
                       const char    *op,     // Operation being performed
                       const char    *target) // The target (e.g., fname)
{
   char *etext, buffer[XrdOucEI::Max_Error_Len], unkbuff[64];

// Get the reason for the error
//
   if (ecode < 0) ecode = -ecode;
   if (!(etext = strerror(ecode)))
      {sprintf(unkbuff, "reason unknown (%d)", ecode); etext = unkbuff;}

// Format the error message
//
    snprintf(buffer,sizeof(buffer),"Unable to %s %s; %s", op, target, etext);

// Print it out if debugging is enabled
//
#ifndef NODEBUG
   HdfsEroute.Emsg(pfx, buffer);
#endif

// Place the error message in the error object and return
//
    einfo.setErrInfo(ecode, buffer);

    if (errno != 0)
       return (errno > 0) ? -errno : errno;
    return -1;
}
