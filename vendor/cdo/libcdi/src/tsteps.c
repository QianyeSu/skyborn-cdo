/* DO NOT REMOVE the config.h include file under any circumstances,
 * it's very much needed on some platforms */
#if defined(HAVE_CONFIG_H)
#include "config.h"
#endif
/* DO NOT REMOVE the above config.h include file under any
 * circumstances as long as it's the autoconf configuration header
 * used to build this package. When it's missing on some platforms,
 * some poor person has to do long, tedious debugging sessions, where
 * struct offsets almost imperceptibly change from one file to the
 * next to find out what happened */

#include <limits.h>
#include <string.h>

#include "cdi.h"
#include "cdi_int.h"
#include "dmemory.h"

static void
tstepsInitEntry(tsteps_t *tstep)
{
  tstep->recIDs = NULL;
  tstep->recinfo = NULL;
  tstep->records = NULL;
  tstep->recordSize = 0;
  tstep->nrecs = 0;
  tstep->curRecID = CDI_UNDEFID;
  tstep->ncStepIndex = 0;
  tstep->position = 0;
  tstep->nallrecs = 0;
  tstep->next = 0;

  ptaxisInit(&(tstep->taxis));
}

int tstepsNewEntry(stream_t *streamptr)
{
  int tsID = streamptr->tstepsNextID++;
  int tstepsTableSize = streamptr->tstepsTableSize;
  tsteps_t *tstepsTable = streamptr->tsteps;

  // If the table overflows, double its size.
  if (tsID == tstepsTableSize)
  {
    if (tstepsTableSize == 0)
      tstepsTableSize = 1;
    if (tstepsTableSize <= INT_MAX / 2)
      tstepsTableSize *= 2;
    else if (tstepsTableSize < INT_MAX)
      tstepsTableSize = INT_MAX;
    else
      Error("Resizing of tstep table failed!");

    tstepsTable = (tsteps_t *)Realloc(tstepsTable, (size_t)tstepsTableSize * sizeof(tsteps_t));
  }

  streamptr->tstepsTableSize = tstepsTableSize;
  streamptr->tsteps = tstepsTable;

  tsteps_t *curTstep = &streamptr->tsteps[tsID];
  tstepsInitEntry(curTstep);

  return tsID;
}

void cdi_create_timesteps(size_t numTimesteps, stream_t *streamptr)
{
  streamptr->ntsteps = (long)numTimesteps;
  if (streamptr->tstepsTableSize > 0)
    return;

  size_t ntsteps = (numTimesteps == 0) ? 1 : numTimesteps;

  streamptr->tsteps = (tsteps_t *)Malloc(ntsteps * sizeof(tsteps_t));
  memset(streamptr->tsteps, 0, ntsteps * sizeof(tsteps_t));

  streamptr->tstepsTableSize = (int)ntsteps;
  streamptr->tstepsNextID = (int)ntsteps;

  for (size_t tsID = 0; tsID < ntsteps; tsID++)
  {
    tstepsInitEntry(&streamptr->tsteps[tsID]);
  }
}
/*
 * Local Variables:
 * c-file-style: "Java"
 * c-basic-offset: 2
 * indent-tabs-mode: nil
 * show-trailing-whitespace: t
 * require-trailing-newline: t
 * End:
 */
