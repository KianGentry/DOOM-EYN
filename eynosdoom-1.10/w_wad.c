// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
// $Log:$
//
// DESCRIPTION:
//	Handles WAD file header, directory, lump I/O.
//
//-----------------------------------------------------------------------------


static const char
rcsid[] = "$Id: w_wad.c,v 1.5 1997/02/03 16:47:57 b1 Exp $";


#ifdef NORMALUNIX
#include <ctype.h>
#include <sys/types.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <fcntl.h>
#include <sys/stat.h>
#define O_BINARY		0
#endif

#include "doomtype.h"
#include "m_swap.h"
#include "i_system.h"
#include "z_zone.h"

#ifdef __GNUG__
#pragma implementation "w_wad.h"
#endif
#include "w_wad.h"






//
// GLOBALS
//

// Location of each lump on disk.
lumpinfo_t*		lumpinfo;		
int			numlumps;

void**			lumpcache;

static int		lumpinfo_capacity;

#define LUMPHASH_SIZE 1024
static int		lumphash[LUMPHASH_SIZE];
static int*		lumphash_next;

static int              wadperf_enabled;
static unsigned long    wadperf_reads;
static unsigned long    wadperf_read_bytes;
static unsigned long    wadperf_cache_hits;
static unsigned long    wadperf_cache_misses;
static unsigned long*   wadperf_lump_misses;
static int              wadread_last_handle = -2;
static int              wadread_last_pos;


#define strcmpi	strcasecmp

static unsigned W_LumpNameHash (int v1, int v2)
{
    return (((unsigned)v1 * 1315423911u) ^ (unsigned)v2) & (LUMPHASH_SIZE-1);
}

static int W_ReadFully (int handle, void* dest, int size)
{
    int total;
    int got;
    byte* out;

    total = 0;
    out = (byte*)dest;

    while (size > 0)
    {
        got = read (handle, out, size);
        if (got <= 0)
            break;
        out += got;
        total += got;
        size -= got;
    }

    return total;
}

void W_SetPerfMode (int enabled)
{
    wadperf_enabled = enabled ? 1 : 0;
}

static void W_ResetPerfStats (void)
{
    wadperf_reads = 0;
    wadperf_read_bytes = 0;
    wadperf_cache_hits = 0;
    wadperf_cache_misses = 0;

    if (wadperf_lump_misses && numlumps > 0)
        memset (wadperf_lump_misses, 0, numlumps * sizeof(*wadperf_lump_misses));
}

void W_ReportPerfStats (void)
{
    unsigned long total_cache;
    int i;
    int slot;
    int top_idx[5];
    unsigned long top_val[5];

    if (!wadperf_enabled)
        return;

    total_cache = wadperf_cache_hits + wadperf_cache_misses;

    printf ("PERF WAD reads=%lu bytes=%lu cache hits=%lu misses=%lu",
            wadperf_reads,
            wadperf_read_bytes,
            wadperf_cache_hits,
            wadperf_cache_misses);

    if (total_cache)
    {
        unsigned long hit_permille;
        hit_permille = (wadperf_cache_hits * 1000UL) / total_cache;
        printf (" hitrate=%lu.%01lu%%", hit_permille/10, hit_permille%10);
    }
    printf ("\n");

    if (!wadperf_lump_misses)
        return;

    for (slot=0 ; slot<5 ; slot++)
    {
        top_idx[slot] = -1;
        top_val[slot] = 0;
    }

    for (i=0 ; i<numlumps ; i++)
    {
        unsigned long v;
        int j;

        v = wadperf_lump_misses[i];
        if (!v)
            continue;

        for (j=0 ; j<5 ; j++)
        {
            if (v > top_val[j])
            {
                int k;
                for (k=4 ; k>j ; k--)
                {
                    top_val[k] = top_val[k-1];
                    top_idx[k] = top_idx[k-1];
                }
                top_val[j] = v;
                top_idx[j] = i;
                break;
            }
        }
    }

    for (slot=0 ; slot<5 ; slot++)
    {
        char name[9];
        if (top_idx[slot] < 0)
            continue;
        memcpy (name, lumpinfo[top_idx[slot]].name, 8);
        name[8] = 0;
        printf ("PERF WAD hotmiss #%d lump=%d name=%s misses=%lu size=%d\n",
                slot+1,
                top_idx[slot],
                name,
                top_val[slot],
                lumpinfo[top_idx[slot]].size);
    }
}

static int W_IsWadFilename (char* filename)
{
    int len;

    len = strlen(filename);
    if (len < 3)
        return 0;

    return !strcmpi (filename+len-3, "wad");
}

static void W_EnsureLumpInfoCapacity (int needed)
{
    int newcap;

    if (needed <= lumpinfo_capacity)
        return;

    newcap = lumpinfo_capacity ? lumpinfo_capacity : 1;
    while (newcap < needed)
        newcap <<= 1;

    lumpinfo = realloc (lumpinfo, newcap * sizeof(*lumpinfo));
    if (!lumpinfo)
        I_Error ("Couldn't realloc lumpinfo");

    lumpinfo_capacity = newcap;
}

static int W_CountFileLumps (char* filename)
{
    wadinfo_t header;
    int handle;

    if (filename[0] == '~')
        filename++;

    if ( (handle = open (filename,O_RDONLY | O_BINARY)) == -1)
        return 0;

    if (!W_IsWadFilename (filename))
    {
        close (handle);
        return 1;
    }

    if (W_ReadFully (handle, &header, sizeof(header)) != sizeof(header))
    {
        close (handle);
        return 0;
    }

    close (handle);
    return LONG(header.numlumps);
}

static void W_BuildLumpHash (void)
{
    int i;
    int v1;
    int v2;
    int hash;

    for (i=0 ; i<LUMPHASH_SIZE ; i++)
        lumphash[i] = -1;

    if (lumphash_next)
    {
        free (lumphash_next);
        lumphash_next = NULL;
    }

    lumphash_next = malloc (numlumps * sizeof(*lumphash_next));
    if (!lumphash_next)
        I_Error ("Couldn't allocate lumphash links");

    for (i=0 ; i<numlumps ; i++)
        lumphash_next[i] = -1;

    for (i=0 ; i<numlumps ; i++)
    {
        v1 = *(int *)lumpinfo[i].name;
        v2 = *(int *)&lumpinfo[i].name[4];
        hash = W_LumpNameHash (v1, v2);
        lumphash_next[i] = lumphash[hash];
        lumphash[hash] = i;
    }
}

void strupr (char* s)
{
    while (*s) { *s = toupper(*s); s++; }
}

int filelength (int handle) 
{ 
    struct stat	fileinfo;
    
    if (fstat (handle,&fileinfo) == -1)
	I_Error ("Error fstating");

    return fileinfo.st_size;
}


void
ExtractFileBase
( char*		path,
  char*		dest )
{
    char*	src;
    int		length;

    src = path + strlen(path) - 1;
    
    // back up until a \ or the start
    while (src != path
	   && *(src-1) != '\\'
	   && *(src-1) != '/')
    {
	src--;
    }
    
    // copy up to eight characters
    memset (dest,0,8);
    length = 0;
    
    while (*src && *src != '.')
    {
	if (++length == 9)
	    I_Error ("Filename base of %s >8 chars",path);

	*dest++ = toupper((int)*src++);
    }
}





//
// LUMP BASED ROUTINES.
//

//
// W_AddFile
// All files are optional, but at least one file must be
//  found (PWAD, if all required lumps are present).
// Files with a .wad extension are wadlink files
//  with multiple lumps.
// Other files are single lumps with the base filename
//  for the lump name.
//
// If filename starts with a tilde, the file is handled
//  specially to allow map reloads.
// But: the reload feature is a fragile hack...

int			reloadlump;
char*			reloadname;


void W_AddFile (char *filename)
{
    wadinfo_t		header;
    lumpinfo_t*		lump_p;
    unsigned		i;
    int			handle;
    int			length;
    int			startlump;
    filelump_t*		fileinfo;
    filelump_t*		fileinfo_alloc;
    filelump_t		singleinfo;
    int			storehandle;
    int                     addcount;
    
    // open the file and add to directory

    // handle reload indicator.
    if (filename[0] == '~')
    {
	filename++;
	reloadname = filename;
	reloadlump = numlumps;
    }
		
    if ( (handle = open (filename,O_RDONLY | O_BINARY)) == -1)
    {
	printf (" couldn't open %s\n",filename);
	return;
    }

    printf (" adding %s\n",filename);
    startlump = numlumps;
    fileinfo_alloc = NULL;

	
    if (!W_IsWadFilename (filename))
    {
	// single lump file
	fileinfo = &singleinfo;
	singleinfo.filepos = 0;
	singleinfo.size = LONG(filelength(handle));
	ExtractFileBase (filename, singleinfo.name);
    addcount = 1;
    }
    else 
    {
	// WAD file
    if (W_ReadFully (handle, &header, sizeof(header)) != sizeof(header))
    {
        I_Error ("W_AddFile: couldn't read header for %s", filename);
    }

	if (strncmp(header.identification,"IWAD",4))
	{
	    // Homebrew levels?
	    if (strncmp(header.identification,"PWAD",4))
	    {
		I_Error ("Wad file %s doesn't have IWAD "
			 "or PWAD id\n", filename);
	    }
	    
	    // ???modifiedgame = true;		
	}
	header.numlumps = LONG(header.numlumps);
	header.infotableofs = LONG(header.infotableofs);
	length = header.numlumps*sizeof(filelump_t);
    fileinfo = fileinfo_alloc = malloc (length);
    if (!fileinfo)
    {
        I_Error ("W_AddFile: couldn't allocate directory for %s", filename);
    }

	lseek (handle, header.infotableofs, SEEK_SET);
    if (W_ReadFully (handle, fileinfo, length) != length)
    {
        I_Error ("W_AddFile: couldn't read directory for %s", filename);
    }
    addcount = header.numlumps;
    }

    numlumps += addcount;
    W_EnsureLumpInfoCapacity (numlumps);

    lump_p = &lumpinfo[startlump];
	
    storehandle = reloadname ? -1 : handle;
	
    for (i=startlump ; i<numlumps ; i++,lump_p++, fileinfo++)
    {
	lump_p->handle = storehandle;
	lump_p->position = LONG(fileinfo->filepos);
	lump_p->size = LONG(fileinfo->size);
	strncpy (lump_p->name, fileinfo->name, 8);
    }

    if (fileinfo_alloc)
	free (fileinfo_alloc);
	
    if (reloadname)
	close (handle);
}




//
// W_Reload
// Flushes any of the reloadable lumps in memory
//  and reloads the directory.
//
void W_Reload (void)
{
    wadinfo_t		header;
    int			lumpcount;
    lumpinfo_t*		lump_p;
    unsigned		i;
    int			handle;
    int			length;
    filelump_t*		fileinfo;
    filelump_t*		fileinfo_base;

	
    if (!reloadname)
	return;
		
    if ( (handle = open (reloadname,O_RDONLY | O_BINARY)) == -1)
	I_Error ("W_Reload: couldn't open %s",reloadname);

    if (W_ReadFully (handle, &header, sizeof(header)) != sizeof(header))
    I_Error ("W_Reload: couldn't read %s", reloadname);

    lumpcount = LONG(header.numlumps);
    header.infotableofs = LONG(header.infotableofs);
    length = lumpcount*sizeof(filelump_t);
    fileinfo = malloc (length);
    if (!fileinfo)
	I_Error ("W_Reload: couldn't allocate directory for %s", reloadname);
    fileinfo_base = fileinfo;

    lseek (handle, header.infotableofs, SEEK_SET);
    if (W_ReadFully (handle, fileinfo, length) != length)
	I_Error ("W_Reload: couldn't read directory for %s", reloadname);
    
    // Fill in lumpinfo
    lump_p = &lumpinfo[reloadlump];
	
    for (i=reloadlump ;
	 i<reloadlump+lumpcount ;
	 i++,lump_p++, fileinfo++)
    {
	if (lumpcache[i])
	    Z_Free (lumpcache[i]);

	lump_p->position = LONG(fileinfo->filepos);
	lump_p->size = LONG(fileinfo->size);
    }

    free (fileinfo_base);
	
    close (handle);
}



//
// W_InitMultipleFiles
// Pass a null terminated list of files to use.
// All files are optional, but at least one file
//  must be found.
// Files with a .wad extension are idlink files
//  with multiple lumps.
// Other files are single lumps with the base filename
//  for the lump name.
// Lump names can appear multiple times.
// The name searcher looks backwards, so a later file
//  does override all earlier ones.
//
void W_InitMultipleFiles (char** filenames)
{	
    int		size;
    int         estimate;
    char**      fn;
    
    // open all the files, load headers, and count lumps
    numlumps = 0;

    estimate = 0;
    for (fn = filenames ; *fn ; fn++)
	estimate += W_CountFileLumps (*fn);

    if (estimate < 1)
	estimate = 1;

    lumpinfo_capacity = estimate;
    lumpinfo = malloc (lumpinfo_capacity * sizeof(*lumpinfo));
    if (!lumpinfo)
	I_Error ("Couldn't allocate lumpinfo");

    for ( ; *filenames ; filenames++)
	W_AddFile (*filenames);

    if (!numlumps)
	I_Error ("W_InitFiles: no files found");
    
    // set up caching
    size = numlumps * sizeof(*lumpcache);
    lumpcache = malloc (size);
    
    if (!lumpcache)
	I_Error ("Couldn't allocate lumpcache");

    memset (lumpcache,0, size);

    if (wadperf_lump_misses)
    {
        free (wadperf_lump_misses);
        wadperf_lump_misses = NULL;
    }

    wadperf_lump_misses = malloc (numlumps * sizeof(*wadperf_lump_misses));
    if (wadperf_lump_misses)
        memset (wadperf_lump_misses, 0, numlumps * sizeof(*wadperf_lump_misses));

    W_ResetPerfStats ();

    W_BuildLumpHash ();
}




//
// W_InitFile
// Just initialize from a single file.
//
void W_InitFile (char* filename)
{
    char*	names[2];

    names[0] = filename;
    names[1] = NULL;
    W_InitMultipleFiles (names);
}



//
// W_NumLumps
//
int W_NumLumps (void)
{
    return numlumps;
}



//
// W_CheckNumForName
// Returns -1 if name not found.
//

int W_CheckNumForName (char* name)
{
    union {
	char	s[9];
	int	x[2];
	
    } name8;
    
    int		v1;
    int		v2;
    int         hash;
    int         i;
    lumpinfo_t*	lump_p;

    // make the name into two integers for easy compares
    strncpy (name8.s,name,8);

    // in case the name was a fill 8 chars
    name8.s[8] = 0;

    // case insensitive
    strupr (name8.s);		

    v1 = name8.x[0];
    v2 = name8.x[1];


    if (lumphash_next)
    {
    hash = W_LumpNameHash (v1, v2);

    for (i = lumphash[hash] ; i != -1 ; i = lumphash_next[i])
    {
        lump_p = &lumpinfo[i];
        if ( *(int *)lump_p->name == v1
         && *(int *)&lump_p->name[4] == v2)
        {
        return i;
        }
    }

    // TFB. Not found.
    return -1;
    }


    // scan backwards so patch lump files take precedence
    lump_p = lumpinfo + numlumps;

    while (lump_p-- != lumpinfo)
    {
    if ( *(int *)lump_p->name == v1
         && *(int *)&lump_p->name[4] == v2)
	{
	    return lump_p - lumpinfo;
	}
    }

    // TFB. Not found.
    return -1;
}




//
// W_GetNumForName
// Calls W_CheckNumForName, but bombs out if not found.
//
int W_GetNumForName (char* name)
{
    int	i;

    i = W_CheckNumForName (name);
    
    if (i == -1)
      I_Error ("W_GetNumForName: %s not found!", name);
      
    return i;
}


//
// W_LumpLength
// Returns the buffer size needed to load the given lump.
//
int W_LumpLength (int lump)
{
    if (lump >= numlumps)
	I_Error ("W_LumpLength: %i >= numlumps",lump);

    return lumpinfo[lump].size;
}



//
// W_ReadLump
// Loads the lump into the given buffer,
//  which must be >= W_LumpLength().
//
void
W_ReadLump
( int		lump,
  void*		dest )
{
    int		c;
    lumpinfo_t*	l;
    int		handle;
	
    if (lump >= numlumps)
	I_Error ("W_ReadLump: %i >= numlumps",lump);

    l = lumpinfo+lump;
	
    // ??? I_BeginRead ();
	
    if (l->handle == -1)
    {
	// reloadable file, so use open / read / close
	if ( (handle = open (reloadname,O_RDONLY | O_BINARY)) == -1)
	    I_Error ("W_ReadLump: couldn't open %s",reloadname);
    }
    else
	handle = l->handle;
		
    if (wadread_last_handle != handle || wadread_last_pos != l->position)
	lseek (handle, l->position, SEEK_SET);
    c = W_ReadFully (handle, dest, l->size);

    wadread_last_handle = handle;
    wadread_last_pos = l->position + c;

    if (wadperf_enabled)
    {
        wadperf_reads++;
        wadperf_read_bytes += c;
    }

    if (c < l->size)
	I_Error ("W_ReadLump: only read %i of %i on lump %i",
		 c,l->size,lump);	

    if (l->handle == -1)
    {
	close (handle);
    wadread_last_handle = -2;
    }
		
    // ??? I_EndRead ();
}


void
W_ReadLumpHeader
( int		lump,
  void*		dest,
  int		size )
{
    int		c;
    lumpinfo_t*	l;
    int		handle;

    if (lump >= numlumps)
	I_Error ("W_ReadLumpHeader: %i >= numlumps",lump);

    l = lumpinfo+lump;

    if (size > l->size)
	size = l->size;

    if (l->handle == -1)
    {
	if ( (handle = open (reloadname,O_RDONLY | O_BINARY)) == -1)
	    I_Error ("W_ReadLumpHeader: couldn't open %s",reloadname);
    }
    else
	handle = l->handle;

    if (wadread_last_handle != handle || wadread_last_pos != l->position)
	lseek (handle, l->position, SEEK_SET);
    c = W_ReadFully (handle, dest, size);

    wadread_last_handle = handle;
    wadread_last_pos = l->position + c;

    if (wadperf_enabled)
    {
        wadperf_reads++;
        wadperf_read_bytes += c;
    }

    if (c < size)
	I_Error ("W_ReadLumpHeader: only read %i of %i on lump %i",
		 c,size,lump);

    if (l->handle == -1)
    {
	close (handle);
    wadread_last_handle = -2;
    }
}




//
// W_CacheLumpNum
//
void*
W_CacheLumpNum
( int		lump,
  int		tag )
{
    if ((unsigned)lump >= numlumps)
	I_Error ("W_CacheLumpNum: %i >= numlumps",lump);
		
    if (!lumpcache[lump])
    {
	// read the lump in
	
	//printf ("cache miss on lump %i\n",lump);
    if (wadperf_enabled)
    {
        wadperf_cache_misses++;
        if (wadperf_lump_misses)
        wadperf_lump_misses[lump]++;
    }
	Z_Malloc (W_LumpLength (lump), tag, &lumpcache[lump]);
	W_ReadLump (lump, lumpcache[lump]);
    }
    else
    {
	//printf ("cache hit on lump %i\n",lump);
    if (wadperf_enabled)
    {
        wadperf_cache_hits++;
    }
	Z_ChangeTag (lumpcache[lump],tag);
    }
	
    return lumpcache[lump];
}



//
// W_CacheLumpName
//
void*
W_CacheLumpName
( char*		name,
  int		tag )
{
    return W_CacheLumpNum (W_GetNumForName(name), tag);
}


//
// W_Profile
//
int		info[2500][10];
int		profilecount;

void W_Profile (void)
{
    int		i;
    memblock_t*	block;
    void*	ptr;
    char	ch;
    FILE*	f;
    int		j;
    char	name[9];
	
	
    for (i=0 ; i<numlumps ; i++)
    {	
	ptr = lumpcache[i];
	if (!ptr)
	{
	    ch = ' ';
	    continue;
	}
	else
	{
	    block = (memblock_t *) ( (byte *)ptr - sizeof(memblock_t));
	    if (block->tag < PU_PURGELEVEL)
		ch = 'S';
	    else
		ch = 'P';
	}
	info[i][profilecount] = ch;
    }
    profilecount++;
	
    f = fopen ("waddump.txt","w");
    name[8] = 0;

    for (i=0 ; i<numlumps ; i++)
    {
	memcpy (name,lumpinfo[i].name,8);

	for (j=0 ; j<8 ; j++)
	    if (!name[j])
		break;

	for ( ; j<8 ; j++)
	    name[j] = ' ';

	fprintf (f,"%s ",name);

	for (j=0 ; j<profilecount ; j++)
	    fprintf (f,"    %c",info[i][j]);

	fprintf (f,"\n");
    }
    fclose (f);
}


