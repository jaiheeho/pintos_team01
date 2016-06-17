#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "filesys/cache.h"
#include "devices/disk.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

#define INDIRECT_MAX_SIZE 0x00010000 /* 64KB*/
#define MAX_SIZE          0x00800000 /* 8MB*/
/* On-disk inode.
   Must be exactly DISK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    disk_sector_t start;                /* First data sector. */
    off_t length;                        /*File size in bytes. */
    unsigned magic;                     /* Magic number. */
    uint32_t links[125];               /* Not used. */
  };

// struct inode_disk
// {
//   struct inode_disk *links[128];
// };


/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, DISK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    disk_sector_t sector;               /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct inode_disk data;             /* Inode content. */
  };
bool inode_free_map_allocate(size_t, struct inode_disk *);
void inode_free_map_release(size_t, struct inode_disk *);
bool inode_free_map_add(size_t, off_t, struct inode *);

/* Returns the disk sector that contains byte offset POS within
   INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
// static disk_sector_t
// byte_to_sector (const struct inode *inode, off_t pos) 
// {
//   ASSERT (inode != NULL);
//   if (pos < inode->data.length)
//     return inode->data.start + pos / DISK_SECTOR_SIZE;
//   else
//     return -1;
// }


static disk_sector_t
byte_to_sector (const struct inode *inode, off_t pos) 
{
  ASSERT (inode != NULL);
  struct inode_disk indirect;

  int length = inode_length(inode);

  // printf("in byte_to_sector :pos : %d, length%d\n",pos, length);
  if (pos < length)
  {
    length = pos / DISK_SECTOR_SIZE;

    int indirect_size = (length % INDIRECT_MAX_SIZE) +1 ;
    int direct_size = (length % INDIRECT_MAX_SIZE) / (DISK_SECTOR_SIZE);

    memset(&indirect, 0, sizeof(struct inode_disk));
    buffer_cache_read((disk_sector_t)inode->data.links[indirect_size], (char *)&indirect, DISK_SECTOR_SIZE, 0);
    return (disk_sector_t) indirect.links[direct_size];
  }
  else
  {
    // printf("hererer1\n");
    return -1;
  }
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   disk.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (disk_sector_t sector, off_t length)
{
  struct inode_disk * disk_inode = NULL;

  bool success = false;

  ASSERT (length >= 0);
  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == DISK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);

  if (disk_inode != NULL)
  {     

    printf("inode creat:length of inode :%d \n", length);
    // inode_free_map_add(0, length, disk_inode);
    success = inode_free_map_allocate (length, disk_inode);
    // struct inode_disk indirect;
    // struct inode_disk double_indirect;

    // buffer_cache_read((disk_sector_t)disk_inode->links[0], (char *)&double_indirect, DISK_SECTOR_SIZE, 0);
    // buffer_cache_read((disk_sector_t)double_indirect.links[0], (char *)&indirect, DISK_SECTOR_SIZE, 0);
    // printf("inode creatdouble indirect : %d, indirect : %d, size : %d\n", disk_inode->links[0],double_indirect.links[0], indirect.links[0]);
    // length = (int) indirect.links[0]; 
    disk_write (filesys_disk, sector, disk_inode);
  } 

  free (disk_inode);
  return success;
}

bool inode_free_map_add(size_t size, off_t pos, struct inode *inode)
{

  int length = bytes_to_sectors(size);

  int indirect_size = (length / INDIRECT_MAX_SIZE)  + 1;
  int direct_size = (length % INDIRECT_MAX_SIZE)/ (DISK_SECTOR_SIZE) +1;

  int _length = bytes_to_sectors(pos);

  int _indirect_size = (_length / INDIRECT_MAX_SIZE)  + 1;
  int _direct_size = (_length % INDIRECT_MAX_SIZE)/ (DISK_SECTOR_SIZE) +1;

  struct inode_disk * indirect = NULL;
  struct inode_disk * double_indirect = &inode->data;
  int i,j;
  int _j;

  ASSERT(indirect_size < DISK_SECTOR_SIZE/4);
  ASSERT(direct_size < DISK_SECTOR_SIZE/4);

  char zeros[DISK_SECTOR_SIZE];
  memset(zeros, 0, DISK_SECTOR_SIZE);
  bool start = true;

  printf("size : %d new size : %d\n", size, pos);
  printf("AT ADD; length : %d, indirect_size; %d, direct_size:%d \n",
    length, indirect_size, direct_size);

  printf("AT ADD end; length : %d , indirect_size; %d, direct_size:%d \n",
    _length, _indirect_size, _direct_size);


  for (i = indirect_size-1 ; i < _indirect_size; i ++)
  {
    disk_sector_t indirect_sector;
    indirect = calloc (1, sizeof (struct inode_disk));
    if (start)
    {
      buffer_cache_read((disk_sector_t)double_indirect->links[i], (char *)indirect, DISK_SECTOR_SIZE, 0);
      j = direct_size;
      if (indirect_size == _indirect_size)
        _j = _direct_size;
      else 
        _j = DISK_SECTOR_SIZE/4;
    }
    else
    {
      free_map_allocate(1, &indirect_sector);
      double_indirect->links[i] = indirect_sector;
      j = 0;
      _j = DISK_SECTOR_SIZE/4;
      if (i == _indirect_size-1)
        _j = _direct_size;
    }
    for (; j <_j ; j++)
    {
      free_map_allocate(1,(disk_sector_t *)&indirect->links[j]);
      buffer_cache_write((disk_sector_t)indirect->links[j], zeros, DISK_SECTOR_SIZE, 0, 0 );
    }
    buffer_cache_write((disk_sector_t)double_indirect->links[i], (char*)indirect, DISK_SECTOR_SIZE, 0,0);
    free(indirect);
  }

  inode->data.length = pos;

  buffer_cache_write((disk_sector_t)inode->sector, (char*)double_indirect, DISK_SECTOR_SIZE, 0,0);
  return true;   
}

bool inode_free_map_allocate(size_t size, struct inode_disk *disk_inode)
{
  int _length = bytes_to_sectors(size);

  int _indirect_size = (_length / INDIRECT_MAX_SIZE)  + 1;
  int _direct_size = (_length % INDIRECT_MAX_SIZE)/(DISK_SECTOR_SIZE) +1;


  struct inode_disk * indirect = NULL;
  struct inode_disk * double_indirect = disk_inode;
  int i,j;
  int _j;

  ASSERT(_indirect_size < DISK_SECTOR_SIZE/4);
  ASSERT(_direct_size < DISK_SECTOR_SIZE/4);

  char zeros[DISK_SECTOR_SIZE];
  memset(zeros, 0, DISK_SECTOR_SIZE);


  printf("AT Allocate end; length : %d, indirect_size; %d, direct_size:%d \n",
    _length, _indirect_size, _direct_size);

  for (i = 0; i < _indirect_size; i ++)
  {
    disk_sector_t indirect_sector;
    indirect = calloc (1, sizeof (struct inode_disk));
    free_map_allocate(1, &indirect_sector);
    double_indirect->links[i] = indirect_sector;
    _j = DISK_SECTOR_SIZE/4;
    if (i == _indirect_size-1)
      _j = _direct_size;
    for (j = 0; j <_j ; j++)
    {
      free_map_allocate(1,(disk_sector_t *)&indirect->links[j]);
      buffer_cache_write((disk_sector_t)indirect->links[j], zeros, DISK_SECTOR_SIZE, 0, 0 );
    }
    buffer_cache_write((disk_sector_t)double_indirect->links[i], (char*)indirect, DISK_SECTOR_SIZE, 0,0);
    free(indirect);
  }
  disk_inode->length = size;
  return true;   
}

void inode_free_map_release(size_t size, struct inode_disk *disk_inode)
{
  int _length = bytes_to_sectors(size);

  int _indirect_size = (_length / INDIRECT_MAX_SIZE)  + 1;
  int _direct_size = (_length % INDIRECT_MAX_SIZE)/(DISK_SECTOR_SIZE) +1;

  struct inode_disk * indirect = calloc (1, sizeof (struct inode_disk));
  struct inode_disk * double_indirect = disk_inode;
  int i,j;
  int _j;

  ASSERT(_indirect_size < DISK_SECTOR_SIZE/4);
  ASSERT(_direct_size < DISK_SECTOR_SIZE/4);

  char zeros[DISK_SECTOR_SIZE];
  memset(zeros, 0, DISK_SECTOR_SIZE);

  // printf("size : %d new size : %d\n", size, pos);
  // printf("AT ADD; length : %d, double_indirect_size: %d, indirect_size; %d, direct_size:%d \n",
  //   length, double_indirect_size, indirect_size, direct_size);

  // printf("AT ADD end; length : %d, double_indirect_size: %d, indirect_size; %d, direct_size:%d \n",
  //   _length, _double_indirect_size, _indirect_size, _direct_size);

  for (i = 0; i < _indirect_size; i ++)
  {
    memset(indirect, 0, DISK_SECTOR_SIZE);
    buffer_cache_read((disk_sector_t)double_indirect->links[i], (char *)indirect, DISK_SECTOR_SIZE, 0);

    _j = DISK_SECTOR_SIZE/4;
    if (i == _indirect_size-1)
      _j = _direct_size;
    for (j = 0; j <_j ; j++)
    {
      free_map_release((disk_sector_t)indirect->links[j],1);
    }
    free_map_release((disk_sector_t)double_indirect->links[i],1);
  }
}

 
 /* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (disk_sector_t sector) 
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          return inode; 
        }
    }
  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  // disk_read (filesys_disk, inode->sector, &inode->data);
  buffer_cache_read(inode->sector, &inode->data, DISK_SECTOR_SIZE, 0);
  // printf("test in open: sector : %d\n", sector);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
disk_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  off_t bytes_read = 0;
  int length = inode_length (inode);
  int inode_left = length;
  /* before close inode flush buffer cache*/

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
  {
    while (inode_left >0)
    {
      disk_sector_t sector_idx = byte_to_sector (inode, bytes_read);
      buffer_cache_elem_free(sector_idx);
      bytes_read += DISK_SECTOR_SIZE;
      inode_left -= DISK_SECTOR_SIZE;
    }

    /* Remove from inode list and release lock. */
    list_remove (&inode->elem);

    /* Deallocate blocks if removed. */
    if (inode->removed) 
      {
        inode_free_map_release(length, &inode->data);
        free_map_release (inode->sector, 1);
      }
    free (inode); 
  }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;

  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      // printf("read _at 1 : offset : %d \n", offset);
      disk_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % DISK_SECTOR_SIZE;
      // printf("read _at 2 : sector_idx : %d\n",sector_idx);
      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = DISK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      /* before directly access to the disk, we will check buffer cache*/
      buffer_cache_read(sector_idx,(char*) buffer + bytes_read,chunk_size, sector_ofs);
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;


  if (inode->deny_write_cnt)
    return 0;

  /* extend file*/
  int length = inode_length (inode);

  // printf("length :  %d\n",length);
  if (length < size+offset)
    inode_free_map_add (length, size + offset, inode);

  length = inode_length (inode);
  // printf("length : %d\n", length);

  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      // printf("write _at 1 : offset : %d size : %d \n", offset, size);
      disk_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % DISK_SECTOR_SIZE;
      // printf("write _at 2 : sector_idx : %d\n",sector_idx);

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = length - offset;
      // printf("write _at 3 : inode_left : %d\n",inode_left);

      int sector_left = DISK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

       // Number of bytes to actually write into this sector. 
      int chunk_size = size < min_left ? size : min_left;
      // printf("write _at 4 : chunk_size : %d\n",chunk_size);

      if (chunk_size <= 0)
        break;

      /* If the sector contains data before or after the chunk
   we're writing, then we need to read in the sector
   first.  Otherwise we start with a sector of all zeros. */
      if (sector_ofs > 0 || chunk_size < sector_left) 
        buffer_cache_write(sector_idx, buffer + bytes_written, chunk_size, sector_ofs, 1);
      else
        buffer_cache_write(sector_idx, buffer + bytes_written,
          chunk_size, sector_ofs, 0);
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    } 
    buffer_cache_write(inode->sector, (char*)&inode->data, DISK_SECTOR_SIZE, 0 , 0);
  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  return inode->data.length;
}