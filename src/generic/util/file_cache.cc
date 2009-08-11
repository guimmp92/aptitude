/** \file file_cache.cc */     // -*-c++-*-

//   Copyright (C) 2009 Daniel Burrows
//
//   This program is free software; you can redistribute it and/or
//   modify it under the terms of the GNU General Public License as
//   published by the Free Software Foundation; either version 2 of
//   the License, or (at your option) any later version.
//
//   This program is distributed in the hope that it will be useful,
//   but WITHOUT ANY WARRANTY; without even the implied warranty of
//   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//   General Public License for more details.
//
//   You should have received a copy of the GNU General Public License
//   along with this program; see the file COPYING.  If not, write to
//   the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
//   Boston, MA 02111-1307, USA.

#include "file_cache.h"

#include "sqlite.h"
#include "util.h"

#include <apt-pkg/fileutl.h>

#include <cwidget/generic/threads/threads.h>
#include <cwidget/generic/util/exception.h>
#include <cwidget/generic/util/ssprintf.h>

#include <boost/format.hpp>
#include <boost/make_shared.hpp>

#include <loggers.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace aptitude::sqlite;
namespace cw = cwidget;

namespace aptitude
{
  namespace util
  {
    namespace
    {
      /** \brief An SQLite-backed cache.
       *
       *  The cache schema is defined below as a gigantic string constant.
       *  The reason for splitting blobs out from the main table is
       *  primarily that the incremental blob-reading functions cause
       *  trouble if someone else updates the row containing the blob;
       *  the blob table above will be immutable until it's removed.
       *
       *  Note that referential integrity is maintained on delete by
       *  removing the corresponding entry in the other table; this
       *  makes it easier to remove entries from the cache.
       *
       *  The "format" table is for future use (to support upgrades to
       *  the cache format); it contains a single row, which currently
       *  contains the integer "1" in the "version" field.
       */
      class file_cache_sqlite : public file_cache
      {
	boost::shared_ptr<db> store;
	std::string filename; // Used to report errors.
	/** \brief The maximum size of the cache, in bytes.
	 *
	 *  \todo What's the best way to compute the current size?  I
	 *  could store it in the database and update it as part of
	 *  the transaction that inserts / removes blobs.  Or I could
	 *  use triggers to keep it up-to-date.  Or perhaps there's an
	 *  sqlite query that will return the number of data pages
	 *  used by a table/column?
	 *
	 *  Also, I need to decide when to vacuum the database: after
	 *  each update, after deletes, or what?
	 */
	int max_size;

	// Used to ensure that only one thread is accessing the
	// database connection at once.  Several sqlite3 functions
	// (e.g., sqlite3_last_insert_rowid()) are not threadsafe.
	cw::threads::mutex store_mutex;

	static const int current_version_number = 2;

	void create_new_database()
	{
	  // Note that I rely on the fact that integer primary key
	  // columns autoincrement.  If the largest one is chosen,
	  // then (according to the sqlite documentation) this will
	  // fail gracefully; cache contents will end up in a random
	  // order.  Since the largest key is on the order of 10^18, I
	  // only really care that something halfway sensible happens
	  // in that case.
	  std::string schema = "                                        \
begin transaction							\
create table format ( version integer )					\
									\
create table globals ( TotalBlobSize integer not null )			\
									\
create table cache ( CacheId integer primary key,			\
                     BlobSize integer not null,				\
                     BlobId integer not null,				\
                     Key text not null )				\
									\
create table blobs ( BlobId integer primary key,			\
                     Data blob not null )				\
									\
create index cache_by_blob_id on cache (BlobId)				\
create unique index cache_by_key on cache (Key)				\
									\
									\
create trigger i_cache_blob_size					\
before insert on cache							\
for each row begin							\
    update globals set TotalBlobSize = TotalBlobSize + NEW.BlobSize	\
end									\
									\
create table u_cache_blob_size						\
before insert on cache							\
for each row begin							\
    update globals set TotalBlobSize = TotalBlobSize + NEW.BlobSize - OLD.BlobSize \
end									\
									\
create table d_cache_blob_size						\
before insert on cache							\
for each row begin							\
    update globals set TotalBlobSize = TotalBlobSize - OLD.BlobSize	\
end									\
									\
									\
									\
create trigger fki_cache_blob_id					\
before insert on cache							\
for each row begin							\
    select raise(rollback, 'Insert on table \"cache\" violates foreign key constraint \"fki_cache_blob_id\"') \
    where (select 1 from blobs where blobs.BlobId = NEW.BlobId) is NULL; \
end									\
									\
create trigger fku_cache_blob_id					\
before update of BlobId on cache					\
for each row begin							\
    select raise(rollback, 'Insert on table \"cache\" violates foreign key constraint \"fki_cache_blob_id\"') \
    where (select 1 from blobs where blobs.BlobId = NEW.BlobId) is NULL; \
end									\
									\
create trigger fkd_cache_blob_id					\
before delete on cache							\
for each row begin							\
    delete from blobs							\
       where blobs.BlobId = OLD.BlobId					\
        and (select 1 from cache where cache.BlobId = OLD.BlobId) is NULL; \
end									\
									\
									\
insert into globals(TotalBlobSize) values(0)				\
";

	  store->exec(schema);

	  boost::shared_ptr<statement> set_version_statement =
	    statement::prepare(*store, "insert into format(version) values(?)");

	  set_version_statement->bind_int(1, current_version_number);
	  set_version_statement->exec();

	  statement::prepare(*store, "commit")->exec();

	  sanity_check_database();
	}

	void sanity_check_database()
	{
	  try
	    {
	      store->exec("begin transaction");

	      try
		{
		  // Currently just checks the version -- but we could also
		  // check the format and so on here.
		  boost::shared_ptr<statement> get_version_statement =
		    statement::prepare(*store, "select version from format");
		  if(!get_version_statement->step())
		    throw FileCacheException("Can't read the cache version number.");
		  else if(get_version_statement->get_int(0) != current_version_number)
		    // TODO: probably it's better to just blow away
		    // the cache in this case.  For a simple download
		    // cache, it's not worth the effort to upgrade
		    // database formats -- better to just throw away
		    // the data.
		    throw FileCacheException("Unsupported version number.");


		  boost::shared_ptr<statement> get_total_size_statement =
		    statement::prepare(*store, "select TotalBlobSize from globals");
		  if(!get_total_size_statement->step())
		    throw FileCacheException("Can't read the total size of all the files in the database.");
		  sqlite3_int64 total_size = get_total_size_statement->get_int64(0);

		  boost::shared_ptr<statement> compute_total_size_statement =
		    statement::prepare(*store, "select sum(BlobSize) from cache");
		  if(!compute_total_size_statement->step())
		    throw FileCacheException("Can't compute the total size of all the files in the database.");
		  sqlite3_int64 computed_total_size = compute_total_size_statement->get_int64(0);

		  if(computed_total_size != total_size)
		    {
		      LOG_WARN(Loggers::getAptitudeDownloadCache(),
			       boost::format("Inconsistent cache state: the stored total size %d does not match the actual total size %d.  Fixing it.")
			       % total_size % computed_total_size);

		      boost::shared_ptr<statement> fix_total_size_statement =
			statement::prepare(*store, "update globals set TotalSize = ?");
		      fix_total_size_statement->bind_int64(1, computed_total_size);
		      fix_total_size_statement->exec();
		    }

		  store->exec("commit");
		}
	      catch(...)
		{
		  try
		    {
		      store->exec("rollback");
		    }
		  catch(...)
		    {
		    }

		  // TODO: maybe if something fails we should just
		  // delete and re-create the database?

		  throw;
		}
	    }
	  catch(sqlite::exception &ex)
	    {
	      throw FileCacheException("Can't sanity-check the database: " + ex.errmsg());
	    }
	}

      public:
	file_cache_sqlite(const std::string &_filename, int _max_size)
	  : store(db::create(_filename)),
	    filename(_filename),
	    max_size(_max_size)
	{
	  // Set up the database.  First, check the format:
	  sqlite::db::statement_proxy check_for_format_statement =
	    store->get_cached_statement("select 1 from sqlite_master where name = 'format'");
	  if(!check_for_format_statement->step())
	    create_new_database();
	  else
	    sanity_check_database();
	}

	void putItem(const std::string &key,
		     const std::string &path)
	{
	  cw::threads::mutex::lock l(store_mutex);

	  LOG_TRACE(Loggers::getAptitudeDownloadCache(),
		    "Caching " << path << " as " << key);

	  try
	    {
	      // Here's the plan:
	      //
	      // 1) Open the file and get its size with fstat().
	      // 2) If the file is too large to ever cache, return
	      //    immediately (don't cache it).
	      // 3) In an sqlite transaction:
	      //    3.a) Retrieve and save the keys of entries,
	      //         starting with the oldest, until removing
	      //         all the stored keys would create enough
	      //         space for the new entry.
	      //    3.b) Delete the entries that were saved.
	      //    3.c) Place the new entry into the cache.


	      // Step 1)
	      FileFd fd;
	      if(!fd.Open(path, FileFd::ReadOnly))
		{
		  std::string msg = cw::util::sstrerror(errno);
		  throw FileCacheException((boost::format("Can't open \"%s\" to store it in the cache: %s")
					    % path % msg).str());
		}

	      struct stat buf;
	      if(fstat(fd.Fd(), &buf) != 0)
		{
		  std::string msg = cw::util::sstrerror(errno);
		  throw FileCacheException((boost::format("Can't determine the size of \"%s\" to store it in the cache: %s")
					    % path % msg).str());
		}

	      // Step 2)
	      if(buf.st_size > max_size)
		{
		  LOG_INFO(Loggers::getAptitudeDownloadCache(),
			   "Refusing to cache \"" << path << "\" as \"" << key
			   << "\": its size " << buf.st_size << " is greater than the cache size limit " << max_size);
		  return;
		}

	      store->exec("begin transaction");

	      // Step 3)
	      try
		{
		  sqlite::db::statement_proxy get_total_size_statement =
		    store->get_cached_statement("select TotalBlobSize from globals");
		  if(!get_total_size_statement->step())
		    throw FileCacheException("Can't read the total size of all the files in the database.");
		  sqlite3_int64 total_size = get_total_size_statement->get_int64(0);

		  if(total_size + buf.st_size > max_size)
		    {
		      bool first = true;
		      sqlite3_int64 last_cache_id_dropped = -1;
		      sqlite3_int64 amount_dropped = 0;

		      // Step 3.a)
		      {
			sqlite::db::statement_proxy read_entries_statement =
			  store->get_cached_statement("select CacheId, BlobSize from cache order by CacheId");

			while(total_size + buf.st_size - amount_dropped > max_size &&
			      read_entries_statement->step())
			  {
			    first = false;
			    last_cache_id_dropped = read_entries_statement->get_int64(0);
			    amount_dropped += read_entries_statement->get_int64(1);
			  }

			if(first)
			  throw FileCacheException("Internal error: no cached files, but the total size is nonzero.");
		      }

		      // Step 3.b)
		      {
			sqlite::db::statement_proxy delete_old_statement =
			  store->get_cached_statement("delete from cache where CacheId <= ?");
			delete_old_statement->bind_int64(1, last_cache_id_dropped);
			delete_old_statement->exec();
		      }

		      // Step 3.c)
		      {
			// The blob has to be inserted before the
			// cache entry, so the foreign key constraints
			// are maintained.

			// Insert a zeroblob first, so we can write it
			// incrementally.
			{
			  sqlite::db::statement_proxy insert_blob_statement =
			    store->get_cached_statement("insert into blob (Data) values (zeroblob(?))");
			  insert_blob_statement->bind_int64(1, buf.st_size);
			  insert_blob_statement->exec();
			}

			sqlite3_int64 inserted_blob_row =
			  store->get_last_insert_rowid();

			// Delete any existing entries for the same
			// key.  This hopefully
			{
			  sqlite::db::statement_proxy delete_key_statement =
			    store->get_cached_statement("delete from cache where Key = ?");
			  delete_key_statement->bind_string(1, key);
			  delete_key_statement->exec();
			}

			// Insert the corresponding entry in the cache
			// table.
			{
			  sqlite::db::statement_proxy insert_cache_statement =
			    store->get_cached_statement("insert or rollback into cache (BlobId, BlobSize, Key) values (?, ?, ?)");
			  insert_cache_statement->bind_int64(1, inserted_blob_row);
			  insert_cache_statement->bind_int64(2, buf.st_size);
			  insert_cache_statement->bind_string(3, key);
			  insert_cache_statement->exec();
			}

			boost::shared_ptr<blob> blob_data =
			  sqlite::blob::open(*store,
					     "main",
					     "blobs",
					     "Data",
					     inserted_blob_row);

			int amount_to_write(buf.st_size);
			int blob_offset = 0;
			static const int block_size = 16384;
			// This can safely be static, since we acquire
			// a mutex before we enter this routine.
			static char buf[block_size];
			while(amount_to_write > 0)
			  {
			    int curr_amt;
			    if(amount_to_write < block_size)
			      curr_amt = static_cast<int>(amount_to_write);
			    else
			      curr_amt = block_size;

			    int amt_read = read(fd.Fd(), buf, curr_amt);
			    if(amt_read == 0)
			      throw FileCacheException((boost::format("Unexpected end of file while reading %s into the cache.") % path).str());
			    else if(amt_read < 0)
			      {
				std::string errmsg(cw::util::sstrerror(errno));
				throw FileCacheException((boost::format("Error while reading %s into the cache: %s.") % path % errmsg).str());
			      }

			    blob_data->write(blob_offset, buf, curr_amt);
			    blob_offset += amt_read;
			    amount_to_write -= amt_read;
			  }
		      }
		    }


		  store->exec("commit");
		}
	      catch(...)
		{
		  // Try to roll back, but don't throw a new exception if
		  // that fails too.
		  try
		    {
		      store->exec("rollback");
		    }
		  catch(...)
		    {
		    }

		  // TODO: maybe instead of rethrowing the exception,
		  // we should delete the cache tables and recreate
		  // them?  But this should only happen if there was a
		  // *database* error rather than an error, e.g.,
		  // reading the file data to cache.
		  throw;
		}
	    }
	  catch(cw::util::Exception &ex)
	    {
	      LOG_WARN(Loggers::getAptitudeDownloadCache(),
		       boost::format("Can't cache \"%s\" as \"%s\": %s")
		       % path % key % ex.errmsg());
	    }
	}

	temp::name getItem(const std::string &key)
	{
	  cw::threads::mutex::lock l(store_mutex);

	  // Here's the plan.
	  //
	  // 1) In an sqlite transaction:
	  //    1.a) Look up the cache entry corresponding
	  //         to this key.
	  //    1.a.i)  If there is no entry, return an invalid name.
	  //    1.a.ii) If there is an entry,
	  //        1.a.ii.A) Update its last use field.
	  //        1.a.ii.B) Extract it to a temporary file
	  //                  and return it.
	  try
	    {
	      store->exec("begin transaction");

	      try
		{
		  sqlite::db::statement_proxy find_cache_entry_statement =
		    store->get_cached_statement("select CacheId, BlobId from cache where Key = ?");
		  find_cache_entry_statement->bind_string(1, key);

		  if(!find_cache_entry_statement->step())
		    // 1.a.i: no matching entry
		    {
		      store->exec("rollback");
		      return temp::name();
		    }
		  else
		    {
		      sqlite3_int64 oldCacheId = find_cache_entry_statement->get_int64(0);
		      sqlite3_int64 blobId = find_cache_entry_statement->get_int64(1);

		      // 1.a.ii.A: update the last use field.
		      {
			// WARNING: this might fail if the largest
			// cache ID has been used.  That should never
			// happen in aptitude (you'd need 10^18 get or
			// put calls), and trying to avoid it seems
			// like it would cause a lot of trouble.
			sqlite::db::statement_proxy update_last_use_statement =
			  store->get_cached_statement("update cache set CacheId = max(select CacheId from cache) where CacheId = ?");
			update_last_use_statement->bind_int64(0, oldCacheId);
			update_last_use_statement->exec();
		      }

		      // TODO: I should consolidate the temporary
		      // directories aptitude creates.
		      temp::dir d("aptitudeCacheExtract");
		      temp::name rval(d, "extracted");

		      {
			FileFd outfile(rval.get_name(), FileFd::WriteEmpty, 0644);

			boost::shared_ptr<sqlite::blob> blob_data =
			  sqlite::blob::open(*store,
					     "main",
					     "blobs",
					     "Data",
					     blobId,
					     false);

			static const int block_size = 16384;
			// Note: this is safe because we hold a mutex
			// for the duration of this method.
			static char buf[block_size];

			int amount_to_read = blob_data->size();
			int blob_offset = 0;

			// Copy the blob into the temporary file.
			while(amount_to_read > 0)
			  {
			    int curr_amt;

			    if(amount_to_read < block_size)
			      curr_amt = amount_to_read;
			    else
			      curr_amt = block_size;

			    blob_data->read(blob_offset, buf, curr_amt);
			    int amt_written = write(outfile.Fd(), buf, curr_amt);

			    if(amt_written < 0)
			      {
				std::string errmsg(cw::util::sstrerror(errno));
				throw FileCacheException((boost::format("Can't open \"%s\" for writing: %s")
							  % rval.get_name() % errmsg).str());
			      }
			    else if(amt_written == 0)
			      {
				throw FileCacheException((boost::format("Unable to write to \"%s\".")
							  % rval.get_name()).str());
			      }
			    else
			      {
				blob_offset += amt_written;
				amount_to_read -= amt_written;
			      }
			  }
		      }

		      store->exec("commit");
		      return rval;
		    }
		}
	      catch(...)
		{
		  // Try to roll back, but don't throw a new exception if
		  // that fails too.
		  try
		    {
		      store->exec("rollback");
		    }
		  catch(...)
		    {
		    }

		  // TODO: maybe instead of rethrowing the exception,
		  // we should delete the cache tables and recreate
		  // them?  But this should only happen if there was a
		  // *database* error rather than an error, e.g.,
		  // reading the file data to cache.
		  throw;
		}
	    }
	  catch(cw::util::Exception &ex)
	    {
	      LOG_WARN(Loggers::getAptitudeDownloadCache(),
		       boost::format("Can't get the cache entry for \"%s\": %s")
		       % key % ex.errmsg());
	      return temp::name();
	    }
	}
      };

      /** \brief A multilevel cache.
       *
       *  "get" requests are serviced from each sub-cache in turn,
       *  failing if the object isn't found in any cache.
       *
       *  "put" requests are forwarded to all sub-caches.
       */
      class file_cache_multilevel : public file_cache
      {
	std::vector<boost::shared_ptr<file_cache> > caches;

      public:
	file_cache_multilevel()
	{
	}

	void push_back(const boost::shared_ptr<file_cache> &cache)
	{
	  caches.push_back(cache);
	}


	void putItem(const std::string &key, const std::string &path)
	{
	  for(std::vector<boost::shared_ptr<file_cache> >::const_iterator
		it = caches.begin(); it != caches.end(); ++it)
	    (*it)->putItem(key, path);
	}

	temp::name getItem(const std::string &key)
	{
	  for(std::vector<boost::shared_ptr<file_cache> >::const_iterator
		it = caches.begin(); it != caches.end(); ++it)
	    {
	      temp::name found = (*it)->getItem(key);
	      if(found.valid())
		return found;
	    }

	  return temp::name();
	}
      };
    }

    boost::shared_ptr<file_cache> file_cache::create(const std::string &filename,
						     int memory_size,
						     int disk_size)
    {
      boost::shared_ptr<file_cache_multilevel> rval = boost::make_shared<file_cache_multilevel>();

      if(memory_size > 0)
	{
	  try
	    {
	      // \note A boost::multi_index_container might be more
	      // efficient for the in-memory cache.  OTOH, it would
	      // require more code.
	      rval->push_back(boost::make_shared<file_cache_sqlite>(":memory:", memory_size));
	    }
	  catch(const cw::util::Exception &ex)
	    {
	      LOG_WARN(Loggers::getAptitudeDownloadCache(),
		       "Unable to create the in-memory cache: " << ex.errmsg());
	    }
	}
      else
	LOG_INFO(Loggers::getAptitudeDownloadCache(),
		 "In-memory cache disabled.");


      if(disk_size > 0)
	{
	  try
	    {
	      rval->push_back(boost::make_shared<file_cache_sqlite>(filename, disk_size));
	    }
	  catch(const cw::util::Exception &ex)
	    {
	      LOG_WARN(Loggers::getAptitudeDownloadCache(),
		       "Unable to open the on-disk cache \"" << filename << "\": " << ex.errmsg());
	    }
	}
      else
	LOG_INFO(Loggers::getAptitudeDownloadCache(),
		 "On-disk cache disabled.");

      return rval;
    }

    file_cache::~file_cache()
    {
    }
  }
}
