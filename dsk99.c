#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>


/*
 ****************************************************************************
 *                               Constants
 ****************************************************************************
 */

#define BLOCK_VIB         0
#define BLOCK_FIB_INDEX   1
#define DISK_NAME_LEN    10
#define FILE_NAME_LEN    10
#define MAX_FILE_COUNT  128
#define SECTOR_SIZE     256

enum
{
  fib_program = 0x01,  // Set for program files
  fib_binary  = 0x02,  // Set for binary files
  fib_wp      = 0x08,  // Set if file is write-protected
  fib_var     = 0x80   // Set if file uses variable-length records
};

enum
{
  file_new = 0x1,  // New file to be added
  file_old = 0x2,  // Existing file in disk image
  file_del = 0x4,  // Existing file to delete
  file_ext = 0x8   // Existing file to extract
};
     

/*
 ****************************************************************************
 *                               Structures
 ****************************************************************************
 */

struct vib_block
{
  char name[10];	// Disk volume name (10 characters, pad with spaces)
  short physrecs;	// Total number of physrecs on disk (usually 360)
  char secspertrack;	// Sectors per track (usually 9 (FM SD)
  char id[3];		// 'DSK'
  char protection;	// 'P' if disk is protected, ' ' otherwise.
  char cylinders;	// Tracks per side (usually 40)
  char heads;		// Sides (1 or 2)
  char density;		// Density: 1 (FM SD), 2 (MFM DD), or 3 (MFM HD)
  char reserved[36];
  char abm[200];	// Allocation bitmap: there is one bit per AU.
};

struct fib_block
{
  char  name[10];            // File name (10 characters, pad with spaces)
  char  reserved[2];
  unsigned char  flags;      // File status flags
  unsigned char  recsperphysrec;  // Logical records per physrec
  short physrec_count;    // File length in physrecs
  unsigned char  eof;     // EOF offset in last physrec for variable length
  unsigned char  reclen;  // Logical record size in bytes ([1,255] 0->256)
  short fixrecs;          // File length in logical records
  char  reserved2[8];
  char  cluster[76][3];   // Data cluster table
};

struct disk_sector
{
  short data[128];
};

// This is an internal representation of the disk
struct file_arg
{
  char file_name[256];
  char output_name[256];
  int  add;
  int  remove;
  int  extract;
  int  program;
  int  fixed;
  int  variable;
  int  binary;
  int  ascii;
  int  protect;
  int  unprotect;
  int  record_size;
};

struct top_args
{
  char disk_name[DISK_NAME_LEN+1];       // Name of disk
  char image_path[256];                  // Path to disk image
  int  file_count;                       // Number of file commands
  int  unprotect;                        // Clear protection flag?
  int  protect;                          // Set protection flag?
  int  create_new;                       // Create new disk image
  int  use_existing;                     // Use existing disk image
  int  list_contents;                    // List image contents
  int  verbose;                          // Use verbose output
  int  extract_all;                      // Extract all files from image
  int  show_help;                        // Display help
  struct file_arg file[MAX_FILE_COUNT];  // List of file operations
};


/*
 ****************************************************************************
 *                               Globals
 ****************************************************************************
 */

struct top_args all_args;
void* disk_buffer;
int disk_size;


/*
 ****************************************************************************
 *                                 Code
 ****************************************************************************
 */


/*===========================================================================
 *                                swap
 *===========================================================================
 * Desription: Swap bytes if this is a little-endian system
 *
 * Parameters: val - Value to swap
 *
 * Return:     Swapped value
 */
short swap(short val)
{
  char test[]={0,1};
  if(*((short*)&test[0]) == 1) return(val);
  return(((val<<8)&0xFF00) | (val >> 8));
}


/*===========================================================================
 *                             cluster_first
 *===========================================================================
 * Desription: Extract first cluster from FIB cluster span record
 *
 * Parameters: cluster - FIB cluster record
 *
 * Return:     First cluster number in span
 */
int cluster_first(unsigned char* cluster)
{
  // first: ABC
  // count: DEF
  // cluster: BC FA DE
  return(((int)(cluster[1] & 0xF) << 8) | cluster[0]);
}


/*===========================================================================
 *                             cluster_count
 *===========================================================================
 * Desription: Extract cluster count from FIB cluster span record
 *
 * Parameters: cluster - FIB cluster record
 *
 * Return:     First cluster number in span
 */
int cluster_count(unsigned char* cluster)
{
  // first: ABC
  // count: DEF
  // cluster: BC FA DE
  return((((int)cluster[2]) << 4) | (cluster[1] >> 4));
}


/*===========================================================================
 *                              make_cluster
 *===========================================================================
 * Desription: Make cluster record
 *
 * Parameters: cluster - Pointer to cluster record
 *             first   - First sector in cluster
 *             count   - Sector count in cluster
 *
 * Return:     First cluster number in span
 */
void make_cluster(unsigned char* cluster, int first, int count)
{
  // first: ABC
  // count: DEF
  // cluster: BC FA DE
  cluster[0] = first & 0xFF;
  cluster[1] = ((first >> 8) & 0x0F) | ((count << 4) & 0xF0);
  cluster[2] = (count >> 4) & 0xFF;
}


/*===========================================================================
 *                               make_name
 *===========================================================================
 * Desription: Convert a C string to a V9T9 name
 *
 * Parameters: dst  - Pointer to V9T9 name
 *             src  - Pointer to source name
 *             size - Maximum size of the V9T9 name 
 *
 * Return:     First cluster number in span
 */
void make_name(char* dst, char* src, int size)
{
  int i;
  for(i = 0; i < size; i++)
  {
    // Convert name to upper case
    // Replace dots and spaces with underscores
    char t = *src++;
    if(t == 0) break; 
    if(t >= 'a' && t <= 'z') t = t-'a'+'A';
    if(t == '.' || t == ' ') t = '_';
    *dst++ = t;
  }
  // Pad name with spaces
  for(;i < size; i++) *dst++ = ' ';
}


/*===========================================================================
 *                              show_help
 *===========================================================================
 * Desription: Display help text to the user
 *
 * Parameters: None
 *
 * Return:     None
 */
void show_help()
{
  printf("dsk99 - TI 99/4A Floppy Disk Management Tool\n");
  printf("\n");
  printf("Usage:\n");
  printf("\n");
  printf("dsk99 {options} {disk image} [-n {disk name}]\n");
  printf("      [{options} {filename} ... [-o {output name}]]\n");
  printf("\n");
  printf("Disk Options\n");
  printf("  -c : Create new disk image\n");
  printf("  -e : Use existing disk image\n");
  printf("  -U : Clear protect flag\n");
  printf("  -W : Set protect flag\n");
  printf("  -n : Set disk name\n");
  printf("  -l : List disk contents\n");
  printf("  -X : Extract all files\n");
  printf("\n");
  printf("File Options\n");
  printf("  -p : File is a program\n");
  printf("  -d : File contains ASCII data\n");
  printf("  -i : File contains binary data\n");
  printf("  -u : File is not write-protected\n");
  printf("  -w : File is write-protected\n");
  printf("  -f{record size} : File contains fixed records of indicated size\n");
  printf("  -v{record size} : File contains variable records of maximum indicated size\n");
  printf("  -a : Add file to image\n");
  printf("  -r : Remove file from image\n");
  printf("  -x : Extract file from image\n");
  printf("  -o : Specify output name\n");
  printf("\n");
  printf("Global Options\n");
  printf("  -V : Verbose output\n");
  printf("\n");
  printf("Examples\n");
  printf("  List the contents of a disk image\n");
  printf("    dsk99 -l disk.v9t9\n");
  printf("\n");
  printf("  Extract all files from a disk image\n");
  printf("    dsk99 -X disk.v9t9\n");
  printf("\n");
  printf("  Add all files in the current directory to a new disk image as programs\n");
  printf("    dsk99 -c disk.v9t9 -ap *\n");
  printf("\n");
  printf("  Add a local file \"records1.dat\" as a \"dis/fix 80\" file named \"fixrec\"\n");
  printf("    dsk99 -c disk.v9t9 -adf80 records1.dat -o fixrec\n");
  printf("\n");
  printf("  Change the existing file \"fixrec\" filetype to \"dis/fix 40\"\n");
  printf("    dsk99 -e disk.v9t9 -df40 records1.dat -o fixrec\n");
  printf("\n");
  printf("  Extract a disk image file named \"fixrec\" to a local file named \"records1.dat\"\n");
  printf("    dsk99 -e disk.v9t9 -x fixrec -o records1.dat\n");
}


/*===========================================================================
 *                             parse_arguments
 *===========================================================================
 * Desription: Parse the command line arguments
 *
 * Parameters: argc - Number of arguments
 *             argv - Array of arguments
 *
 * Return:     Were the arguments parsed correcctly?
 */
#if 0
int parse_arguments(int argc, char **argv)
{
  enum
  {
    cNONE = 0,
    cFILENAME,
    cOUTNAME,
    cDISKPATH,
    cDISKNAME
  };

  struct optionset
  {
    char *set;     // Set of all valid flags in this option
    int   expect;  // Type of parameter we expect to get next
  };  

  // valid sets of flags for options
  struct optionset valid_set[] =
  {
    {"Vh",                  cNONE},
    {"oV",                  cOUTNAME},
    {"rV",                  cFILENAME},
    {"xV",                  cFILENAME},
    {"nV",                  cDISKNAME},
    {"cWUlV",               cDISKPATH},
    {"eWUlXV",              cDISKPATH},
    {"pdifwuvV0123456789",  cFILENAME},
    {"apdifwuvV0123456789", cFILENAME},
    {NULL,    cNONE}
  };

  int i;
  int expect = cNONE;
  struct file_arg curr_file;
  int last_file = -1;
  int last_arg = 0;

  // Display help if no arguments given
  if(argc == 1)
  {
    show_help();
    return(0);
  }

  for(i = 1; i < argc; i++)
  {
    char *arg = argv[i];
    if(arg[0] == '-')
    {
      // This is an option
      
      // Find the set to use
      struct optionset *set = valid_set;
      while(set->set != NULL)
      {
        // If all option flags are contained in this set, use it
        if(strspn(&arg[1], set->set) == strlen(arg)-1) break;

        // Check next set
        set++;
      }

      // Check for option validity
      if(set->set == NULL ||
         (last_arg != 0 && 
          expect != cNONE && 
          set->expect != cNONE && 
          set->expect != expect))
      {
        printf("Invalid option \"%s\"\n", arg);
        if(all_args.verbose)
        {
          if(set->set == NULL)      printf("This option does not exist\n");
          if(set->expect != expect) printf("This option can't be used with other options it's with\n");
        }
        return(0);
      }

      if(last_arg == 0)
      {
        memset(&curr_file, 0, sizeof(struct file_arg));
        last_arg = 1;
      }

      // Process flags
      char *op = &arg[1];
      while(*op)
      {
        switch(*op)
        {
          case 'a':  curr_file.add          = 1; break;
          case 'c':  all_args.create_new    = 1; break;
          case 'd':  curr_file.ascii        = 1; break;
          case 'e':  all_args.use_existing  = 1; break;
          case 'f':  curr_file.fixed        = 1; break;
          case 'h':  all_args.show_help     = 1; break;
          case 'i':  curr_file.binary       = 1; break;
          case 'l':  all_args.list_contents = 1; break;
          case 'n':  break;
          case 'o':  break;
          case 'p':  curr_file.program      = 1; break;
          case 'r':  curr_file.remove       = 1; break;
          case 'u':  curr_file.unprotect    = 1; break;
          case 'U':  all_args.unprotect     = 1; break;
          case 'v':  curr_file.variable     = 1; break;
          case 'V':  all_args.verbose++;         break;
          case 'W':  all_args.protect       = 1; break;
          case 'w':  curr_file.protect      = 1; break;
          case 'x':  curr_file.extract      = 1; break;
          case 'X':  all_args.extract_all   = 1; break;

          default:   printf("Unknown option \"%c\"\n", *op); return(0);
        }

        op++;
        // Process record length
        if((*(op-1) == 'f' || *(op-1) == 'v') && set->expect == cFILENAME)
        {
          if(*op < '0' || *op > '9' ||
             (curr_file.record_size = strtol(op, &op, 10)) == 0)
          {
            printf("Unknown option \"%s\"\n", (op-1)); return(0);
          }
          if(curr_file.record_size > 254)
          {
            printf("Invalid record length %d\n", curr_file.record_size);
            return(0);
          }

          struct file_arg *file = &all_args.file[all_args.file_count];
          file->ascii = curr_file.ascii;
          file->binary = curr_file.binary;
          file->fixed = curr_file.fixed;
          file->variable = curr_file.variable;
          file->record_size = curr_file.record_size;
        }
      }
      expect = set->expect;
    }
    else
    {
      // This is a name
      int i;
      if(expect == cNONE)
      {
        printf("Invalid argument \"%s\"\n", arg);
        return(0);
      }
      last_arg = 0;
      switch(expect)
      {
        case cFILENAME:
          for(i = 0; i < all_args.file_count; i++)
          {
            if(strcmp(all_args.file[i].file_name, arg) == 0) break;
          }          
          if(i >= MAX_FILE_COUNT)
          {
            printf("Disk full, cannot add \"%s\"\n", arg);
            return(0);
          }
          if(i == all_args.file_count)
            all_args.file_count++;
          if(curr_file.add != 0 || curr_file.extract != 0) last_file = i;
//          memcpy(&all_args.file[i], &curr_file, sizeof(struct file_arg));
          strcpy(all_args.file[i].file_name, arg);
          break;
          
        case cOUTNAME:
          if(last_file == -1)
          {
            printf("No file to use for output name \"%s\"\n", arg);
            return(0);
          }
          strcpy(all_args.file[i].output_name, arg);
          last_file = -1;
          break;

        case cDISKPATH:
          strcpy(all_args.image_path, arg);
          expect = cNONE;
          break;

        case cDISKNAME:
          strncpy(all_args.disk_name, arg, DISK_NAME_LEN);
          expect = cNONE;
          break;

        default:
          printf("Internal error, unknown name type %d for \"%s\"\n", 
                 expect, arg);
          return(0);
      }
    }
  }
  return(1);
}
#endif
int parse_arguments(int argc, char **argv)
{
  enum
  {
    cNONE = 0,
    cFILENAME,
    cOUTNAME,
    cDISKPATH,
    cDISKNAME
  };

  struct optionset
  {
    char *set;     // Set of all valid flags in this option
    int   expect;  // Type of parameter we expect to get next
  };  

  // valid sets of flags for options
  struct optionset valid_set[] =
  {
    {"Vh",                  cNONE},
    {"oV",                  cOUTNAME},
    {"rV",                  cFILENAME},
    {"xV",                  cFILENAME},
    {"nV",                  cDISKNAME},
    {"cWUlV",               cDISKPATH},
    {"eWUlXV",              cDISKPATH},
    {"pdifwuvV0123456789",  cFILENAME},
    {"apdifwuvV0123456789", cFILENAME},
    {NULL,    cNONE}
  };

  int i;
  int expect = cNONE;
  struct file_arg curr_file;
  int last_file = -1;

  // Display help if no arguments given
  if(argc == 1)
  {
    show_help();
    return(0);
  }

  memset(&curr_file, 0, sizeof(struct file_arg));
  for(i = 1; i < argc; i++)
  {
    char *arg = argv[i];
    if(arg[0] == '-')
    {
      // This is an option
      
      // Find the set to use
      struct optionset *set = valid_set;
      while(set->set != NULL)
      {
        // If all option flags are contained in this set, use it
        if(strspn(&arg[1], set->set) == strlen(arg)-1) break;

        // Check next set
        set++;
      }

      // Check for option validity
      if(expect != cFILENAME ||
         set->expect != cOUTNAME)
      {
        if((set->set == NULL)
           ||
           (expect != cNONE && 
            set->expect != cNONE && 
            set->expect != expect))
        {
          printf("Invalid option \"%s\"\n", arg);
          if(all_args.verbose)
          {
            if(set->set == NULL)      printf("This option does not exist\n");
            if(set->expect != expect) printf("This option can't be used with other options it's with\n");
          }
          return(0);
        }
      }

      // Process flags
      char *op = &arg[1];
      while(*op)
      {
        // Handle mutually exclusive options
        if(strchr("arx", *op))
        {
          curr_file.add       = 0;
          curr_file.remove    = 0;
          curr_file.extract   = 0;
        }
        if(strchr("di", *op))
        {
          curr_file.ascii     = 0;
          curr_file.binary    = 0;
        }
        if(strchr("fpv", *op))
        {
          curr_file.fixed     = 0;
          curr_file.program   = 0;
          curr_file.variable  = 0;
        }
        if(strchr("uw", *op))
        {
          curr_file.unprotect = 0;
          curr_file.protect   = 0;
        }

        switch(*op)
        {
          case 'a':  curr_file.add          = 1; break;
          case 'c':  all_args.create_new    = 1; break;
          case 'd':  curr_file.ascii        = 1; break;
          case 'e':  all_args.use_existing  = 1; break;
          case 'f':  curr_file.fixed        = 1; break;
          case 'h':  all_args.show_help     = 1; break;
          case 'i':  curr_file.binary       = 1; break;
          case 'l':  all_args.list_contents = 1; break;
          case 'n':  break;
          case 'o':  break;
          case 'p':  curr_file.program      = 1; break;
          case 'r':  curr_file.remove       = 1; break;
          case 'u':  curr_file.unprotect    = 1; break;
          case 'U':  all_args.unprotect     = 1; break;
          case 'v':  curr_file.variable     = 1; break;
          case 'V':  all_args.verbose++;         break;
          case 'W':  all_args.protect       = 1; break;
          case 'w':  curr_file.protect      = 1; break;
          case 'x':  curr_file.extract      = 1; break;
          case 'X':  all_args.extract_all   = 1; break;

          default:   printf("Unknown option \"%c\"\n", *op); return(0);
        }

        op++;
        // Process record length
        if(set->expect == cFILENAME)
        {
          if((*(op-1) == 'f' || *(op-1) == 'v') && set->expect == cFILENAME)
          {
            if(*op < '0' || *op > '9' ||
               (curr_file.record_size = strtol(op, &op, 10)) == 0)
            {
              printf("Unknown option \"%s\"\n", (op-1)); return(0);
            }
            if(curr_file.record_size > 254)
            {
              printf("Invalid record length %d\n", curr_file.record_size);
              return(0);
            }
          }
        }
      }
      expect = set->expect;
    }
    else
    {
      // This is a name
      int i;
      if(expect == cNONE)
      {
        printf("Invalid argument \"%s\"\n", arg);
        return(0);
      }

      switch(expect)
      {
        case cFILENAME:
          for(i = 0; i < all_args.file_count; i++)
          {
            if(strcmp(all_args.file[i].file_name, arg) == 0) break;
          }          
          if(i >= MAX_FILE_COUNT)
          {
            printf("Disk full, cannot add \"%s\"\n", arg);
            return(0);
          }
          if(i == all_args.file_count)
            all_args.file_count++;
          if(curr_file.add != 0 || curr_file.extract != 0) last_file = i;
          memcpy(&all_args.file[i], &curr_file, sizeof(struct file_arg));
          strcpy(all_args.file[i].file_name, arg);
          break;
          
        case cOUTNAME:
          if(last_file == -1)
          {
            printf("No file to use for output name \"%s\"\n", arg);
            return(0);
          }
          strcpy(all_args.file[i].output_name, arg);
          last_file = -1;
          expect = cNONE;
          memset(&curr_file, 0, sizeof(struct file_arg));
          break;

        case cDISKPATH:
          strcpy(all_args.image_path, arg);
          expect = cNONE;
          memset(&curr_file, 0, sizeof(struct file_arg));
          break;

        case cDISKNAME:
          strncpy(all_args.disk_name, arg, DISK_NAME_LEN);
          expect = cNONE;
          memset(&curr_file, 0, sizeof(struct file_arg));
          break;

        default:
          printf("Internal error, unknown name type %d for \"%s\"\n", 
                 expect, arg);
          return(0);
      }
    }
  }
  return(1);
}


/*===========================================================================
 *                              list_disk
 *===========================================================================
 * Desription: List the contents of the disk
 *
 * Parameters: None
 *
 * Return:     None
 */
void list_disk()
{
  int i;
  struct disk_sector *sector = disk_buffer;
  struct vib_block *vib = disk_buffer;

  // Dump header
  short physrecs = vib->physrecs;
  vib->physrecs = 0;  // Terminate name
  printf("Disk Name : %s\n",vib->name);
  printf("Disk Size : %d\n",swap(physrecs) * 256);
  printf("Protected?: %s\n",(vib->protection == 'P' ? "Yes": "No"));
  printf("Cylinders : %d\n",vib->cylinders);
  printf("Heads     : %d\n",vib->heads);
  printf("Density   : %s\n",(vib->density == 1 ? "FM SD":
                            (vib->density == 2 ? "MFM DD":
                            (vib->density == 3 ? "MFM HD": 
                                                 "Unknown"))));

  printf("\n");
  printf("Name        Type         WP  Size   Sectors\n");
  printf("----------  -----------  --  -----  ------\n");
  // List files
  for(i = 0; i < MAX_FILE_COUNT; i++)
  {
    short fib_idx = swap(sector[BLOCK_FIB_INDEX].data[i]);
    if(fib_idx != 0)
    {
      struct fib_block* fib = (struct fib_block*)(&sector[fib_idx]);
      int first = cluster_first(&fib->cluster[0][0]);
      int i;

      // File type
      fib->reserved[0] = 0;  // Terminate file name
      printf("%s  ", fib->name);
      if(fib->flags & fib_program)
      {
        printf("program      ");
      }
      else
      {
        if(fib->flags & fib_binary) printf("int/");
          else printf("dis/");
        if(fib->flags & fib_var) printf("var");
          else printf("fix");
        printf(" %-3d  ", fib->reclen);
      }
      
      // Write protect
      if(fib->flags & fib_wp) printf("wp  ");
        else                  printf("    ");
        
      // File size
      printf("%5d  ", (swap(fib->physrec_count)-1) * 256 + fib->eof);

      // Sector usage
      for(i=0; i<76; i++)
      {
        int first = cluster_first(&fib->cluster[i][0]);
        int count = cluster_count(&fib->cluster[i][0]);

        if(count == 0) break;
        if(count > 2)
        {
          printf("%d-%d  ", first, first + count);
        }
        else
        {
          printf("%d", first);
        }
      }
      printf("\n");
    }
  }
}


/*
0: Program/data file indicator 0 = Data file 1 = Program file

1: Binary/ASCII data 0 = ASCII data (DISPLAY file) 1 = Binary data (INTERNAL 
or program file)

2: Reserved for future data type expansion

3: Protect flag 0 = Not protected 1 = Protected

4-6: Reserved for future expansion

7: FIXED/VARIABLE flag 0 = Fixed length records 1 = Variable length records

Byte 13 contains the number of logical records per AU.

Bytes 14-15 contain the number of logical records allocated on Level 2
(256 byte records}.

Byte 16 contains the EOF offset within the highest physical AU for variable
length record files and program files.

Byte 17 contains the logical record size in bytes. In case of variable length
records, this entry will indicate the maximum allowable record size.

Bytes 18-19 contain the number of records allocated on Level 3, For variable
length records, this entry is replaced with the number of Level 2 records
actually used. (NOTE: The bytes in this entry are in reverse order.)

Bytes 20-27 have been reserved for future expansion. They will be fixed to 0
in this implementation of disk peripheral software.

Bytes 23-255 contain three byte blocks indicating the clusters that have been
allocated for the file. The first 12 bits in each entry indicate the address of
the first AU in the cluster . The second 12 bits indicate the highest logical
record offset in the cluster of contiguous records. This indication has been 
chosen, rather than the number of data-records in the chain, since it reduces
the amount of computation required for relative record file access.
*/


/*===========================================================================
 *                             mark_sector
 *===========================================================================
 * Desription: Set sector usage in the allocation bitmap
 *
 * Parameters: sector - Sector number to mark
 *             used   - Is this sector used?
 *
 * Return:     None
 */
void mark_sector(int sector, int used)
{
  struct vib_block *vib = disk_buffer;
  if(used)
  {
    vib->abm[sector/8] |= 1 <<(sector%8);
  }
  else
  {
    vib->abm[sector/8] &= ~(1 <<(sector%8));
  }
}


/*===========================================================================
 *                             create_disk
 *===========================================================================
 * Desription: Create a new disk image in memory
 *
 * Parameters: None
 *
 * Return:     None
 */
void create_disk()
{
  int i;
  struct vib_block *vib;
  disk_size = 92160;
  disk_buffer = malloc(disk_size);
  memset(disk_buffer, 0, disk_size);
  
  // Format disk as SSSD image
  vib = (struct vib_block*)disk_buffer;
  make_name(vib->name, "", DISK_NAME_LEN);
  vib->physrecs = swap(360);
  vib->secspertrack = 9;
  strcpy(vib->id, "DSK");
  vib->cylinders    = 40;
  vib->heads        = 1;
  vib->density      = 1;

  // Set allocation bitmap
  memset(&vib->abm[0], 0xFF, sizeof(vib->abm));
  for(i = 2; i < disk_size / sizeof(struct disk_sector); i++)
  {
    mark_sector(i, 0);
  }
}


/*===========================================================================
 *                             save_disk
 *===========================================================================
 * Desription: Save the disk image in memory to a file
 *
 * Parameters: filename - File used to store disk image
 *
 * Return:     Was disk image stored correctly?
 */
int save_disk(char *filename)
{
  FILE *file;
  file = fopen(filename, "wb");
  if(file == NULL)
  {
    printf("Cannot dave disk file \"%s\"\n", filename);
    return(0);
  }
  fwrite(disk_buffer, disk_size, 1, file);
  fclose(file);

  return(1);
}


/*===========================================================================
 *                             load_disk
 *===========================================================================
 * Desription: Load disk image to memory
 *
 * Parameters: filename - File from which to read disk image
 *
 * Return:     Was disk image loaded correctly?
 */
int load_disk(char *filename)
{
  // Read disk image
  FILE *file = fopen(filename, "rb");      
  if(file == NULL)
  {
    printf("Cannot open disk image \"%s\"\n", all_args.image_path);
    return(0);
  }

  // Load image into memory
  fseek(file, 0, SEEK_END);
  disk_size = ftell(file);
  fseek(file, 0, SEEK_SET);
  disk_buffer = malloc(disk_size);
  fread(disk_buffer, disk_size, 1, file);
 
  // Confirm this is a valid image
  struct vib_block *vib = (struct vib_block*)disk_buffer;
  if(vib->id[0] != 'D' ||
     vib->id[1] != 'S' ||
     vib->id[2] != 'K')
  {
    printf("%s is not a V9T9 disk image\n", all_args.image_path);
    return(0);
  }

  // File succcessfully loaded
  return(1);
}


/*===========================================================================
 *                            extract_file
 *===========================================================================
 * Desription: Copy a file from the disk image to a seperate file
 *
 * Parameters: fib      - File information block for the file to be extracted
 *             filename - File name to use for extracted file
 *
 * Return:     None
 */
void extract_file(struct fib_block *fib, char *filename)
{
  int i;
  struct disk_sector *sector = disk_buffer;
  FILE *file;
  int file_size;
  char name_buffer[FILE_NAME_LEN + 1];

  if(fib == NULL) return;

  // Make name for the extracted file
  if(filename == NULL)
  {
    // Remove trailng spaces from name on disk
    char *p;
    strncpy(name_buffer, fib->name, FILE_NAME_LEN);
    if((p = strchr(name_buffer, ' ')) != NULL) *p = 0;
    if((p = strchr(name_buffer, '/')) != NULL) *p = '_';
    filename = name_buffer;
  }

  // Open extraction destination
  file = fopen(filename, "wb");
  if(file == NULL)
  {
    printf("Cannot open file \"%s\"\n", filename);
  }

  // Copy file contents to destination
  file_size = (swap(fib->physrec_count)-1) * 256 + fib->eof;
  for(i=0; i<76; i++)
  {
    int first = cluster_first(&fib->cluster[i][0]);
    int count = cluster_count(&fib->cluster[i][0]);
    int j;

    if(count == 0) break;
    for(j=0; j<=count; j++)
    {
      int size = 256;
      if(size > file_size) size = file_size;
      fwrite(&sector[first + j], size, 1, file);
      file_size -= size;
    }    
  }
  fclose(file);
  
  if(all_args.verbose)
    printf("Extracted disk file \"%s\" to \"%s\"\n",
           fib->name, filename);
}


/*===========================================================================
 *                               find_fib
 *===========================================================================
 * Desription: Find the FIB for a file in the disk image
 *
 * Parameters: filename - File name to search for in V9T9 format
 *
 * Return:     Pointer to the found FIB, NULL if not found
 */
struct fib_block* find_fib(char *filename)
{
  int i;
  struct disk_sector *sector = disk_buffer;

  // Iterate through files
  for(i = 0; i < MAX_FILE_COUNT; i++)
  {
    short fib_idx = swap(sector[BLOCK_FIB_INDEX].data[i]);
    if(fib_idx != 0)
    {
      // Try to match this name
      struct fib_block* fib = (struct fib_block*)(&sector[fib_idx]);
      if(strncmp(filename, fib->name, FILE_NAME_LEN) == 0)
        return(fib);
    }
  }
  return(NULL);
}


/*===========================================================================
 *                             remove_file
 *===========================================================================
 * Desription: Remove a file from the disk image
 *
 * Parameters: filename - File name to remove in V9T9 format
 *
 * Return:     Was file removed?
 */
int remove_file(char *filename)
{
  struct fib_block *fib;
  int i;
  int secno;
  struct disk_sector *sector = disk_buffer;

  fib = find_fib(filename);
  if(fib == NULL)
  {
    printf("Cannot remove file \"%s\"\n", filename);
    return(0);
  }

  // Free all sectors used by this file
  for(i=0; i<76; i++)
  {
    int first = cluster_first(&fib->cluster[i][0]);
    int count = cluster_count(&fib->cluster[i][0]);
    if(count == 0) break;
    for(secno = first; secno<=first+count; secno++)
    {
      memset(&sector[secno], 0, sizeof(struct disk_sector));
      mark_sector(secno, 0);
    }    
  }

  // Free the FIB block too
  memset(fib, 0, sizeof(struct disk_sector));
  secno = ((intptr_t)fib - (intptr_t)disk_buffer) / sizeof(struct disk_sector);
  mark_sector(secno, 0);

  // Remove this entry in the file list
  for(i = 0; i < MAX_FILE_COUNT; i++)
  {
    short fib_idx = swap(sector[BLOCK_FIB_INDEX].data[i]);
    if(fib_idx == secno) break;
  }
  for(i = i; i < MAX_FILE_COUNT; i++)
  {
    sector[BLOCK_FIB_INDEX].data[i] = sector[BLOCK_FIB_INDEX].data[i + 1];
  }

  if(all_args.verbose)
    printf("Removing file \"%s\" from disk image\n", filename);
  return(1);
}


/*===========================================================================
 *                              extract_all
 *===========================================================================
 * Desription: Extract all files from the disk image
 *
 * Parameters: None
 *
 * Return:     None
 */
void extract_all()
{
  int i;
  struct disk_sector *sector = disk_buffer;

  // Iterate over all files
  for(i = 0; i < MAX_FILE_COUNT; i++)
  {
    short fib_idx = swap(sector[BLOCK_FIB_INDEX].data[i]);
    if(fib_idx != 0)
    {
      struct fib_block* fib = (struct fib_block*)(&sector[fib_idx]);
      extract_file(fib, NULL);
    }
  }
}


/*===========================================================================
 *                                allocate
 *===========================================================================
 * Desription: Allocate a free sector from the disk
 *
 * Parameters: None
 *
 * Return:     Pointer to new sector, NULL if none available
 */
struct disk_sector* allocate()
{
  int i;
  struct vib_block *vib = (struct vib_block*)disk_buffer;
  struct disk_sector *sector = (struct disk_sector*)disk_buffer;
  for(i = 2; i < disk_size / sizeof(struct disk_sector); i++)
  {
    if((vib->abm[i/8] & (1 << (i % 8))) == 0)
    {
      mark_sector(i, 1);
      return(&sector[i]);
    }
  }
  return(NULL);
}


/*===========================================================================
 *                             free_sector_count
 *===========================================================================
 * Desription: Determine the number of free sectors on this disk
 *
 * Parameters: None
 *
 * Return:     Number of free sectors
 */
int free_sector_count()
{
  int i;
  int count = 0;
  struct vib_block *vib = (struct vib_block*)disk_buffer;
  struct disk_sector *sector = (struct disk_sector*)disk_buffer;
  for(i = 2; i < disk_size / sizeof(struct disk_sector); i++)
  {
    if((vib->abm[i/8] & (1 << (i % 8))) == 0)
    {
      count++;
    }
  }
  return(count);
}


/*===========================================================================
 *                                add_file
 *===========================================================================
 * Desription: Add a file to the disk image
 *
 * Parameters: filename - File name to add
 *             diskname - Name used for the file in V9T9 format
 *
 * Return:     Was file removed?
 */
int add_file(char *filename, char *diskname)
{
  struct fib_block *fib;
  int first = 0;
  int count = 0;
  FILE *file;
  int file_size;
  char *cluster;
  int i;
  int j;
  int sector_size = sizeof(struct disk_sector);
  struct disk_sector *sector = (struct disk_sector*)disk_buffer;

  if(all_args.verbose)
    printf("Attempting to add \"%s\" as \"%s\"\n", filename, diskname);

  // Search for existing file with matching name
  if(find_fib(diskname) != NULL)
  {
    printf("Cannot add \"%s\" as \"%s\", file already exists\n",
           filename, diskname);
    return(0);
  }

  // Make sure the Source file exists
  file = fopen(filename, "rb");
  if(file == NULL)
  {
    printf("Cannot add \"%s\", file does not exist\n", filename);
    return(0);
  }

  // Find file size
  fseek(file, 0, SEEK_END);
  file_size = ftell(file);
  fseek(file, 0, SEEK_SET);

  // Make sure we can fit this file (file data + FIB)
  if(free_sector_count() < (file_size/sector_size)+1)
  {
    printf("Cannot add \"%s\", disk full\n", filename);
    fclose(file);
    return(0);
  }  	  
  
  // Create FIB for this file
  fib = (struct fib_block*)allocate();
  if(fib == NULL)
  {
    printf("Cannot add \"%s\", disk full\n", filename);
    fclose(file);
    return(0);
  }
  memset(fib, 0, sector_size);
  fib->flags = fib_program;
  fib->physrec_count = swap((file_size / sector_size) + 1);
  fib->eof = file_size % sector_size;
  strncpy(fib->name, diskname, FILE_NAME_LEN);

  // Save file contents
  cluster = &fib->cluster[0][0]; 
  while(file_size > 0)
  {
    int secnum;
    struct disk_sector *sector = allocate();
    if(sector == NULL)
    {
      printf("Cannot add \"%s\", disk is full\n", filename);
      fclose(file);
      return(0);
    }
    fread(sector, sector_size, 1, file);

    // Sectors in a cluster must be contiguous, check that here
    secnum = ((intptr_t)sector - (intptr_t)disk_buffer) / sector_size;
    if(secnum != first + count)
    {
      if(first != 0)
      {
        // No longer contiguous, make a new cluster for this sector
        make_cluster(cluster, first, count);
        cluster += 3;
      }
      first = secnum;
      count = 0;
    }
    count++;
    file_size -= sector_size;
  }
  make_cluster(cluster, first, count);

  // Add file to listing
  for(i = 0; i < MAX_FILE_COUNT; i++)
  {
    short fib_idx = swap(sector[BLOCK_FIB_INDEX].data[i]);
    if(fib_idx == 0 ||
       strncmp(((struct fib_block*)&sector[fib_idx])->name,
               fib->name, FILE_NAME_LEN) > 0)
      break;
  }
  for(j = MAX_FILE_COUNT - 1; j > i ; j--)
  {
    sector[BLOCK_FIB_INDEX].data[j] = sector[BLOCK_FIB_INDEX].data[j-1];
  }
  sector[BLOCK_FIB_INDEX].data[i] = 
    swap(((intptr_t)fib - (intptr_t)disk_buffer) / sector_size);

  fclose(file);
  return(1);
}


/*===========================================================================
 *                                  main
 *===========================================================================
 * Desription: Entry point for the program
 *
 * Parameters: argc - Number of command arguments
 *             argv - Argument list
 *
 * Return:     None
 */
int main(int argc, char **argv)
{
  int i;
  int modified = 0;
  struct vib_block* vib;

  memset(&all_args, 0, sizeof(all_args));
  if(parse_arguments(argc, argv) == 0) return(1);
  if(all_args.verbose > 1)
  {
    printf("\n");
    printf("disk path     =%s\n", all_args.image_path);
    printf("disk name     =%s\n", all_args.disk_name);
    printf("disk protect  =%d\n", all_args.protect);
    printf("disk unprotect=%d\n", all_args.unprotect);
    printf("create disk   =%d\n", all_args.create_new);
    printf("use existing  =%d\n", all_args.use_existing);
    printf("list disk     =%d\n", all_args.list_contents);
    printf("extract all   =%d\n", all_args.extract_all);
    printf("verbose       =%d\n", all_args.verbose);
    printf("show help     =%d\n", all_args.show_help);

    for(i = 0; i < all_args.file_count; i++)
    {
      printf("\n");
      printf("file %d\n", i);
      printf("filename = %s\n", all_args.file[i].file_name);
      printf("outname  = %s\n", all_args.file[i].output_name);
      printf("add      = %d\n", all_args.file[i].add);
      printf("remove   = %d\n", all_args.file[i].remove);
      printf("extract  = %d\n", all_args.file[i].extract);
      printf("program  = %d\n", all_args.file[i].program);
      printf("protect  = %d\n", all_args.file[i].protect);
      printf("ascii    = %d\n", all_args.file[i].ascii);
      printf("binary   = %d\n", all_args.file[i].binary);
      printf("fixed    = %d\n", all_args.file[i].fixed);
      printf("variable = %d\n", all_args.file[i].variable);
      printf("rec_size = %d\n", all_args.file[i].record_size);
    }
    printf("\n");
  }

  if(all_args.show_help)
  {
    show_help();
    return(0);
  }

  // Get disk image
  if(all_args.create_new)
  {
    create_disk();
    if(all_args.verbose)
      printf("Creating new disk image \"%s\"\n",all_args.image_path);
    modified = 1;
  }
  else
  {
    if(load_disk(all_args.image_path) == 0)
      return(0);
    if(all_args.verbose)
      printf("Using disk image \"%s\"\n",all_args.image_path);
  }
  vib = (struct vib_block*)disk_buffer;

  // Extract all files
  if(all_args.extract_all)
    extract_all();
  
  // Set disk name
  if(all_args.disk_name[0] != 0)
  {
    make_name(vib->name, all_args.disk_name, DISK_NAME_LEN);   
    modified = 1;
    if(all_args.verbose)
    {
      char name[DISK_NAME_LEN+1];
      name[DISK_NAME_LEN] = 0;
      strncpy(name, vib->name, DISK_NAME_LEN);
      printf("Setting new disk name \"%s\"\n", name);
    }
  }

  // Set disk protection
  if(all_args.protect)
  {
    vib->protection = 'P';
    modified = 1;
    if(all_args.verbose) printf("Setting disk protection\n");
  }
  if(all_args.unprotect)
  {
    vib->protection = 0;
    modified = 1;
    if(all_args.verbose) printf("Clearing disk protection\n");
  }

  // Act on individual files
  for(i = 0; i < all_args.file_count; i++)
  {
    struct fib_block *fib;
    char name[FILE_NAME_LEN + 1];
    name[FILE_NAME_LEN] = 0;

    if(all_args.file[i].output_name[0] == 0)
      strcpy(all_args.file[i].output_name, all_args.file[i].file_name);

    // Make disk name    
    make_name(name, all_args.file[i].file_name, FILE_NAME_LEN);

    // Extract file from disk
    if(all_args.file[i].extract )
    {
      fib = find_fib(name);
      if(fib == NULL)
        printf("Cannot find file \"%s\"\n", all_args.file[i].file_name);
      else
        extract_file(fib, all_args.file[i].output_name);
    }

    // Remove file from disk
    if(all_args.file[i].remove)
      modified = remove_file(name);

    // Add file to disk
    if(all_args.file[i].add)
    {
      make_name(name, all_args.file[i].output_name, FILE_NAME_LEN);
      modified = add_file(all_args.file[i].file_name, name);
    }

    // Set file attributes
    if(all_args.file[i].protect  || all_args.file[i].unprotect ||
       all_args.file[i].binary   || all_args.file[i].ascii     ||
       all_args.file[i].variable || all_args.file[i].fixed     ||
       all_args.file[i].program  || all_args.file[i].add)   
    {
      fib = find_fib(name);
      if(fib == NULL)
      {
        printf("Cannot find file \"%s\"\n", all_args.file[i].file_name);
      }
      else
      {
        if(all_args.file[i].protect)   fib->flags |=  fib_wp;
        if(all_args.file[i].unprotect) fib->flags &= ~fib_wp;
        if(all_args.file[i].binary)    fib->flags |=  fib_binary;
        if(all_args.file[i].ascii)     fib->flags &= ~fib_binary;
        if(all_args.file[i].variable)  fib->flags |=  fib_var;
        if(all_args.file[i].fixed)     fib->flags &= ~fib_var;
        if(all_args.file[i].program)   
        {
          fib->flags |= fib_program;
          fib->flags &= ~(fib_binary | fib_var);
          fib->reclen = 0;
        }
        if(all_args.file[i].binary   || all_args.file[i].ascii ||
           all_args.file[i].variable || all_args.file[i].fixed)
        {
          fib->flags &= ~fib_program;

          int j;
          int sector_count = 0;
          int sector_max = 0;
          for(j=0; j<76; j++)
          {
            int first = cluster_first(&fib->cluster[j][0]);
            int count = cluster_count(&fib->cluster[j][0]);
            if(count == 0) break;

            sector_count += count;
            sector_max = first + count;
          }

          if(all_args.file[i].variable)
          {
            fib->reclen = 254;
            fib->recsperphysrec = 254 / fib->reclen;
            fib->fixrecs = sector_max;
          }
          else if(all_args.file[i].fixed)
          {
            fib->reclen = all_args.file[i].record_size;
            fib->recsperphysrec = SECTOR_SIZE / fib->reclen;
            fib->fixrecs = (sector_count * SECTOR_SIZE) / fib->reclen;
          }
        }
        if(all_args.verbose)
        {
          printf("Setting file \"%s\" as ", all_args.file[i].file_name);
          if(fib->flags & fib_program)
            printf("program\n");
          else
            printf("%s/%s %d\n", 
                   (fib->flags & fib_binary) ? "internal" : "display",
                   (fib->flags & fib_var)    ? "variable" : "fixed",
                   fib->reclen);
        }
        modified = 1;
      }
    }
  }

  // Save the modified disk image
  if(modified)
  {
    save_disk(all_args.image_path); 
    if(all_args.verbose)
      printf("Saving modified disk image as \"%s\"\n", all_args.image_path);
  }

  // List disk contents
  if(all_args.list_contents)
    list_disk(); 

  return(0);
}
