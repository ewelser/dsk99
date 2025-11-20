dsk99 - TI 99/4A Floppy Disk Management Tool

Usage:

dsk99 {options} {disk image} [-n {disk name}]
      [{options} {filename} ... [-o {output name}]]

Disk Options
  -c : Create new disk image
  -e : Use existing disk image
  -U : Clear protect flag
  -W : Set protect flag
  -n : Set disk name
  -l : List disk contents
  -X : Extract all files

File Options
  -p : File is a program
  -d : File contains ASCII data
  -i : File contains binary data
  -u : File is not write-protected
  -w : File is write-protected
  -f{record size} : File contains fixed records of indicated size
  -v{record size} : File contains variable records of maximum indicated size
  -a : Add file to image
  -r : Remove file from image
  -x : Extract file from image
  -o : Specify output name

Global Options
  -V : Verbose output

Examples
  List the contents of a disk image
    dsk99 -l disk.v9t9

  Extract all files from a disk image
    dsk99 -X disk.v9t9

  Add all files in the current directory to a new disk image as programs
    dsk99 -c disk.v9t9 -ap *

  Add a local file "records1.dat" as a "dis/fix 80" file named "fixrec"
    dsk99 -c disk.v9t9 -adf80 records1.dat -o fixrec

  Change the existing file "fixrec" filetype to "dis/fix 40"
    dsk99 -e disk.v9t9 -df40 records1.dat -o fixrec

  Extract a disk image file named "fixrec" to a local file named "records1.dat"
    dsk99 -e disk.v9t9 -x fixrec -o records1.dat

