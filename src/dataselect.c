/***************************************************************************
 * dataselect.c - Mini-SEED data selection.
 *
 * Opens one or more user specified files, applys filtering criteria
 * and outputs any matched data while time-ordering the data and
 * optionally pruning any overlap (at record or sample level) and
 * splitting records on day, hour or minute boundaries.
 *
 * Written by Chad Trabant, IRIS Data Management Center.
 *
 * modified 2006.248
 ***************************************************************************/

// Go over sample-level pruning logic, USE TOLERANCE, test and re-test

// trimrecord does not take start/end as boundaries but explicit times!!  rework for boundaries.

// Splitting on a day boundary doesn't work in fringe case, figure that out.

/* _ISOC9X_SOURCE needed to get a declaration for llabs on some archs */
#define _ISOC9X_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <errno.h>
#include <time.h>
#include <regex.h>

#include <libmseed.h>

#include "dsarchive.h"

#define VERSION "0.1"
#define PACKAGE "dataselect"

/* For a linked list of strings, as filled by strparse() */
typedef struct StrList_s {
  char             *element;
  struct StrList_s *next;
} StrList;

/* POD request record containers */
typedef struct ReqRec_s {
  struct StrList_s *strlist;
  char  *network;
  char  *station;
  char  *location;
  char  *channel;
  time_t datastart;
  time_t dataend;
  char  *filename;
  char  *headerdir;
  time_t reqstart;
  time_t reqend;
  char   pruned;
  struct ReqRec_s *prev;
  struct ReqRec_s *next;
} ReqRec;

/* Input/output file information containers */
typedef struct Filelink_s {
  struct ReqRec_s *reqrec;
  char    *infilename;     /* Input file name */
  FILE    *infp;           /* Input file descriptor */
  char    *outfilename;    /* Output file name */
  FILE    *outfp;          /* Output file descriptor */
  int      reordercount;   /* Number of records re-ordered */
  int      recsplitcount;  /* Number of records split */
  int      recrmcount;     /* Number of records removed */
  int      rectrimcount;   /* Number of records trimed */
  hptime_t earliest;       /* Earliest data time in this file */
  hptime_t latest;         /* Latest data time in this file */
  int      byteswritten;   /* Number of bytes written out */
  struct Filelink_s *next;
} Filelink;

/* Archive (output structure) definition containers */
typedef struct Archive_s {
  DataStream  datastream;
  struct Archive_s *next;
} Archive;

/* Mini-SEED record information structures */
typedef struct Record_s {
  struct Filelink_s *flp;
  off_t     offset;
  int       reclen;
  hptime_t  starttime;
  hptime_t  endtime;
  char      quality;
  hptime_t  newstart;
  hptime_t  newend;
  struct Record_s *prev;
  struct Record_s *next;
} Record;

/* Record map, holds Record structures for a given MSTrace */
typedef struct RecordMap_s {
  long long int    recordcnt;
  struct Record_s *first;
  struct Record_s *last;
} RecordMap;

/* Time segment containers */
typedef struct TimeSegment_s {
  hptime_t  starttime;
  hptime_t  endtime;
  struct TimeSegment_s *next;
} TimeSegment;


static int processpod (char *requestfile, char *datadir);
static int setofilelimit (int limit);
static ReqRec *readreqfile (char *requestfile);
static int writereqfile (char *requestfile, ReqRec *rrlist);

static MSTraceGroup *processtraces (void);
static void writetraces (MSTraceGroup *mstg);
static int trimrecord (Record *rec, char *recbuf);
static void record_handler (char *record, int reclen);

static int prunetraces (MSTraceGroup *mstg);
static int trimtraces (MSTrace *lptrace, MSTrace *hptrace);
static int qcompare (const char quality1, const char quality2);

static int readfiles (void);
static MSTraceGroup *reinitgroup (MSTraceGroup *mstg);
static void printmodsummary (flag nomods);
static void printtracemap (MSTraceGroup *mstg);

static int processparam (int argcount, char **argvec);
static char *getoptval (int argcount, char **argvec, int argopt);
static int strparse (const char *string, const char *delim, StrList **list);
static void addfile (char *filename, ReqRec *reqrec);
static int  addarchive(const char *path, const char *layout);
static int readregexfile (char *regexfile, char **pppattern);
static void freefilelist (void);
static void usage (int level);

static flag     verbose       = 0;
static flag     basicsum      = 0;    /* Controls printing of basic summary */
static flag     bestquality   = 1;    /* Use Q, D, R quality to retain the "best" data when pruning */
static flag     prunedata     = 0;    /* Prune data: 'r= record level, 's' = sample level */
static double   timetol       = -1.0; /* Time tolerance for continuous traces */
static double   sampratetol   = -1.0; /* Sample rate tolerance for continuous traces */
static char     restampqind   = 0;    /* Re-stamp data record/quality indicator */
static int      reclen        = -1;   /* Input data record length, autodetected in most cases */
static flag     modsummary    = 0;    /* Print modification summary after all processing */
static hptime_t starttime     = HPTERROR;  /* Limit to records after starttime */
static hptime_t endtime       = HPTERROR;  /* Limit to records before endtime */
static char     splitboundary = 0;    /* Split records on day(d), hour(h) or minute(m) boundaries */

static regex_t *match         = 0;    /* Compiled match regex */
static regex_t *reject        = 0;    /* Compiled reject regex */

static char    *podreqfile    = 0;    /* POD h. request file */
static char    *poddatadir    = 0;    /* POD data directory */

static flag     replaceinput  = 0;    /* Replace input files */
static flag     nobackups     = 0;    /* Remove re-named original files when done with them */
static char    *outputfile    = 0;    /* Single output file */
static Archive *archiveroot   = 0;    /* Output file structures */

static char     recordbuf[16384];     /* Global record buffer */

static Filelink *filelist = 0;        /* List of input files */

static MSTraceGroup *mstg = 0;        /* Global MSTraceGroup */


int
main ( int argc, char **argv )
{  
  /* Process input parameters */
  if (processparam (argc, argv) < 0)
    return 1;
  
  if ( podreqfile && poddatadir )
    {
      if ( verbose > 2 )
	fprintf (stderr, "Pruning POD structure:\nrequest file: %s, data dir: %s\n",
		 podreqfile, poddatadir);
      
      if ( processpod (podreqfile, poddatadir) )
	fprintf (stderr, "ERROR processing POD structure\n");
    }
  else
    {
      if ( verbose > 2 )
	fprintf (stderr, "Pruning input files\n");
      
      /* Read and process all files specified on the command line */
      readfiles ();
      processtraces ();
      
      if ( modsummary )
	printmodsummary (verbose);
      
      freefilelist ();
    }
  
  return 0;
}  /* End of main() */


/***************************************************************************
 * processpod:
 *
 * Process data from a POD structure.  Group input data files by
 * channel and prune each group separately using a Q > D > R priority.
 *
 * Returns 0 on success and -1 on error.
 ***************************************************************************/
static int
processpod (char *requestfile, char *datadir)
{
  ReqRec *reqrecs;
  ReqRec *fox, *hound;
  ReqRec *reciter, *recnext;
  Filelink *flp;
  int filecount;

  char tmpfilename[1024];
  
  /* Read the request file */
  if ( ! (reqrecs = readreqfile(requestfile)) )
    return -1;
  
  /* Group the input files by channel (complete matching NSLC) and
   * prune as a group.
   *
   * All files in the request file will be pruned, grouping by channel
   * is a minor memory footprint optimization.
   */
  
  hound = reqrecs;
  while ( hound )
    {
      if ( hound->pruned )
	{
	  hound = hound->next;
	  continue;
	}
      
      /* Build appropriate data file name using base data dir and record file name */
      snprintf (tmpfilename, sizeof(tmpfilename), "%s/%s/%s",
		poddatadir, hound->station, hound->filename);
      
      /* Add file to list to be pruned and mark it as pruned */
      addfile (tmpfilename, hound);
      hound->pruned = 1;
      filecount = 1;
      
      /* Find all the other NSLC files and add them to the list */
      fox = reqrecs;
      while ( fox )
	{
	  if ( fox == hound || fox->pruned )
	    {
	      fox = fox->next;
	      continue;
	    }
	  
	  if ( ! strcmp (hound->network, fox->network) &&
	       ! strcmp (hound->station, fox->station) &&
	       ! strcmp (hound->location, fox->location) &&
	       ! strcmp (hound->channel, fox->channel) )
	    {
	      /* Build appropriate data file name using base data dir and record file name */
	      snprintf (tmpfilename, sizeof(tmpfilename), "%s/%s/%s",
			poddatadir, fox->station, fox->filename);
	      
	      /* Add file to list to be pruned and mark it as pruned */
	      addfile (tmpfilename, fox);
	      fox->pruned = 1;
	      filecount++;
	    }
	  
	  fox = fox->next;
	}
      
      /* Increase open file limit if necessary, in general
       * we need 2 X the filecount and some wiggle room. */
      if ( setofilelimit ((filecount * 2) + 20) == -1 )
	{
	  freefilelist ();
	  hound = hound->next;
	  continue;
	}
      
      /* Read & prune data files & free file list */
      readfiles ();
      processtraces ();
      
      /* Update the time values in the request records */
      flp = filelist;
      while ( flp )
	{
	  if ( flp->byteswritten == 0 )
	    {
	      /* This signals no coverage, request record will be removed below */
	      flp->reqrec->datastart = flp->reqrec->dataend = 0;
	    }
	  else
	    {
	      flp->reqrec->datastart = (time_t) (MS_HPTIME2EPOCH(flp->earliest));
	      flp->reqrec->dataend = (time_t) (MS_HPTIME2EPOCH(flp->latest));
	    }
	  
	  flp = flp->next;
	}
      
      if ( modsummary )
	printmodsummary (verbose);
      
      /* Clean up global file list */
      freefilelist ();
      
      hound = hound->next;
    }
  
  if ( verbose > 1 )
    fprintf (stderr, "Renaming and rewriting request file (h.)\n");
  
  /* Remove request records that have no time coverage, i.e. all data pruned */
  reciter = reqrecs;
  while ( reciter )
    {
      recnext = reciter->next;
      
      if ( reciter->datastart == 0 && reciter->dataend == 0 )
	{
	  if ( reciter == reqrecs )  /* First in chain */
	    reqrecs = reciter->next;
	  else if ( reciter->next == 0 )  /* Last in chain */
	    reciter->prev->next = 0;
	  else  /* In middle of chain */
	    {
	      reciter->prev->next = reciter->next;
	      reciter->next->prev = reciter->prev;
	    }
	  
	  free (reciter);
	}
      
      reciter = recnext;
    }
  
  /* Rename original request file (add '.orig') */
  snprintf (tmpfilename, sizeof(tmpfilename), "%s.orig", requestfile);
  
  if ( rename (requestfile, tmpfilename) )
    fprintf (stderr, "ERROR renaming %s -> %s : '%s'\n",
	     tmpfilename, requestfile, strerror(errno));
  
  /* Write the request file */
  if ( writereqfile(requestfile, reqrecs) )
    return -1;
  
  return 0;
}  /* End of processpod() */


/***************************************************************************
 * setofilelimit:
 *
 * Check the current open file limit and if it is not >= 'limit' try
 * to increase it to 'limit'.
 *
 * Returns the open file limit on success and -1 on error.
 ***************************************************************************/
static int
setofilelimit (int limit)
{
  struct rlimit rlim;
  
  /* Get the current soft open file limit */
  if ( getrlimit (RLIMIT_NOFILE, &rlim) == -1 )
    {
      fprintf (stderr, "ERROR getrlimit failed to get open file limit\n");
      return -1;
    }
  
  if ( rlim.rlim_cur < limit )
    {
      rlim.rlim_cur = (rlim_t) limit;
      
      if ( verbose > 1 )
	fprintf (stderr, "Setting open file limit to %d\n",
		 (int) rlim.rlim_cur);
      
      if ( setrlimit (RLIMIT_NOFILE, &rlim) == -1 )
	{
	  fprintf (stderr, "ERROR setrlimit failed to set open file limit\n");
	  return -1;
	}
    }
  
  return (int) rlim.rlim_cur;
}  /* End of setofilelimit() */


/***************************************************************************
 * readreqfile:
 *
 * Parse the specified request file and return a chain of ReqRec structs.
 *
 * The request file (h. file) is can reference the same file multiple
 * times, but these request lines will be grouped together for
 * processing and the output request file will only have one line per
 * input file.  The data and request start/end times will be the
 * outermost times of any grouping.
 *
 * The request file is assumed to NEVER reference a file for more than
 * one channel, in otherwords there is never two or more unique
 * channels in a given file.
 *
 * Returns a pointer on success and NULL on error.
 ***************************************************************************/
static ReqRec *
readreqfile (char *requestfile)
{
  FILE *rf;
  ReqRec *rr = 0;
  ReqRec *lastrr = 0;
  ReqRec *newrr;
  ReqRec *looprr;
  char reqline[200];
  StrList *list = 0;
  StrList *listptr;
  int reqreccnt = 0;

  char tmpfilename[1024];

  if ( verbose )
    fprintf (stderr, "Reading request file: %s\n", requestfile);
  
  if ( ! (rf = fopen (requestfile, "rb")) )
    {
      fprintf (stderr, "ERROR Cannot open request file '%s': %s\n",
	       requestfile, strerror(errno));
      return NULL;
    }
  
  while ( fgets(reqline, sizeof(reqline), rf) )
    {
      if ( strparse(reqline, "\t", &list) != 10 )
	{
	  if ( verbose > 2 )
	    fprintf (stderr, "ERROR skipping request line: '%s'\n", reqline);
	  continue;
	}
      
      reqreccnt++;
      
      newrr = (ReqRec *) malloc (sizeof(ReqRec));
      newrr->strlist = list;
      
      listptr = list;
      
      newrr->station = listptr->element;
      listptr = listptr->next;
      newrr->network = listptr->element;
      listptr = listptr->next;
      newrr->channel = listptr->element;
      listptr = listptr->next;
      newrr->location = listptr->element;
      listptr = listptr->next;
      newrr->datastart = (time_t) (MS_HPTIME2EPOCH (ms_seedtimestr2hptime(listptr->element)));
      listptr = listptr->next;
      newrr->dataend = (time_t) (MS_HPTIME2EPOCH (ms_seedtimestr2hptime(listptr->element)));
      listptr = listptr->next;
      newrr->filename = listptr->element;
      listptr = listptr->next;
      newrr->headerdir = listptr->element;
      listptr = listptr->next;
      newrr->reqstart = (time_t) (MS_HPTIME2EPOCH (ms_seedtimestr2hptime(listptr->element)));
      listptr = listptr->next;
      newrr->reqend = (time_t) (MS_HPTIME2EPOCH (ms_seedtimestr2hptime(listptr->element)));
      listptr = listptr->next;
      
      newrr->pruned = 0;
      newrr->prev = 0;
      newrr->next = 0;
      
      /* Build appropriate data file name using base data dir and record file name */
      snprintf (tmpfilename, sizeof(tmpfilename), "%s/%s/%s",
		poddatadir, newrr->station, newrr->filename);
      
      /* Check for file access */
      if ( access(tmpfilename, F_OK) )
	{
	  fprintf (stderr, "Cannot find file '%s', keeping a placeholder\n", tmpfilename);
	  newrr->pruned = 1;
	}
      
      /* If this is the first ReqRec */
      if ( ! rr )
	{
	  rr = newrr;
	  lastrr = newrr;
	}
      /* Other wise add to or update the ReqRec list */
      else
	{
	  /* Search the list for matching files */
	  looprr = rr;
	  while ( looprr )
	    {
	      /* If a matching file name was found update the time stamps and
	       * free this new one.  One buried assumption with this approach
	       * is that there will never be more than one channel in a given
	       * data file (unique Net, Sta, Loc, Chan & Quality) */
	      if ( ! strcmp (looprr->filename, newrr->filename) )
		{
		  if ( looprr->datastart > newrr->datastart )
		    looprr->datastart = newrr->datastart;
		  if ( looprr->dataend < newrr->dataend )
		    looprr->dataend = newrr->dataend;
		  
		  if ( looprr->reqstart > newrr->reqstart )
		    looprr->reqstart = newrr->reqstart;
		  if ( looprr->reqend < newrr->reqend )
		    looprr->reqend = newrr->reqend;
		  
		  free ( newrr );
		  break;
		}
	      
	      looprr = looprr->next;
	    }
	  
	  /* If no match add new ReqRec to the list */
	  if ( ! looprr )
	    {
	      newrr->prev = lastrr;
	      lastrr->next = newrr;
	      lastrr = newrr;
	    }
	}
    }
  
  if ( verbose )
    fprintf (stderr, "Read %d request records (lines)\n", reqreccnt);
  
  fclose (rf);
  
  return rr;
} /* End of readreqfile() */


/***************************************************************************
 * writereqfile:
 *
 * Write a request file for a chained ReqRec list.
 *
 * Returns 0 on success and -1 on error.
 ***************************************************************************/
static int
writereqfile (char *requestfile, ReqRec *rrlist)
{
  FILE *rf;
  ReqRec *rr = 0;
  char datastart[30];
  char dataend[30];
  char reqstart[30];
  char reqend[30];
  
  int reqreccnt = 0;

  if ( verbose )
    fprintf (stderr, "Writing request file: %s\n", requestfile);
  
  if ( ! (rf = fopen (requestfile, "wb")) )
    {
      fprintf (stderr, "ERROR Cannot open request file '%s': %s\n",
	       requestfile, strerror(errno));
      return -1;
    }
  
  rr = rrlist;
  
  while ( rr )
    {
      reqreccnt++;
      
      strftime (datastart, sizeof(datastart), "%Y,%j,%H:%M:%S", gmtime(&rr->datastart));
      strftime (dataend, sizeof(dataend), "%Y,%j,%H:%M:%S", gmtime(&rr->dataend));
      strftime (reqstart, sizeof(reqstart), "%Y,%j,%H:%M:%S", gmtime(&rr->reqstart));
      strftime (reqend, sizeof(reqend), "%Y,%j,%H:%M:%S", gmtime(&rr->reqend));
      
      fprintf (rf,"%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
	       rr->station,rr->network,rr->channel,rr->location,
	       datastart, dataend,
	       rr->filename, rr->headerdir,
	       reqstart, reqend);
      
      rr = rr->next;
    }
  
  fclose (rf);
  
  if ( verbose )
    fprintf (stderr, "Wrote %d request records (lines)\n", reqreccnt);

  return 0;
} /* End of writereqfile() */


/***************************************************************************
 * processtraces:
 *
 * Process all MSTrace entries in the global MSTraceGroups by first
 * pruning them and then writing out the remaining data.
 *
 * Returns a pointer to a MSTraceGroup on success and NULL on error.
 ***************************************************************************/
static MSTraceGroup *
processtraces (void)
{
  /* Sort trace group by srcname, sample rate, starttime and descending end time */
  mst_groupsort (mstg);
  
  if ( verbose > 2 )
    printtracemap (mstg);
  
  /* Prune data */
  if ( prunedata )
    prunetraces (mstg);
  
  /* Write all MSTrace associated records to output file(s) */
  writetraces (mstg);
  
  return NULL;
}  /* End of processtraces() */


/***************************************************************************
 * writetraces():
 *
 * Write all MSTrace associated records to output file(s).  If an output
 * file is specified all records will be written to it, otherwise
 * records will be written to the original files and (optionally)
 * backups of the original files will remain.
 * 
 * This routine will also call trimrecord() to trim a record when data
 * suturing is requested.  Record trimming is triggered when
 * MSRecord.newstart or MSRecord.newend are set for any output
 * records.
 *
 * The quality flag is optionally set for all output records.
 *
 ***************************************************************************/
static void
writetraces (MSTraceGroup *mstg)
{
  static int totalrecsout = 0;
  static int totalbytesout = 0;
  char *wb = "wb";
  char *ab = "ab";
  char *mode;
  char stopflag = 0;
  
  hptime_t hpdelta;
  
  MSTrace *mst;
  RecordMap *recmap;
  Record *rec;
  Filelink *flp;
  Archive *arch;
  
  FILE *ofp = 0;
  
  if ( ! mstg )
    return;
  
  if ( ! mstg->traces )
    return;
  
  /* Open the output file if specified */
  if ( outputfile )
    {
      /* Decide if we are appending or overwriting */
      mode = ( totalbytesout ) ? ab : wb;
      
      if ( strcmp (outputfile, "-") == 0 )
        {
          ofp = stdout;
        }
      else if ( (ofp = fopen (outputfile, mode)) == NULL )
        {
          fprintf (stderr, "ERROR Cannot open output file: %s (%s)\n",
                   outputfile, strerror(errno));
          return;
        }
    }
  
  mst = mstg->traces;
  
  while ( mst && ! stopflag )
    {
      recmap = (RecordMap *) mst->private;
      rec = recmap->first;
      
      while ( rec && ! stopflag )
	{
	  /* Skip marked records */
	  if ( rec->reclen == 0 )
	    {
	      rec = rec->next;
	      continue;
	    }
	  
	  /* Make sure the record buffer is large enough */
	  if ( rec->reclen > sizeof(recordbuf) )
	    {
	      fprintf (stderr, "ERROR Record length (%d bytes) larger than buffer (%llu bytes)\n",
		       rec->reclen, (long long unsigned int)sizeof(recordbuf));
	      stopflag = 1;
	      break;
	    }
	  
	  /* Open file for reading if not already done */
	  if ( ! rec->flp->infp )
	    if ( ! (rec->flp->infp = fopen (rec->flp->infilename, "rb")) )
	      {
		fprintf (stderr, "ERROR opening '%s' for reading: %s\n",
			 rec->flp->infilename, strerror(errno));
		stopflag = 1;
		break;
	      }
	  
	  /* Seek to record offset */
	  if ( lmp_fseeko (rec->flp->infp, rec->offset, SEEK_SET) == -1 )
	    {
	      fprintf (stderr, "ERROR Cannot seek in '%s': %s\n",
		       rec->flp->infilename, strerror(errno));
	      stopflag = 1;
	      break;
	    }
	  
	  /* Read record into buffer */
	  if ( fread (recordbuf, rec->reclen, 1, rec->flp->infp) != 1 )
	    {
	      fprintf (stderr, "ERROR reading %d bytes at offset %llu from '%s'\n",
		       rec->reclen, (long long unsigned)rec->offset,
		       rec->flp->infilename);
	      stopflag = 1;
	      break;
	    }
	  
	  /* Trim data from the record if new start or end times are specifed */
	  if ( rec->newstart || rec->newend )
	    {
	      if ( trimrecord (rec, recordbuf) )
		{
		  rec = rec->next;
		  continue;
		}
	    }
	  
	  /* Re-stamp quality indicator if specified */
          if ( restampqind )
            {
              if ( verbose > 1 )
                fprintf (stderr, "Re-stamping data quality indicator to '%c'\n", restampqind);
              
              *(recordbuf + 6) = restampqind;
            }
	  
	  /* Write to a single output file if specified */
	  if ( ofp )
	    {
	      if ( fwrite (recordbuf, rec->reclen, 1, ofp) != 1 )
		{
		  fprintf (stderr, "ERROR writing to '%s'\n", outputfile);
		  stopflag = 1;
		  break;
		}
	    }
	  
	  /* Write to Archive(s) if specified */
	  if ( archiveroot )
	    {
	      MSRecord *msr = 0;
	      
	      if ( msr_unpack (recordbuf, rec->reclen, &msr, 0, verbose) != MS_NOERROR )
		{
		  fprintf (stderr, "ERROR unpacking Mini-SEED, cannot write to archive\n");
		}
	      else
		{
		  arch = archiveroot;
		  while ( arch )
		    {
		      ds_streamproc (&arch->datastream, msr, 0, verbose-1);
		      arch = arch->next;
		    }
		}
	      
	      msr_free (&msr);
	    }

	  /* Open original file for output if replacing input and write */
	  if ( replaceinput )
	    {
	      if ( ! rec->flp->outfp )
		if ( ! (rec->flp->outfp = fopen (rec->flp->outfilename, "wb")) )
		  {
		    fprintf (stderr, "ERROR opening '%s' for writing: %s\n",
			     rec->flp->outfilename, strerror(errno));
		    stopflag = 1;
		    break;
		  }
	      
	      if ( fwrite (recordbuf, rec->reclen, 1, rec->flp->outfp) != 1 )
		{
		  fprintf (stderr, "ERROR writing to '%s'\n", rec->flp->outfilename);
		  stopflag = 1;
		  break;
		}
	    }
	  
	  /* Update file entry time stamps and counts */
	  if ( ! rec->flp->earliest || (rec->flp->earliest > rec->starttime) )
	    {
	      rec->flp->earliest = rec->starttime;
	    }
	  if ( ! rec->flp->latest || (rec->flp->latest < rec->endtime) )
	    {
	      hpdelta = ( mst->samprate ) ? (hptime_t) (HPTMODULUS / mst->samprate) : 0;
	      rec->flp->latest = rec->endtime + hpdelta;
	    }
	  
	  rec->flp->byteswritten += rec->reclen;
	  
	  totalrecsout++;
	  totalbytesout += rec->reclen;
	  
	  rec = rec->next;
	}
      
      mst = mst->next;
    }
  
  /* Close all open input & output files and remove backups if requested */
  flp = filelist;
  while ( flp )
    {
      if ( ! ofp && verbose )
	{
	  if ( replaceinput )
	    fprintf (stderr, "Wrote %d bytes from file %s (was %s)\n",
		     flp->byteswritten, flp->infilename, flp->outfilename);
	  else
	    fprintf (stderr, "Wrote %d bytes from file %s\n",
		     flp->byteswritten, flp->infilename);
	}

      if ( flp->infp )
	{
	  fclose (flp->infp);
	  flp->infp = 0;
	}
      
      if ( flp->outfp )
	{
	  fclose (flp->outfp);
	  flp->outfp = 0;
	}
      
      if ( nobackups && ! ofp )
	if ( unlink (flp->infilename) )
	  fprintf (stderr, "ERROR removing '%s': %s\n",
		   flp->infilename, strerror(errno));
      
      flp = flp->next;
    }
  
  /* Close output file if used */
  if ( ofp )
    {
      fclose (ofp);
      ofp = 0;
    }

  if ( verbose )
    {
      fprintf (stderr, "Wrote %d bytes of %d records to output file(s)\n",
	       totalbytesout, totalrecsout);
    }
  
  return;
}  /* End of writetraces() */


/***************************************************************************
 * trimrecord():
 *
 * Unpack a data record and trim samples, either from the beginning or
 * the end, to fit the Record.newstart and/or Record.newend times and
 * pack the record.
 *
 * Return 0 on success and -1 on failure.
 ***************************************************************************/
static int
trimrecord (Record *rec, char *recordbuf)
{
  MSRecord *msr = 0;
  
  char srcname[50];
  char stime[30];
  char etime[30];
  
  int trimsamples;
  int samplesize;
  int packedsamples;
  int packedrecords;
  int retcode;
  
  if ( ! rec || ! recordbuf )
    return -1;
  
  /* Sanity check for new start/end times */
  if ( (rec->newstart && rec->newend && rec->newstart >= rec->newend) ||
       (rec->newstart && (rec->newstart <= rec->starttime || rec->newstart >= rec->endtime)) ||
       (rec->newend && (rec->newend >= rec->endtime || rec->newend <= rec->starttime)) )
    {
      fprintf (stderr, "ERROR: problem with new start/end record times, skipping.\n");
      fprintf (stderr, "  Original record from %s\n", rec->flp->infilename);
      ms_hptime2seedtimestr (rec->starttime, stime);
      ms_hptime2seedtimestr (rec->endtime, etime);
      fprintf (stderr, "      Start: %s       End: %s\n", stime, etime);
      ms_hptime2seedtimestr (rec->newstart, stime);
      ms_hptime2seedtimestr (rec->newend, etime);
      fprintf (stderr, "  New start: %s   New end: %s\n", stime, etime);
    }
  
  /* Unpack data record */
  if ( (retcode = msr_unpack(recordbuf, rec->reclen, &msr, 1, verbose-1)) != MS_NOERROR )
    {
      fprintf (stderr, "ERROR: cannot unpacking Mini-SEED record: %s\n", get_errorstr(retcode));
      return -1;
    }
  
  if ( verbose > 1 )
    {
      msr_srcname (msr, srcname);
      fprintf (stderr, "Triming record: %s (%c)\n", srcname, msr->dataquality);
      ms_hptime2seedtimestr (rec->starttime, stime);
      ms_hptime2seedtimestr (rec->endtime, etime);
      fprintf (stderr, "     Start: %s       End: %s\n", stime, etime);
      ms_hptime2seedtimestr (rec->newstart, stime);
      ms_hptime2seedtimestr (rec->newend, etime);
      fprintf (stderr, " New start: %s   New end: %s\n", stime, etime);
    }
  
  /* Remove samples from the beginning of the record */
  if ( rec->newstart )
    {
      trimsamples = (int) (((rec->newstart - rec->starttime) / HPTMODULUS) * msr->samprate + 0.5);
      
      if ( verbose > 2 )
	fprintf (stderr, "Removing %d samples from the start\n", trimsamples);
      
      msr->starttime = rec->newstart;
      
      samplesize = get_samplesize (msr->sampletype);
      
      memmove (msr->datasamples,
	       (char *)msr->datasamples + (samplesize * trimsamples),
	       samplesize * (msr->numsamples - trimsamples));
      
      msr->numsamples -= trimsamples;
      msr->samplecnt -= trimsamples;
    }
  
  /* Remove samples from the end of the record */
  if ( rec->newend )
    {
      trimsamples = (int) (((rec->endtime - rec->newend) / HPTMODULUS) * msr->samprate + 0.5);
      
      if ( verbose > 2 )
	fprintf (stderr, "Removing %d samples from the end\n", trimsamples);
      
      msr->numsamples -= trimsamples;
      msr->samplecnt -= trimsamples;
    }
  
  /* Pack the data record into the global record buffer used by writetraces() */
  packedrecords = msr_pack (msr, &record_handler, &packedsamples, 1, verbose-1);
  
  /* Clean up MSRecord */
  msr_free (&msr);  
  
  return 0;
}  /* End of trimrecord() */


/***************************************************************************
 * record_handler():
 *
 * Used by trimrecord() to save repacked Mini-SEED to global record
 * buffer.
 ***************************************************************************/
static void
record_handler (char *record, int reclen)
{
  /* Copy record to global record buffer */
  memcpy (recordbuf, record, reclen);
  
}  /* End of record_handler() */


/***************************************************************************
 * prunetraces():
 *
 * Remove all redundant records from the MSTraces.
 *
 * Compare each MSTrace to every other MSTrace in the MSTraceGroup,
 * for each MSTrace that has the same NSLC and sampling rate as
 * another MSTrace decide which one has priority and call trimtraces()
 * to determine if there is any overlap and remove the overlapping
 * records.
 *
 * If the 'bestquality' option is being used the priority is
 * determined from the data quality flags with Q > D > R.  If the
 * qualities are the same (or bestquality not requested) the longer
 * MSTrace will get priority over the shorter.
 *
 * To avoid numerous string operations this routine will compare
 * MSTrace srcnames by comparing 44 bytes starting from
 * MSTrace->network which should be the strings for network, station,
 * location and channel.  As long as the MSTrace struct does not
 * change this shortcut will be valid.
 *
 * Return 0 on success and -1 on failure.
 ***************************************************************************/
static int
prunetraces (MSTraceGroup *mstg)
{
  MSTrace *mst = 0;
  MSTrace *imst = 0;
  int priority = 0;

  if ( ! mstg )
    return -1;
  
  if ( ! mstg->traces )
    return -1;
  
  if ( verbose )
    fprintf (stderr, "Pruning MSTrace data\n");
  
  /* Compare each MSTrace to every other MSTrace */
  mst = mstg->traces;
  while ( mst )
    {
      imst = mst->next;
      while ( imst )
	{
	  /* Continue with next if srcname or sample rate are different */
	  if ( memcmp(mst->network, imst->network, 44) ||
	       ! MS_ISRATETOLERABLE (mst->samprate,imst->samprate) )
	    {
	      imst = imst->next;
	      continue;
	    }
	  
	  /* Test if the MSTraces overlap */
	  if ( mst->endtime > imst->starttime &&
	       mst->starttime < imst->endtime )
	    {
	      /* Determine priority:
	       *  -1 = mst > imst
	       *   0 = mst == imst
	       *   1 = mst < imst */
	      
	      /* If best quality is requested compare the qualities to determine priority */
	      if ( bestquality )
		priority = qcompare(mst->dataquality, imst->dataquality);
	      
	      /* If priorities are equal (qualities are equal or no checking) 
	       * give priority to the longest MSTrace */
	      if ( priority == 0 )
		{
		  if ( (mst->endtime - mst->starttime) > (imst->endtime - imst->starttime) )
		    priority = -1;
		  else
		    priority = 1;
		}
	      
	      /* Trim records from the lowest quality MSTrace */
	      if ( priority == 1 )
		trimtraces (mst, imst);
	      else
		trimtraces (imst, mst);
	    }
	  
	  imst = imst->next;
	}
      
      mst = mst->next;
    }
  
  return 0;
}  /* End of prunetraces() */


/***************************************************************************
 * trimtraces():
 *
 * Mark Records in the lower priority MSTrace that are completely
 * overlapped by the higher priority MSTrace.  Record entries are
 * marked by setting Record.reclen = 0.
 *
 * Time coverage for the higher priority MSTrace is determined and
 * stored in a series of TimeSegments structs.  Each Record in the
 * lower priority MSTrace is then compared to each TimeSegment to see
 * if it's completely overlapped.
 *
 * The record map chain (MSTrace->private->first pointer) must be time
 * ordered.
 *
 * lptrace = lower priority MSTrace (LP)
 * hptrace = higher priority MSTrace (HP)
 *
 * Returns the number of Record modifications on success and -1 on error.
 ***************************************************************************/
static int
trimtraces (MSTrace *lptrace, MSTrace *hptrace)
{
  RecordMap *recmap;
  Record *rec;
  
  TimeSegment *ts = 0;
  TimeSegment *tsp = 0;
  TimeSegment *newts;
  hptime_t hpdelta, hptimetol;
  hptime_t effstarttime, effendtime;
  int newsegment;

  char srcname[50];
  char stime[30];
  char etime[30];
  int modcount = 0;
  
  if ( ! lptrace || ! hptrace )
    return -1;
  
  /* Determine sample period in high precision time ticks */
  hpdelta = ( hptrace->samprate ) ? (hptime_t) (HPTMODULUS / hptrace->samprate) : 0;
  
  /* Determine time tolerance in high precision time ticks */
  hptimetol = ( timetol == -1 ) ? (hpdelta / 2) : (hptime_t) (HPTMODULUS * timetol);
  
  /* Build a TimeSegment list of coverage in the HP MSTrace.  Records
   * are in time order otherwise they wouldn't be in the MSTrace.
   * This re-calculation of the time coverage is done as an
   * optimization to avoid the need for each record in the HP MSTrace
   * to be compared to each record in the LP MSTrace.  The optimizaion
   * becomes less effective as gaps in the HP MSTrace coverage
   * increase. */
  recmap = (RecordMap *) hptrace->private;
  rec = recmap->first;
  newsegment = 1;
  while ( rec )
    {
      /* Check if record has been marked as non-contributing */
      if ( rec->reclen == 0 )
	{
	  rec = rec->next;
	  continue;
	}
      
      /* Determine effective record start and end times */
      effstarttime = ( rec->newstart ) ? rec->newstart : rec->starttime;
      effendtime = ( rec->newend ) ? rec->newend : rec->endtime;
      
      /* Create a new segment if a break in the time-series is detected */
      if ( tsp )
	if ( llabs((tsp->endtime + hpdelta) - effstarttime) > hptimetol )
	  newsegment = 1;
      
      if ( newsegment )
	{
	  newsegment = 0;
	  
	  newts = (TimeSegment *) malloc (sizeof(TimeSegment));
	  
	  if ( ts == 0 )
	    ts = tsp = newts;
	  
	  tsp->next = newts;
	  tsp = newts;
	  tsp->next = 0;
	  
	  tsp->starttime = effstarttime;
	}
      
      tsp->endtime = effendtime;
      
      rec = rec->next;
    }
  
  /* Traverse the Record chain for the LP MSTrace and mark Records
   * that are completely overlapped by the HP MSTrace coverage */
  recmap = (RecordMap *) lptrace->private;
  rec = recmap->first;
  while ( rec )
    {
      tsp = ts;
      while ( tsp )
	{
	  /* Determine effective record start and end times for comparison */
	  effstarttime = ( rec->newstart ) ? rec->newstart : rec->starttime;
	  effendtime = ( rec->newend ) ? rec->newend : rec->endtime;
	  
	  /* Mark Record if it is completely overlaped by HP data */
	  if ( effstarttime >= tsp->starttime &&
	       effendtime <= tsp->endtime )
	    {
	      if ( verbose )
		{
		  mst_srcname (lptrace, srcname);
		  ms_hptime2seedtimestr (rec->starttime, stime);
		  ms_hptime2seedtimestr (rec->endtime, etime);
		  fprintf (stderr, "Removing Record %s (%c) :: %s  %s\n",
			   srcname, rec->quality, stime, etime);
		}
	      
	      rec->flp->recrmcount++;
	      rec->reclen = 0;
	      modcount++;
	    }
	  
	  /* Determine the new start/end times if pruning at the sample level */
	  if ( prunedata == 's' && rec->reclen != 0 )
	    {
	      /* Record overlaps beginning of HP coverage */
	      if ( effstarttime <= hptrace->starttime &&
		   effendtime >= hptrace->starttime )
		{
		  rec->newend = hptrace->starttime - hpdelta;
		  rec->flp->rectrimcount++;
		  modcount++;	  
		}
	      
	      /* Record overlaps end of HP coverage */
	      if ( effstarttime <= hptrace->endtime &&
		   effendtime >= hptrace->endtime )
		{
		  rec->newstart = hptrace->endtime + hpdelta;
		  rec->flp->rectrimcount++;
		  modcount++;
		}
	    }
	  
	  tsp = tsp->next;
	}
      
      rec = rec->next;
    }
  
  /* Free the TimeSegment list */
  tsp = ts;
  while ( tsp )
    {
      ts = tsp;
      tsp = tsp->next;
      
      free (ts);
    }
  
  return modcount;
} /* End of trimtraces() */


/***************************************************************************
 * qcompare:
 *
 * Compare two different quality codes.
 *
 * Returns:
 * -1 = quality1 is greater than quality2
 *  0 = qualities are equal
 *  1 = quality2 is greater than quality1
 ***************************************************************************/
static int
qcompare (const char quality1, const char quality2)
{
  if ( quality1 == quality2 )
    return 0;
  
  if ( quality1 == 'R' && (quality2 == 'D' || quality2 == 'Q') )
    return 1;
  
  if ( quality1 == 'D' && quality2 == 'Q' )
    return 1;
  
  return -1;
} /* End of qcompare() */


/***************************************************************************
 * readfiles:
 *
 * Read input files (global file list) building a MSTraceGroup and
 * record maps for each trace.  All input files are renamed with a
 * ".orig" suffix before being read.
 *
 * Returns 0 on success and 1 otherwise.
 ***************************************************************************/
static int
readfiles (void)
{
  Filelink *flp;
  MSRecord *msr = 0;
  MSTrace *mst = 0;
  int retcode;
  flag whence;
  
  int totalrecs  = 0;
  int totalsamps = 0;
  int totalfiles = 0;
  
  RecordMap *recmap;
  Record *rec;
  
  RecordMap newrecmap;
  Record *newrec;
  
  off_t fpos;
  hptime_t recstarttime;
  hptime_t recendtime;
  
  char basesrc[50];
  char srcname[50];
  char stime[30];
  
  int infilenamelen;
  
  if ( ! filelist )
    return 1;
  
  /* (Re)Initialize MSTraceGroup */
  mstg = reinitgroup (mstg);
  
  /* Read all input files and construct continuous traces, using the
   * libmseed MSTrace and MSTraceGroup functionality.  For each trace
   * maintain a list of each data record that contributed to the
   * trace, implemented as a RecordMap struct (MSTrace->private) where
   * a linked list of Record structs is maintained.  The records are
   * listed in time order.
   */
  
  flp = filelist;
  
  while ( flp != 0 )
    {
      /* Add '.orig' suffix to input file if it will be replaced */
      if ( replaceinput )
	{
	  /* The output file name is the original input file name */
	  flp->outfilename = flp->infilename;
	  
	  infilenamelen = strlen(flp->outfilename) + 6;
	  flp->infilename = (char *) malloc (infilenamelen);
	  snprintf (flp->infilename, infilenamelen, "%s.orig", flp->outfilename);
	  
	  if ( rename (flp->outfilename, flp->infilename) )
	    {
	      fprintf (stderr, "ERROR renaming %s -> %s : '%s'\n",
		       flp->outfilename, flp->infilename, strerror(errno));
	      return 1;
	    }
	}
      
      if ( verbose )
	{
	  if ( replaceinput ) 
	    fprintf (stderr, "Processing: %s (was %s)\n", flp->infilename, flp->outfilename);
	  else
	    fprintf (stderr, "Processing: %s\n", flp->infilename);
	}
      
      /* Loop over the input file */
      while ( (retcode = ms_readmsr (&msr, flp->infilename, reclen, &fpos, NULL, 1, 0, verbose-2))
	      == MS_NOERROR )
	{
	  recstarttime = msr_starttime (msr);
	  recendtime = msr_endtime (msr);
	  
	  /* Generate the srcname and add the quality code */
	  msr_srcname (msr, basesrc);
	  snprintf (srcname, sizeof(srcname), "%s_%c", basesrc, msr->dataquality);
	  
	  /* Generate an ASCII start time string */
	  ms_hptime2seedtimestr (recstarttime, stime);
	  
	  // Prune to sample if pruning...
	  
	  /* Check if record matches start time criteria */
	  if ( (starttime != HPTERROR) && (starttime < recstarttime) )
	    {
	      if ( verbose >= 3 )
		fprintf (stderr, "Skipping (starttime) %s, %s\n", srcname, stime);
	      continue;
	    }
	      
	  /* Check if record matches end time criteria */
	  if ( (endtime != HPTERROR) && (recendtime > endtime) )
	    {
	      if ( verbose >= 3 )
		fprintf (stderr, "Skipping (endtime) %s, %s\n", srcname, stime);
	      continue;
	    }
	  
	  /* Check if record is matched by the match regex */
	  if ( match )
	    {
	      if ( regexec ( match, srcname, 0, 0, 0) != 0 )
		{
		  if ( verbose >= 3 )
		    fprintf (stderr, "Skipping (match) %s, %s\n", srcname, stime);
		  continue;
		}
	    }

	  /* Check if record is rejected by the reject regex */
	  if ( reject )
	    {
	      if ( regexec ( reject, srcname, 0, 0, 0) == 0 )
		{
		  if ( verbose >= 3 )
		    fprintf (stderr, "Skipping (reject) %s, %s\n", srcname, stime);
		  continue;
		}
	    }
	  
	  if ( verbose > 2 )
	    msr_print (msr, verbose - 3);
	  
	  /* Add record to the MSTraceGroup */
	  if ( ! (mst = mst_addmsrtogroup (mstg, msr, bestquality, timetol, sampratetol)) )
	    {
	      fprintf (stderr, "ERROR adding record to trace group, %s, %s\n", srcname, stime);
	    }
	  
	  /* Determine where the record fit this MSTrace
	   * whence:
	   * 0 = New MSTrace
	   * 1 = End of MSTrace
	   * 2 = Beginning of MSTrace
	   */
	  whence = 0;
	  if ( mst->private )
	    {
	      if ( mst->endtime == recendtime )
		whence = 1;
	      else if ( mst->starttime == recstarttime )
		whence = 2;
	      else if ( recendtime == recstarttime )
		{
		  /* Determine best fit for records with no span (not added to the MSTrace coverage) */
		  if ( llabs (recstarttime - mst->endtime) < llabs (recstarttime - mst->starttime) )
		    whence = 1;
		  else
		    whence = 2;
		}
	      else
		{
		  fprintf (stderr, "ERROR determining where record fit relative to trace\n");
		  msr_print (msr, 1);
		  continue;
		}
	    }
	  
	  /* Create and populate new Record structure */
	  rec = (Record *) malloc (sizeof(Record));
	  rec->flp = flp;
	  rec->offset = fpos;
	  rec->reclen = msr->reclen;
	  rec->starttime = recstarttime;
	  rec->endtime = recendtime;
	  rec->quality = msr->dataquality;
	  rec->newstart = 0;
	  rec->newend = 0;
	  rec->prev = 0;
	  rec->next = 0;
	  
	  /* Populate a new record map */
	  newrecmap.recordcnt = 1;
	  newrecmap.fisrt = rec;
	  newrecmap.last = rec;
	  
	  /* If pruning at the sample level trim right at the start/end times */
	  if ( prunedata == 's' )
	    {
	      /* If the Record crosses the start time */
	      if ( starttime != HPTERROR && (starttime > recstarttime) && (starttime < recendtime) )
		{
		  //CHAD, think about this...
		  if ( rec->newstart && rec->newstart < starttime )
		    {
		      fprintf (stderr, "DB: HERE-start\n");
		      rec->newstart = starttime;
		    }
		}
	      /* If the Record crosses the end time */
	      if ( endtime != HPTERROR && (endtime > recstarttime) && (endtime < recendtime) )
		{
		  //CHAD, think about this...
		  if ( rec->newend && rec->newend > endtime )
		    {
		      fprintf (stderr, "DB: HERE-end\n");
		      rec->newend = endtime;
		    }
		}
	    }
	  
	  /* Create extra Record structures if splitting on a time boundary */
	  if ( splitboundary )
	    {
	      BTime startbtime;
	      hptime_t boundary = HPTERROR;
	      hptime_t effstarttime;
	      hptime_t hpdelta = ( msr->samprate ) ? (hptime_t) (HPTMODULUS / msr->samprate) : 0;
	      
	      for (;;)
		{
		  effstarttime = (rec->newstart) ? rec->newstart : rec->starttime;
		  ms_hptime2btime (effstarttime, &startbtime);
		  
		  /* Determine next split boundary */
		  if ( splitboundary == 'd' ) /* Days */
		    {
		      startbtime.day += 1;
		      startbtime.hour = startbtime.min = startbtime.sec = startbtime.fract = 0;
		      boundary = ms_btime2hptime (&startbtime);
		    }
		  else if ( splitboundary == 'h' ) /* Hours */
		    {
		      startbtime.hour += 1;
		      startbtime.min = startbtime.sec = startbtime.fract = 0;
		      boundary = ms_btime2hptime (&startbtime);
		    }
		  else if ( splitboundary == 'm' ) /* Minutes */
		    {
		      startbtime.min += 1;
		      startbtime.sec = startbtime.fract = 0;
		      boundary = ms_btime2hptime (&startbtime);
		    }
		  else
		    {
		      fprintf (stderr, "ERROR split boundary code unrecognized: '%c'\n", splitboundary);
		      break;
		    }
		  
		  /* If end time is beyond the boundary create a new Record */
		  if ( rec->endtime > boundary )
		    {
		      newrec = (Record *) malloc (sizeof(Record));
		      memcpy (newrec, rec, sizeof(Record));
		      
		      /* Set current Record and next Record new boundary times */
		      rec->newend = boundary - hpdelta;
		      newrec->newstart = boundary;
		      
		      /* Update new record map */
		      newrecmap.recordcnt++;
		      newrecmap.last = newrec;
		      
		      /* Insert the new Record in chain and set as current */
		      rec->next = newrec;
		      newrec->prev = rec;
		      rec = newrec;
		      
		      flp->recsplitcount++;
		    }
		  /* Otherwise we are done */
		  else
		    {
		      break;
		    }
		}
	    } /* Done splitting on time boundary */
	  
	  CHAD, merge the newrecmap with the existing recmap or replace, might need to realloc newrecmap...

	  /* Add all Record entries into the RecordMap */
	  rec = recs;
	  while ( rec )
	    {
	      nextrec = rec->next;

	      /* Add record details to end of the RecordMap */
	      if ( whence == 1 )
		{
		  recmap = (RecordMap *) mst->private;
		  
		  rec->prev = recmap->last;
		  rec->next = 0;
		  recmap->last->next = rec;
		  
		  recmap->last = rec;
		  recmap->recordcnt++;
		}
	      /* Add record details to beginning of the RecordMap */
	      else if ( whence == 2 )
		{
		  recmap = (RecordMap *) mst->private;
		  
		  rec->prev = 0;
		  rec->next = recmap->first;
		  recmap->first->prev = rec;
		  
		  recmap->first = rec;
		  recmap->recordcnt++;
		  
		  /* Increment reordered count */
		  flp->reordercount++;
		}
	      /* First record for this MSTrace, allocate RecordMap */
	      else
		{
		  if ( mst->private )
		    fprintf (stderr, "ERROR, supposedly first record, but RecordMap not empty\n");
		  
		  rec->prev = rec->next = 0;
		  recmap = (RecordMap *) malloc (sizeof(RecordMap));
		  recmap->first = rec;
		  recmap->last = rec;
		  recmap->recordcnt = 1;
		  
		  mst->private = recmap;
		}
	      
	      rec = nextrec;
	    }
	  
	  totalrecs++;
	  totalsamps += msr->samplecnt;
	}
      
      if ( retcode != MS_ENDOFFILE )
	fprintf (stderr, "ERROR reading %s: %s\n", flp->infilename, get_errorstr(retcode));
      
      /* Make sure everything is cleaned up */
      ms_readmsr (&msr, NULL, 0, NULL, NULL, 0, 0, 0);
      
      totalfiles++;
      flp = flp->next;
    } /* End of looping over file list */
  
  if ( basicsum )
    printf ("Files: %d, Records: %d, Samples: %d\n", totalfiles, totalrecs, totalsamps);
  
  return 0;
}  /* End of readfiles() */


/***************************************************************************
 * reinitgroup():
 *
 * (Re)Initialize a MSTraceGroup, freeing all associated memory.
 *
 * Return pointer to (re)initialized MSTraceGroup.
 ***************************************************************************/
static MSTraceGroup *
reinitgroup (MSTraceGroup *mstg)
{
  MSTrace *mst;
  RecordMap *recmap;
  Record *rec, *nextrec;
  
  if ( ! mstg )
    {
      return mst_initgroup (mstg);
    }
  
  /* Free all Records from each MSTrace in group */
  mst = mstg->traces;
  
  while ( mst )
    {
      recmap = (RecordMap *) mst->private;
      rec = recmap->first;
      
      while (rec)
	{
	  nextrec = rec->next;
	  free (rec);
	  rec = nextrec;
	}
      
      free (mst->private);
      mst->private = 0;
      
      mst = mst->next;
    }
  
  mst_initgroup (mstg);
  
  return mstg;
}  /* End of reinitgroup() */


/***************************************************************************
 * printmodsummary():
 *
 * Print a summary of modifications to stdout.  If 'nomods' is true
 * include files that were not modified.
 ***************************************************************************/
static void
printmodsummary (flag nomods)
{
  Filelink *flp;
  
  printf ("File modification summary:\n");
  
  flp = filelist;
  
  while ( flp != 0 )
    {
      if ( ! nomods && ! flp->reordercount && ! flp->recrmcount && ! flp->rectrimcount )
	{
	  flp = flp->next;
	  continue;
	}
      
      if ( replaceinput )
	printf (" Records split: %3d trimmed: %3d removed: %3d, Segments reordered: %3d :: %s\n",
		flp->reordercount, flp->recsplitcount, flp->rectrimcount, flp->recrmcount, flp->outfilename);
      else
	printf (" Records split: %3d trimmed: %3d removed: %3d, Segments reordered: %3d :: %s\n",
		flp->reordercount, flp->recsplitcount, flp->rectrimcount, flp->recrmcount, flp->infilename);
      
      flp = flp->next;
    }
  
  return;
}  /* End of printmodsummary() */


/***************************************************************************
 * printtracemap():
 *
 * Print record map for each MSTrace to stdout.
 ***************************************************************************/
static void
printtracemap (MSTraceGroup *mstg)
{
  MSTrace *mst = 0;
  char srcname[50];
  char stime[30];
  char etime[30];
  int tracecnt = 0;
  RecordMap *recmap;
  Record *rec;
  
  if ( ! mstg )
    {
      return;
    }
  
  mst = mstg->traces;
  
  /* Print out the appropriate header */
  fprintf (stderr, "\nMSTrace Map:\n");
  fprintf (stderr, "   Source              Start sample             End sample        Hz   Samples\n");
  
  while ( mst )
    {
      mst_srcname (mst, srcname);
      
      /* Create formatted time strings */
      if ( ms_hptime2seedtimestr (mst->starttime, stime) == NULL )
	fprintf (stderr, "ERROR converting trace start time for %s\n", srcname);
      
      if ( ms_hptime2seedtimestr (mst->endtime, etime) == NULL )
	fprintf (stderr, "ERROR converting trace end time for %s\n", srcname);
      
      /* Print MSTrace header */
      printf ("%-15s %-24s %-24s %-4.4g %-d\n",
	      srcname, stime, etime, mst->samprate, mst->samplecnt);
      
      if ( ! mst->private )
	{
	  fprintf (stderr, "  No record map associated with this MSTrace.\n");
	}
      else
	{
	  recmap = (RecordMap *) mst->private;
	  rec = recmap->first;
	  
	  printf ("Record map contains %lld records:\n", recmap->recordcnt);
	  
	  while ( rec )
	    {
	      ms_hptime2seedtimestr (rec->starttime, stime);
	      ms_hptime2seedtimestr (rec->endtime, etime);
	      
	      printf ("  Filename: %s  Offset: %llu  RecLen: %d\n    Start: %s\n    End: %s\n",
		      rec->flp->infilename, (long long unsigned)rec->offset, rec->reclen, stime, etime);
	      
	      rec = rec->next;
	    }
	}
      
      tracecnt++;
      mst = mst->next;
    }
  
  if ( tracecnt != mstg->numtraces )
    fprintf (stderr, "ERROR printtracemap(): number of traces in trace group is inconsistent\n");
  
  printf ("End of MSTrace Map: %d trace(s)\n\n", tracecnt);
  
}  /* End of printtracemap() */


/***************************************************************************
 * processparam():
 * Process the command line parameters.
 *
 * Returns 0 on success, and -1 on failure
 ***************************************************************************/
static int
processparam (int argcount, char **argvec)
{
  int optind;
  char *matchpattern = 0;
  char *rejectpattern = 0;
  char *tptr;
  
  /* Process all command line arguments */
  for (optind = 1; optind < argcount; optind++)
    {
      if (strcmp (argvec[optind], "-V") == 0)
	{
	  fprintf (stderr, "%s version: %s\n", PACKAGE, VERSION);
	  exit (0);
	}
      else if (strcmp (argvec[optind], "-h") == 0)
	{
	  usage (0);
	  exit (0);
	}
      else if (strcmp (argvec[optind], "-H") == 0)
	{
	  usage (1);
	  exit (0);
	}
      else if (strncmp (argvec[optind], "-v", 2) == 0)
	{
	  verbose += strspn (&argvec[optind][1], "v");
	}
      else if (strcmp (argvec[optind], "-tt") == 0)
	{
	  timetol = strtod (getoptval(argcount, argvec, optind++), NULL);
	}
      else if (strcmp (argvec[optind], "-rt") == 0)
	{
	  sampratetol = strtod (getoptval(argcount, argvec, optind++), NULL);
	}
      else if (strcmp (argvec[optind], "-E") == 0)
	{
	  bestquality = 0;
	}
      else if (strcmp (argvec[optind], "-ts") == 0)
	{
	  starttime = ms_seedtimestr2hptime (getoptval(argcount, argvec, optind++));
	  if ( starttime == HPTERROR )
	    return -1;
	}
      else if (strcmp (argvec[optind], "-te") == 0)
	{
	  endtime = ms_seedtimestr2hptime (getoptval(argcount, argvec, optind++));
	  if ( endtime == HPTERROR )
	    return -1;
	}
      else if (strcmp (argvec[optind], "-m") == 0)
	{
	  matchpattern = getoptval(argcount, argvec, optind++);
	}
      else if (strcmp (argvec[optind], "-r") == 0)
	{
	  rejectpattern = getoptval(argcount, argvec, optind++);
	}
      else if (strcmp (argvec[optind], "-R") == 0)
        {
          replaceinput = 1;
        }
      else if (strcmp (argvec[optind], "-nb") == 0)
        {
          nobackups = 1;
        }
      else if (strcmp (argvec[optind], "-o") == 0)
        {
          outputfile = getoptval(argcount, argvec, optind++);
        }
      else if (strcmp (argvec[optind], "-A") == 0)
        {
          if ( addarchive(getoptval(argcount, argvec, optind++), NULL) == -1 )
            return -1;
        }
      else if (strcmp (argvec[optind], "-Pr") == 0)
	{
	  prunedata = 'r';
	}
      else if (strcmp (argvec[optind], "-Ps") == 0 || strcmp (argvec[optind], "-P") == 0)
	{
	  prunedata = 's';
	}
      else if (strcmp (argvec[optind], "-Sd") == 0)
	{
	  splitboundary = 'd';
	}
      else if (strcmp (argvec[optind], "-Sh") == 0)
	{
	  splitboundary = 'h';
	}
      else if (strcmp (argvec[optind], "-Sm") == 0)
	{
	  splitboundary = 'm';
	}
      else if (strcmp (argvec[optind], "-Q") == 0)
        {
          tptr = getoptval(argcount, argvec, optind++);
          restampqind = *tptr;
          
          if ( ! MS_ISDATAINDICATOR (restampqind) )
            {
              fprintf(stderr, "ERROR Invalid data indicator: '%c'\n", restampqind);
              exit (1);
            }
        }
      else if (strcmp (argvec[optind], "-sum") == 0)
	{
	  basicsum = 1;
	}
      else if (strcmp (argvec[optind], "-mod") == 0)
        {
          modsummary = 1;
        }
      else if (strcmp (argvec[optind], "-POD") == 0)
	{
	  if ( argvec[optind+1] )
	    if ( (optind+1) < argcount && *argvec[optind+1] != '-' )
	      podreqfile = argvec[optind+1];
	  optind++;
	  
	  if ( argvec[optind+1] )
	    if ( (optind+1) < argcount && *argvec[optind+1] != '-' )
	      poddatadir = argvec[optind+1];
	  optind++;
	  
	  if ( ! podreqfile || ! poddatadir )
	    {
	      fprintf (stderr, "ERROR Option -POD requires two values, try -h for usage\n");
	      exit (1);
	    }
        }
      else if (strcmp (argvec[optind], "-CHAN") == 0)
        {
          if ( addarchive(getoptval(argcount, argvec, optind++), CHANLAYOUT) == -1 )
            return -1;
        }
      else if (strcmp (argvec[optind], "-CDAY") == 0)
        {
          if ( addarchive(getoptval(argcount, argvec, optind++), CDAYLAYOUT) == -1 )
            return -1;
        }
      else if (strcmp (argvec[optind], "-BUD") == 0)
        {
          if ( addarchive(getoptval(argcount, argvec, optind++), BUDLAYOUT) == -1 )
            return -1;
        }
      else if (strcmp (argvec[optind], "-CSS") == 0)
        {
          if ( addarchive(getoptval(argcount, argvec, optind++), CSSLAYOUT) == -1 )
            return -1;
        }
      else if (strncmp (argvec[optind], "-", 1) == 0 &&
	       strlen (argvec[optind]) > 1 )
	{
	  fprintf(stderr, "ERROR Unknown option: %s\n", argvec[optind]);
	  exit (1);
	}
      else
	{
	  addfile (argvec[optind], NULL);
	}
    }
  
  /* Cannot specify both input files and POD */
  if ( filelist && (podreqfile && poddatadir) )
    {
      fprintf (stderr, "ERROR Cannot specify both input files and POD structure\n");
      exit (1);
    }
  
  /* Make sure input file(s) or POD were specified */
  if ( filelist == 0 && ! (podreqfile && poddatadir) )
    {
      fprintf (stderr, "No input files were specified\n\n");
      fprintf (stderr, "%s version %s\n\n", PACKAGE, VERSION);
      fprintf (stderr, "Try %s -h for usage\n", PACKAGE);
      exit (1);
    }

  /* Expand match pattern from a file if prefixed by '@' */
  if ( matchpattern )
    {
      if ( *matchpattern == '@' )
	{
	  tptr = matchpattern + 1; /* Skip the @ sign */
	  matchpattern = 0;
	  
	  if ( readregexfile (tptr, &matchpattern) <= 0 )
	    {
	      fprintf (stderr, "ERROR reading match pattern regex file\n");
	      exit (1);
	    }
	}
    }
  
  /* Expand reject pattern from a file if prefixed by '@' */
  if ( rejectpattern )
    {
      if ( *rejectpattern == '@' )
	{
	  tptr = rejectpattern + 1; /* Skip the @ sign */
	  rejectpattern = 0;
	  
	  if ( readregexfile (tptr, &rejectpattern) <= 0 )
	    {
	      fprintf (stderr, "ERROR reading reject pattern regex file\n");
	      exit (1);
	    }
	}
    }
  
  /* Compile match and reject patterns */
  if ( matchpattern )
    {
      match = (regex_t *) malloc (sizeof(regex_t));
      
      if ( regcomp (match, matchpattern, REG_EXTENDED) != 0)
	{
	  fprintf (stderr, "ERROR compiling match regex: '%s'\n", matchpattern);
	}
    }
  
  if ( rejectpattern )
    {
      reject = (regex_t *) malloc (sizeof(regex_t));
      
      if ( regcomp (reject, rejectpattern, REG_EXTENDED) != 0)
	{
	  fprintf (stderr, "ERROR compiling reject regex: '%s'\n", rejectpattern);
	}
    }

  /* Report the program version */
  if ( verbose )
    fprintf (stderr, "%s version: %s\n", PACKAGE, VERSION);
  
  return 0;
}  /* End of processparam() */


/***************************************************************************
 * getoptval:
 * Return the value to a command line option; checking that the value is 
 * itself not an option (starting with '-') and is not past the end of
 * the argument list.
 *
 * argcount: total arguments in argvec
 * argvec: argument list
 * argopt: index of option to process, value is expected to be at argopt+1
 *
 * Returns value on success and exits with error message on failure
 ***************************************************************************/
static char *
getoptval (int argcount, char **argvec, int argopt)
{
  if ( argvec == NULL || argvec[argopt] == NULL ) {
    fprintf (stderr, "ERROR getoptval(): NULL option requested\n");
    exit (1);
    return 0;
  }
  
  /* Special case of '-o -' usage */
  if ( (argopt+1) < argcount && strcmp (argvec[argopt], "-o") == 0 )
    if ( strcmp (argvec[argopt+1], "-") == 0 )
      return argvec[argopt+1];
  
  if ( (argopt+1) < argcount && *argvec[argopt+1] != '-' )
    return argvec[argopt+1];
  
  fprintf (stderr, "ERROR Option %s requires a value, try -h for usage\n", argvec[argopt]);
  exit (1);
  return 0;
}  /* End of getoptval() */


/***************************************************************************
 * addfile:
 *
 * Add file to end of the global file list (filelist).
 ***************************************************************************/
static void
addfile (char *filename, ReqRec *reqrec)
{
  Filelink *lastlp, *newlp;
  
  if ( filename == NULL )
    {
      fprintf (stderr, "ERROR addfile(): No file name specified\n");
      return;
    }
  
  lastlp = filelist;
  while ( lastlp != 0 )
    {
      if ( lastlp->next == 0 )
	break;
      
      lastlp = lastlp->next;
    }
  
  newlp = (Filelink *) malloc (sizeof (Filelink));
  newlp->reqrec = reqrec;
  newlp->infilename = strdup(filename);
  newlp->infp = 0;
  newlp->outfilename = 0;
  newlp->outfp = 0;
  newlp->reordercount = 0;
  newlp->recsplitcount = 0;
  newlp->recrmcount = 0;
  newlp->rectrimcount = 0;
  newlp->earliest = 0;
  newlp->latest = 0;
  newlp->byteswritten = 0;
  newlp->next = 0;
  
  if ( lastlp == 0 )
    filelist = newlp;
  else
    lastlp->next = newlp;
  
}  /* End of addfile() */


/***************************************************************************
 * addarchive:
 * Add entry to the data stream archive chain.  'layout' if defined
 * will be appended to 'path'.
 *
 * Returns 0 on success, and -1 on failure
 ***************************************************************************/
static int
addarchive ( const char *path, const char *layout )
{
  Archive *newarch;
  int pathlayout;
  
  if ( ! path )
    {
      fprintf (stderr, "addarchive: cannot add archive with empty path\n");
      return -1;
    }

  newarch = (Archive *) malloc (sizeof (Archive));
  
  if ( newarch == NULL )
    {
      fprintf (stderr, "addarchive: cannot allocate memory for new archive definition\n");
      return -1;
    }
  
  /* Setup new entry and add it to the front of the chain */
  pathlayout = strlen (path) + 2;
  if ( layout )
    pathlayout += strlen (layout);
  
  newarch->datastream.path = (char *) malloc (pathlayout);
  
  if ( layout )
    snprintf (newarch->datastream.path, pathlayout, "%s/%s", path, layout);
  else
    snprintf (newarch->datastream.path, pathlayout, "%s", path);
  
  newarch->datastream.grouproot = NULL;
  
  if ( newarch->datastream.path == NULL )
    {
      fprintf (stderr, "addarchive: cannot allocate memory for new archive path\n");
      if ( newarch )
        free (newarch);
      return -1;
    }
  
  newarch->next = archiveroot;
  archiveroot = newarch;
  
  return 0;
}  /* End of addarchive() */


/***************************************************************************
 * readregexfile:
 *
 * Read a list of regular expressions from a file and combine them
 * into a single, compound expression which is returned in *pppattern.
 * The return buffer is reallocated as need to hold the growing
 * pattern.  When called *pppattern should not point to any associated
 * memory.
 *
 * Returns the number of regexes parsed from the file or -1 on error.
 ***************************************************************************/
static int
readregexfile (char *regexfile, char **pppattern)
{
  FILE *fp;
  char  line[1024];
  char  linepattern[1024];
  int   regexcnt = 0;
  int   newpatternsize;
  
  /* Open the regex list file */
  if ( (fp = fopen (regexfile, "rb")) == NULL )
    {
      fprintf (stderr, "ERROR opening regex list file %s: %s\n",
	       regexfile, strerror (errno));
      return -1;
    }
  
  if ( verbose )
    fprintf (stderr, "Reading regex list from %s\n", regexfile);
  
  *pppattern = NULL;
  
  while ( (fgets (line, sizeof(line), fp)) !=  NULL)
    {
      /* Trim spaces and skip if empty lines */
      if ( sscanf (line, " %s ", linepattern) != 1 )
	continue;
      
      /* Skip comment lines */
      if ( *linepattern == '#' )
	continue;
      
      regexcnt++;
      
      /* Add regex to compound regex */
      if ( *pppattern )
	{
	  newpatternsize = strlen(*pppattern) + strlen(linepattern) + 4;
	  *pppattern = realloc (*pppattern, newpatternsize);	  
	  snprintf (*pppattern, newpatternsize, "%s|(%s)", *pppattern, linepattern);
	}
      else
	{
	  newpatternsize = strlen(linepattern) + 3;
	  *pppattern = realloc (*pppattern, newpatternsize);
	  snprintf (*pppattern, newpatternsize, "(%s)", linepattern);
	}
    }
  
  fclose (fp);
  
  return regexcnt;
}  /* End readregexfile() */


/***************************************************************************
 * freefilelist:
 *
 * Free all memory assocated with global file list.
 ***************************************************************************/
static void
freefilelist (void)
{
  Filelink *flp, *nextflp;
   
  flp = filelist;

  while ( flp )
    {
      nextflp = flp->next;
      
      if ( flp->infilename )
	free (flp->infilename);
      if ( flp->outfilename )
	free (flp->outfilename);
      
      free (flp);
      
      flp = nextflp;
    }

  filelist = 0;
  
  return;
}  /* End of freefilelist() */


/***************************************************************************
 * strparse:
 *
 * splits a 'string' on 'delim' and puts each part into a linked list
 * pointed to by 'list' (a pointer to a pointer).  The last entry has
 * it's 'next' set to 0.  All elements are NULL terminated strings.
 * If both 'string' and 'delim' are NULL then the linked list is
 * traversed and the memory used is free'd and the list pointer is
 * set to NULL.
 *
 * Returns the number of elements added to the list, or 0 when freeing
 * the linked list.
 ***************************************************************************/
static int
strparse (const char *string, const char *delim, StrList **list)
{
  const char *beg;			/* beginning of element */
  const char *del;			/* delimiter */
  int stop = 0;
  int count = 0;
  int total;

  StrList *curlist = 0;
  StrList *tmplist = 0;

  if (string != NULL && delim != NULL)
    {
      total = strlen (string);
      beg = string;

      while (!stop)
	{

	  /* Find delimiter */
	  del = strstr (beg, delim);

	  /* Delimiter not found or empty */
	  if (del == NULL || strlen (delim) == 0)
	    {
	      del = string + strlen (string);
	      stop = 1;
	    }

	  tmplist = (StrList *) malloc (sizeof (StrList));
	  tmplist->next = 0;

	  tmplist->element = (char *) malloc (del - beg + 1);
	  strncpy (tmplist->element, beg, (del - beg));
	  tmplist->element[(del - beg)] = '\0';

	  /* Add this to the list */
	  if (count++ == 0)
	    {
	      curlist = tmplist;
	      *list = curlist;
	    }
	  else
	    {
	      curlist->next = tmplist;
	      curlist = curlist->next;
	    }

	  /* Update 'beg' */
	  beg = (del + strlen (delim));
	  if ((beg - string) > total)
	    break;
	}

      return count;
    }
  else
    {
      curlist = *list;
      while (curlist != NULL)
	{
	  tmplist = curlist->next;
	  free (curlist->element);
	  free (curlist);
	  curlist = tmplist;
	}
      *list = NULL;

      return 0;
    }
}  /* End of strparse() */


/***************************************************************************
 * usage():
 * Print the usage message.
 ***************************************************************************/
static void
usage (int level)
{
  fprintf (stderr, "%s - select, sort and prune Mini-SEED: %s\n\n", PACKAGE, VERSION);
  fprintf (stderr, "Usage: %s [options] file1 [file2] [file3] ...\n\n", PACKAGE);
  fprintf (stderr,
	   " ## Options ##\n"
	   " -V           Report program version\n"
	   " -h           Show this usage message\n"
	   " -H           Show usage message with 'format' details (see -A option)\n"
	   " -v           Be more verbose, multiple flags can be used\n"
	   " -tt secs     Specify a time tolerance for continuous traces\n"
	   " -rt diff     Specify a sample rate tolerance for continuous traces\n"
	   " -E           Consider all qualities equal instead of 'best' prioritization\n"
	   "\n"
	   " ## Data selection options ##\n"
	   " -ts time     Limit to records that start after time\n"
	   " -te time     Limit to records that end before time\n"
	   "                time format: 'YYYY[,DDD,HH,MM,SS,FFFFFF]' delimiters: [,:.]\n"
	   " -m match     Limit to records matching the specified regular expression\n"
	   " -r reject    Limit to records not matchint the specfied regular expression\n"
	   "                Regular expressions are applied to: 'NET_STA_LOC_CHAN_QUAL'\n"
	   "\n"
	   " ## Output options ##\n"
	   " -R           Replace input files, default leaves .orig files\n"
	   " -nb          Do not keep backups of original input files if replacing them\n"
	   " -o file      Specify a single output file\n"
	   " -A format    Write all records is a custom directory/file layout (try -H)\n"
	   " -Pr          Prune data at the record level using 'best' quality priority\n"
	   " -Ps          Prune data at the sample level using 'best' quality priority\n"
	   " -S[dhm]      Split records on day, hour or minute boundaries\n"
	   " -Q DRQ       Re-stamp output data records with specified quality: D, R or Q\n"
           "\n"
	   " ## Diagnostic output ##\n"
	   " -sum         Print a basic summary after reading all input files\n"
	   " -mod         Print summary of file modifications after processing\n"
	   "\n"
	   " ## Input data ##\n"
	   " -POD reqfile datadir\n"
	   "              Prune data from a POD structure\n"
	   " file#        Files(s) of Mini-SEED records\n"
	   "\n");

  if  ( level )
    {
      fprintf (stderr,
               "\n"
	       "  # Preset format layouts #\n"
	       " -CHAN dir    Write all records into separate Net.Sta.Loc.Chan files\n"
	       " -CDAY dir    Write all records into separate Net.Sta.Loc.Chan-day files\n"
	       " -BUD BUDdir  Write all records in a BUD file layout\n"
	       " -CSS CSSdir  Write all records in a CSS-like file layout\n"
	       "\n"
               "The archive 'format' argument is expanded for each record using the\n"
               "following flags:\n"
               "\n"
               "  n : network code, white space removed\n"
               "  s : station code, white space removed\n"
               "  l : location code, white space removed\n"
               "  c : channel code, white space removed\n"
               "  Y : year, 4 digits\n"
               "  y : year, 2 digits zero padded\n"
               "  j : day of year, 3 digits zero padded\n"
               "  H : hour, 2 digits zero padded\n"
               "  M : minute, 2 digits zero padded\n"
               "  S : second, 2 digits zero padded\n"
               "  F : fractional seconds, 4 digits zero padded\n"
               "  q : single character record quality indicator (D, R, Q)\n"
               "  L : data record length in bytes\n"
               "  r : Sample rate (Hz) as a rounded integer\n"
               "  R : Sample rate (Hz) as a float with 6 digit precision\n"
               "  %% : the percent (%%) character\n"
               "  # : the number (#) character\n"
               "\n"
               "The flags are prefaced with either the %% or # modifier.  The %% modifier\n"
               "indicates a defining flag while the # indicates a non-defining flag.\n"
               "All records with the same set of defining flags will be written to the\n"
               "same file. Non-defining flags will be expanded using the values in the\n"
               "first record for the resulting file name.\n"
               "\n");
    }
}  /* End of usage() */
