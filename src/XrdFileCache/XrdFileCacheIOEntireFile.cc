//----------------------------------------------------------------------------------
// Copyright (c) 2014 by Board of Trustees of the Leland Stanford, Jr., University
// Author: Alja Mrak-Tadel, Matevz Tadel, Brian Bockelman
//----------------------------------------------------------------------------------
// XRootD is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// XRootD is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with XRootD.  If not, see <http://www.gnu.org/licenses/>.
//----------------------------------------------------------------------------------

#include <stdio.h>
#include <fcntl.h>

#include "XrdSys/XrdSysError.hh"
#include "XrdSfs/XrdSfsInterface.hh"
#include "XrdSys/XrdSysPthread.hh"

#include "XrdFileCacheIOEntireFile.hh"
#include "XrdFileCacheStats.hh"
#include "XrdFileCacheTrace.hh"

#include "XrdOuc/XrdOucEnv.hh"

using namespace XrdFileCache;

//______________________________________________________________________________


IOEntireFile::IOEntireFile(XrdOucCacheIO2 *io, XrdOucCacheStats &stats, Cache & cache)
   : IO(io, stats, cache),
   m_file(0),
   m_localStat(0)
{
   XrdCl::URL url(GetInput()->Path());
   std::string fname = url.GetPath();

   m_file = Cache::GetInstance().GetFileWithLocalPath(fname, this);
   if (! m_file)
   {
      struct stat st;
      int res = Fstat(st);

      // This should not happen, but make a printout to see it.
      if (res)
         TRACEIO(Error, "IOEntireFile::IOEntireFile, could not get valid stat");

      m_file = new File(this, fname, 0, st.st_size);
   }

   Cache::GetInstance().AddActive(m_file);
}

//______________________________________________________________________________
IOEntireFile::~IOEntireFile()
{
   // called from Detach() if no sync is needed or
   // from Cache's sync thread
   TRACEIO(Debug, "IOEntireFile::~IOEntireFile() ");

   if (m_file) {
      m_cache.Detach(m_file);
      m_file = 0;
   }
   delete m_localStat;
}

//______________________________________________________________________________
int IOEntireFile::Fstat(struct stat &sbuff)
{
   XrdCl::URL url(GetPath());
   std::string name = url.GetPath();
   name += Info::m_infoExtension;

   int res = 0;
   if( ! m_localStat)
   {
      res =  initCachedStat(name.c_str());
      if (res) return res;
   }

   memcpy(&sbuff, m_localStat, sizeof(struct stat));
   return 0;
}

//______________________________________________________________________________
long long IOEntireFile::FSize()
{
   return m_file->GetFileSize();
}

//______________________________________________________________________________
void IOEntireFile::RelinquishFile(File* f)
{
   TRACEIO(Info, "IOEntireFile::RelinquishFile");
   assert(m_file == f);
   m_file = 0;
}

//______________________________________________________________________________

int IOEntireFile::initCachedStat(const char* path)
{
   // Called indirectly from the constructor.

   int res = -1;
   struct stat tmpStat;

   if (m_cache.GetOss()->Stat(path, &tmpStat) == XrdOssOK)
   {
      XrdOssDF* infoFile = m_cache.GetOss()->newFile(Cache::GetInstance().RefConfiguration().m_username.c_str());
      XrdOucEnv myEnv;
      if (infoFile->Open(path, O_RDONLY, 0600, myEnv) == XrdOssOK)
      {
         Info info(m_cache.GetTrace());
         if (info.Read(infoFile, path))
         {
            tmpStat.st_size = info.GetFileSize();
            TRACEIO(Info, "IOEntireFile::initCachedStat successfuly read size from info file = " << tmpStat.st_size);
            res = 0;
         }
         else
         {
            // file exist but can't read it
            TRACEIO(Debug, "IOEntireFile::initCachedStat info file is not complete");
         }
      }
      else
      {
         TRACEIO(Error, "IOEntireFile::initCachedStat can't open info file " << strerror(errno));
      }
      infoFile->Close();
      delete infoFile;
   }

   if (res)
   {
      res = GetInput()->Fstat(tmpStat);
      TRACEIO(Debug, "IOEntireFile::initCachedStat  get stat from client res= " << res << "size = " << tmpStat.st_size);
   }

   if (res == 0)
   {
      m_localStat = new struct stat;
      memcpy(m_localStat, &tmpStat, sizeof(struct stat));
   }
   return res;
}

//______________________________________________________________________________
bool IOEntireFile::ioActive()
{
   if ( ! m_file)
   {
      return false;
   }
   else
   {
      return m_file->ioActive();
   }
}

//______________________________________________________________________________

XrdOucCacheIO *IOEntireFile::Detach()
{
   // Called from XrdPosixFile destructor
   
   TRACEIO(Debug, "IOEntireFile::Detach() ");

   XrdOucCacheIO * io = GetInput();

   if ( ! FinalizeSyncBeforeExit() )
   {
      delete this;
   }
   else
   {
      m_cache.RegisterDyingFilesNeedSync(this);
   }
   
   return io;
}


//______________________________________________________________________________

bool IOEntireFile::FinalizeSyncBeforeExit()
{
   if (m_file)
      return m_file->FinalizeSyncBeforeExit();
   else
      return false;
}

//______________________________________________________________________________

int IOEntireFile::Read (char *buff, long long off, int size)
{
   TRACEIO(Dump, "IOEntireFile::Read() "<< this << " off: " << off << " size: " << size );

   // protect from reads over the file size
   if (off >= FSize())
      return 0;
   if (off < 0)
   {
      errno = EINVAL;
      return -1;
   }
   if (off + size > FSize())
      size = FSize() - off;


   ssize_t bytes_read = 0;
   ssize_t retval = 0;

   retval = m_file->Read(buff, off, size);
   if (retval >= 0)
   {
      bytes_read += retval;
      buff += retval;
      size -= retval;

      if (size > 0)
         TRACEIO(Warning, "IOEntireFile::Read() bytes missed " <<  size );
   }
   else
   {
      TRACEIO(Warning, "IOEntireFile::Read() pass to origin bytes ret " << retval );
   }

   return (retval < 0) ? retval : bytes_read;
}


/*
 * Perform a readv from the cache
 */
int IOEntireFile::ReadV (const XrdOucIOVec *readV, int n)
{
   TRACEIO(Dump, "IO::ReadV(), get " <<  n << " requests" );
   return m_file->ReadV(readV, n);
}

