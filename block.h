#ifndef BLOCK_H
#define BLOCK_H

#include "cache.h"

#include <stdint.h>
#include <map>
#include <vector>

#include <boost/noncopyable.hpp>
#include <boost/optional.hpp>
#include <boost/shared_ptr.hpp>

#include <string.h>

//----------------------------------------------------------------

namespace persistent_data {

	uint32_t const MD_BLOCK_SIZE = 4096;

	typedef uint64_t block_address;

	template <uint32_t BlockSize>
	class block_io : private boost::noncopyable {
	public:
		typedef boost::shared_ptr<block_io> ptr;
		typedef unsigned char buffer[BlockSize];
		typedef unsigned char const const_buffer[BlockSize];

		block_io(std::string const &path, block_address nr_blocks, bool writeable = false);
		~block_io();

		block_address get_nr_blocks() const {
			return nr_blocks_;
		}

		void read_buffer(block_address location, buffer &buf) const;
		void write_buffer(block_address location, const_buffer &buf);

	private:
		int fd_;
		block_address nr_blocks_;
		bool writeable_;
	};

	template <uint32_t BlockSize = MD_BLOCK_SIZE>
	class block_manager : private boost::noncopyable {
	public:
		typedef boost::shared_ptr<block_manager> ptr;

		block_manager(std::string const &path,
			      block_address nr_blocks,
			      unsigned max_concurrent_locks,
			      bool writeable = false);

		typedef unsigned char buffer[BlockSize];
		typedef unsigned char const const_buffer[BlockSize];

		class validator {
		public:
			typedef boost::shared_ptr<validator> ptr;

			virtual ~validator() {}

			virtual void check(const_buffer &b, block_address location) const = 0;
			virtual void prepare(buffer &b, block_address location) const = 0;
		};

		class noop_validator : public validator {
		public:
			void check(const_buffer &b, block_address location) const {}
			void prepare(buffer &b, block_address location) const {}
		};

		enum block_type {
			BT_SUPERBLOCK,
			BT_NORMAL
		};

		struct block {
			typedef boost::shared_ptr<block> ptr;

			block(typename block_io<BlockSize>::ptr io,
			      block_address location,
			      block_type bt,
			      typename validator::ptr v,
			      bool zero = false);
			~block();

			void check_read_lockable() const {
				// FIXME: finish
			}

			void check_write_lockable() const {
				// FIXME: finish
			}

			void flush();

			typename block_io<BlockSize>::ptr io_;
			block_address location_;
			buffer data_;
			typename validator::ptr validator_;
			block_type bt_;
			bool dirty_;
		};
		typedef typename block::ptr block_ptr;

		class read_ref {
		public:
			read_ref(block_manager<BlockSize> const &bm,
				 block_ptr b);
			read_ref(read_ref const &rhs);
			virtual ~read_ref();

			read_ref const &operator =(read_ref const &rhs);

			block_address get_location() const;
			const_buffer &data() const;

		protected:
			friend class block_manager; // FIXME: still needed?
			block_manager<BlockSize> const &bm_;
			block_ptr block_;
			unsigned *holders_;
		};

		// Inherited from read_ref, since you can read a block that's write
		// locked.
		class write_ref : public read_ref {
		public:
			write_ref(block_manager<BlockSize> const &bm,
				  typename block::ptr b);

			using read_ref::data;
			buffer &data();
		};

		// Locking methods
		read_ref
		read_lock(block_address location,
			  typename validator::ptr v =
			  typename validator::ptr(new noop_validator())) const;

		write_ref
		write_lock(block_address location,
			   typename validator::ptr v =
			   typename validator::ptr(new noop_validator()));

		write_ref
		write_lock_zero(block_address location,
				typename validator::ptr v =
				typename validator::ptr(new noop_validator()));

		// The super block is the one that should be written last.
		// Unlocking this block triggers the following events:
		//
		// i) synchronous write of all dirty blocks _except_ the
		// superblock.
		//
		// ii) synchronous write of superblock
		//
		// If any locks are held at the time of the superblock
		// being unlocked then an exception will be thrown.
		write_ref superblock(block_address b,
				     typename validator::ptr v =
				     typename validator::ptr(new noop_validator()));
		write_ref superblock_zero(block_address b,
					  typename validator::ptr v =
					  typename validator::ptr(new noop_validator()));

		block_address get_nr_blocks() const;

		void flush() const;

	private:
		void check(block_address b) const;
		void write_block(block_ptr b) const;

		enum lock_type {
			READ_LOCK,
			WRITE_LOCK
		};

		struct cache_traits {
			typedef typename block::ptr value_type;
			typedef block_address key_type;

			static key_type get_key(value_type const &v) {
				return v->location_;
			}
		};

		typename block_io<BlockSize>::ptr io_;
		mutable base::cache<cache_traits> cache_;

		// FIXME: we need a dirty list as well as a cache

		typedef std::map<block_address, std::pair<lock_type, unsigned> > held_map;
		mutable held_map held_locks_;
	};
}

#include "block.tcc"

//----------------------------------------------------------------

#endif
