#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "cdi.h"

#ifdef HAVE_LIBNETCDF

#include <ctype.h>
#include <limits.h>

#include "dmemory.h"
#include "cdi_int.h"
#include "cdi_uuid.h"
#include "stream_cdf.h"
#include "cdf_int.h"
#include "varscan.h"
#include "vlist.h"
#include "cdf_util.h"
#include "cdf_lazy_grid.h"
#include "cdf_filter.h"

// On Windows, define strcasecmp manually
#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <unistd.h>
#endif

enum VarStatus
{
  UndefVar = -1,
  CoordVar = 0,
  DataVar = 1,
};

enum AxisType
{
  X_AXIS = 1,
  Y_AXIS = 2,
  Z_AXIS = 3,
  E_AXIS = 4,
  T_AXIS = 5,
};

static int axisTypeChar[] = {'?', 'X', 'Y', 'Z', 'E', 'T'};

typedef struct
{
  int dimid;   // NetCDF dim ID
  int ncvarid; // NetCDF var ID
  int dimtype; // AxisType
  size_t len;  // Dimension size
  char name[CDI_MAX_NAME];
} ncdim_t;

#define MAX_COORDVARS 5
#define MAX_AUXVARS 4
#define MAX_DIMS_CDF 8

typedef struct
{
  int cdiVarID;
  int ncid;
  int varStatus;
  bool ignoreVar;
  bool isLonLatMapping;
  bool isHealpixMapping;
  bool isCubeSphere;
  bool isCharAxis;
  bool isIndexAxis;
  bool isXaxis;
  bool isYaxis;
  bool isZaxis;
  bool isTaxis;
  bool isLon;
  bool isLat;
  bool isClimatology;
  bool hasCalendar;
  bool hasFormulaterms;
  bool printWarning;
  int timetype;
  int param;
  int code;
  int tabnum;
  int bounds;
  int gridID;
  int zaxisID;
  int gridtype;
  int zaxistype;
  int xdim;
  int ydim;
  int zdim;
  int xvarid;
  int yvarid;
  int rpvarid;
  int zvarid;
  int tvarid;
  int ivarid;
  int psvarid;
  int p0varid;
  int ncoordvars;
  int cvarids[MAX_COORDVARS];
  int coordvarids[MAX_COORDVARS];
  int auxvarids[MAX_AUXVARS];
  int nauxvars;
  int cellarea;
  int tableID;
  int truncation;
  int position;
  int numLPE;
  bool missvalDefined;
  bool fillvalDefined;
  int xtype;
  int gmapid;
  int positive;
  int ndims;
  int dimids[MAX_DIMS_CDF];   // Netcdf dimension IDs
  int dimtypes[MAX_DIMS_CDF]; // AxisType
  size_t chunks[MAX_DIMS_CDF];
  bool isChunked;
  int chunkType;
  int chunkSize;
  size_t chunkCacheSize;
  size_t chunkCacheNelems;
  float chunkCachePreemption;
  size_t gridSize;
  size_t xSize;
  size_t ySize;
  size_t zSize;
  int nattsNC;
  int natts;
  int *atts;
  size_t vctsize;
  double *vct;
  double missval;
  double fillval;
  double addoffset;
  double scalefactor;
  bool hasFilter;
  bool hasDeflate;
  bool hasSzip;
  bool isUnsigned;
  bool validrangeDefined;
  double validrange[2];
  int typeOfEnsembleForecast;
  int numberOfForecastsInEnsemble;
  int perturbationNumber;
  int unitsLen;
  char name[CDI_MAX_NAME];
  char longname[CDI_MAX_NAME];
  char stdname[CDI_MAX_NAME];
  char units[CDI_MAX_NAME];
  char filterSpec[CDI_MAX_NAME];
} ncvar_t;

typedef struct
{
  char gridfile[8912];
  unsigned char uuid[CDI_UUID_SIZE];
  int number_of_grid_used;
  int timedimid;
} GridInfo;

static CdiDateTime
scan_time_string(const char *ptu)
{
  int year = 0, month = 0, day = 0;
  int hour = 0, minute = 0;
  double fseconds = 0.0;
  char ch = ' ';

  if (*ptu)
    sscanf(ptu, "%d-%d-%d%c%d:%d:%lf", &year, &month, &day, &ch, &hour, &minute, &fseconds);

  if (day > 999 && year < 32)
  {
    int tmp = year;
    year = day;
    day = tmp;
  }

  int second = (int)fseconds;
  double aseconds;
  double ms = modf(fseconds, &aseconds) * 1000;
  assert((int)aseconds == second);
  CdiDateTime datetime;
  datetime.date.year = year;
  datetime.date.month = (short)month;
  datetime.date.day = (short)day;
  datetime.time.hour = (short)hour;
  datetime.time.minute = (short)minute;
  datetime.time.second = (short)second;
  datetime.time.ms = (short)ms;

  return datetime;
}

static int
scan_time_units(const char *unitstr)
{
  int timeunit = get_time_units(strlen(unitstr), unitstr);
  if (timeunit == -1)
    Warning("Unsupported TIMEUNIT: %s!", unitstr);
  return timeunit;
}

static int
set_base_time(const char *timeUnitsStr, taxis_t *taxis)
{
  int taxisType = TAXIS_ABSOLUTE;

  size_t len = strlen(timeUnitsStr);
  while (isspace(*timeUnitsStr) && len)
  {
    timeUnitsStr++;
    len--;
  }
  char tmp[32], *tu = tmp;
  if (len + 1 > sizeof(tmp))
    tu = (char *)Malloc(len + 1);

  for (size_t i = 0; i < len; i++)
    tu[i] = (char)tolower((int)timeUnitsStr[i]);
  tu[len] = 0;

  int timeUnits = get_time_units(len, tu);
  if (timeUnits == -1)
  {
    Warning("Unsupported TIMEUNIT: %s!", timeUnitsStr);
    if (tu != tmp)
      Free(tu);
    return 1;
  }

  size_t pos = 0;
  while (pos < len && !isspace(tu[pos]))
    ++pos;
  if (tu[pos])
  {
    while (isspace(tu[pos]))
      ++pos;

    if (strStartsWith(tu + pos, "since"))
      taxisType = TAXIS_RELATIVE;

    while (pos < len && !isspace(tu[pos]))
      ++pos;
    if (tu[pos])
    {
      while (isspace(tu[pos]))
        ++pos;

      if (taxisType == TAXIS_ABSOLUTE)
      {
        if (timeUnits == TUNIT_DAY)
        {
          if (!strStartsWith(tu + pos, "%y%m%d.%f"))
          {
            Warning("Unsupported format %s for TIMEUNIT day!", tu + pos);
            timeUnits = -1;
          }
        }
        else if (timeUnits == TUNIT_MONTH)
        {
          if (!strStartsWith(tu + pos, "%y%m.%f"))
          {
            Warning("Unsupported format %s for TIMEUNIT month!", tu + pos);
            timeUnits = -1;
          }
        }
        else if (timeUnits == TUNIT_YEAR)
        {
          if (!strStartsWith(tu + pos, "%y.%f"))
          {
            Warning("Unsupported format %s for TIMEUNIT year!", tu + pos);
            timeUnits = -1;
          }
        }
        else
        {
          Warning("Unsupported format for time units: %s!", tu);
        }
      }
      else if (taxisType == TAXIS_RELATIVE)
      {
        taxis->rDateTime = scan_time_string(tu + pos);
        if (CDI_Debug)
          Message("rdate = %d  rtime = %d", (int)cdiDate_get(taxis->rDateTime.date), cdiTime_get(taxis->rDateTime.time));
      }
    }
  }

  taxis->type = taxisType;
  taxis->unit = timeUnits;

  if (tu != tmp)
    Free(tu);

  if (CDI_Debug)
    Message("taxisType = %d  timeUnits = %d", taxisType, timeUnits);

  return 0;
}

bool xtypeIsText(int xtype)
{
  bool isText = (xtype == NC_CHAR) || (xtype == NC_STRING);
  return isText;
}

static bool
xtypeIsFloat(nc_type xtype)
{
  return (xtype == NC_FLOAT || xtype == NC_DOUBLE);
}

static bool
xtypeIsInt(nc_type xtype)
{
  bool isInt = xtype == NC_SHORT || xtype == NC_INT || xtype == NC_BYTE || xtype == NC_USHORT || xtype == NC_UINT || xtype == NC_UBYTE;
  return isInt;
}

static bool
xtypeIsInt64(nc_type xtype)
{
  return (xtype == NC_INT64 || xtype == NC_UINT64);
}

static int
cdfInqDatatype(stream_t *streamptr, int xtype, bool isUnsigned)
{
  int datatype = -1;

  if (xtype == NC_BYTE && isUnsigned)
    xtype = NC_UBYTE;

  // clang-format off
  if      (xtype == NC_BYTE  )  datatype = CDI_DATATYPE_INT8;
  else if (xtype == NC_CHAR  )  datatype = CDI_DATATYPE_UINT8;
  else if (xtype == NC_SHORT )  datatype = CDI_DATATYPE_INT16;
  else if (xtype == NC_INT   )  datatype = CDI_DATATYPE_INT32;
  else if (xtype == NC_FLOAT )  datatype = CDI_DATATYPE_FLT32;
  else if (xtype == NC_DOUBLE)  datatype = CDI_DATATYPE_FLT64;
  else if (xtype == NC_UBYTE )  datatype = CDI_DATATYPE_UINT8;
  else if (xtype == NC_LONG  )  datatype = CDI_DATATYPE_INT32;
  else if (xtype == NC_USHORT)  datatype = CDI_DATATYPE_UINT16;
  else if (xtype == NC_UINT  )  datatype = CDI_DATATYPE_UINT32;
  else if (xtype == NC_INT64 )  datatype = CDI_DATATYPE_FLT64;
  else if (xtype == NC_UINT64)  datatype = CDI_DATATYPE_FLT64;
  else
  {
    CdfInfo *cdfInfo = &(streamptr->cdfInfo);
    if (xtype != cdfInfo->complexFloatId && xtype != cdfInfo->complexDoubleId)
    {
      bool isUserDefinedType = false;
#ifdef NC_FIRSTUSERTYPEID
      isUserDefinedType = (xtype >= NC_FIRSTUSERTYPEID);
#endif
      if (isUserDefinedType)
      {
        int fileID = streamptr->fileID;
        size_t nfields = 0, compoundsize = 0;
        int status = nc_inq_compound(fileID, xtype, NULL, &compoundsize, &nfields);
        if (status == NC_NOERR && nfields == 2 && (compoundsize == 8 || compoundsize == 16))
        {
          nc_type field_type1 = -1, field_type2 = -1;
          int field_dims1 = 0, field_dims2 = 0;
          nc_inq_compound_field(fileID, xtype, 0, NULL, NULL, &field_type1, &field_dims1, NULL);
          nc_inq_compound_field(fileID, xtype, 1, NULL, NULL, &field_type2, &field_dims2, NULL);
          if (field_type1 == field_type2 && field_dims1 == 0 && field_dims2 == 0)
          {
            if      (field_type1 == NC_FLOAT)  cdfInfo->complexFloatId = xtype;
            else if (field_type1 == NC_DOUBLE) cdfInfo->complexDoubleId = xtype;
          }
        }
      }
    }
    if      (xtype == cdfInfo->complexFloatId )  datatype = CDI_DATATYPE_CPX32;
    else if (xtype == cdfInfo->complexDoubleId)  datatype = CDI_DATATYPE_CPX64;
  }
  // clang-format on

  return datatype;
}

static void
cdfGetAttInt(int fileID, int ncvarid, const char *attname, size_t attlen, int *attint)
{
  *attint = 0;

  nc_type atttype;
  size_t nc_attlen;
  cdf_inq_atttype(fileID, ncvarid, attname, &atttype);
  cdf_inq_attlen(fileID, ncvarid, attname, &nc_attlen);

  if (xtypeIsFloat(atttype) || xtypeIsInt(atttype))
  {
    bool needAlloc = (nc_attlen > attlen);
    int *pintatt = needAlloc ? (int *)Malloc(nc_attlen * sizeof(int)) : attint;
    cdf_get_att_int(fileID, ncvarid, attname, pintatt);
    if (needAlloc)
    {
      memcpy(attint, pintatt, attlen * sizeof(int));
      Free(pintatt);
    }
  }
}

static void
cdfGetAttInt64(int fileID, int ncvarid, const char *attname, size_t attlen, int64_t *attint)
{
  *attint = 0;

  nc_type atttype;
  size_t nc_attlen;
  cdf_inq_atttype(fileID, ncvarid, attname, &atttype);
  cdf_inq_attlen(fileID, ncvarid, attname, &nc_attlen);

  if (xtypeIsFloat(atttype) || xtypeIsInt(atttype) || xtypeIsInt64(atttype))
  {
    long long *plongatt = (long long *)Malloc(nc_attlen * sizeof(long long));
    cdf_get_att_longlong(fileID, ncvarid, attname, plongatt);
    for (size_t i = 0; i < attlen; ++i)
      attint[i] = plongatt[i];
    Free(plongatt);
  }
}

static void
cdfGetAttDouble(int fileID, int ncvarid, char *attname, size_t attlen, double *attdouble)
{
  *attdouble = 0.0;

  nc_type atttype;
  size_t nc_attlen;
  cdf_inq_atttype(fileID, ncvarid, attname, &atttype);
  cdf_inq_attlen(fileID, ncvarid, attname, &nc_attlen);

  if (xtypeIsFloat(atttype) || xtypeIsInt(atttype))
  {
    bool needAlloc = (nc_attlen > attlen);
    double *pdoubleatt = needAlloc ? (double *)Malloc(nc_attlen * sizeof(double)) : attdouble;
    cdf_get_att_double(fileID, ncvarid, attname, pdoubleatt);
    if (needAlloc)
    {
      memcpy(attdouble, pdoubleatt, attlen * sizeof(double));
      Free(pdoubleatt);
    }
  }
}

static bool
cdfCheckAttInt(int fileID, int ncvarid, const char *attname)
{
  nc_type atttype;
  int status_nc = nc_inq_atttype(fileID, ncvarid, attname, &atttype);

  return (status_nc == NC_NOERR && xtypeIsInt(atttype));
}

static bool
cdfCheckAttText(int fileID, int ncvarid, const char *attname)
{
  nc_type atttype;
  int status_nc = nc_inq_atttype(fileID, ncvarid, attname, &atttype);
  return (status_nc == NC_NOERR && (atttype == NC_CHAR || atttype == NC_STRING));
}

static void
cdfGetAttText(int fileID, int ncvarid, const char *attname, size_t attlen, char *atttext)
{
  atttext[0] = 0;

  nc_type atttype;
  size_t nc_attlen;
  cdf_inq_atttype(fileID, ncvarid, attname, &atttype);
  cdf_inq_attlen(fileID, ncvarid, attname, &nc_attlen);

  if (atttype == NC_CHAR)
  {
    char attbuf[65636];
    if (nc_attlen < sizeof(attbuf))
    {
      cdf_get_att_text(fileID, ncvarid, attname, attbuf);

      if (nc_attlen > (attlen - 1))
        nc_attlen = (attlen - 1);

      attbuf[nc_attlen++] = 0;
      memcpy(atttext, attbuf, nc_attlen);
    }
  }
  else if (atttype == NC_STRING)
  {
    if (nc_attlen == 1)
    {
      char *attbuf = NULL;
      cdf_get_att_string(fileID, ncvarid, attname, &attbuf);

      size_t ssize = strlen(attbuf) + 1;
      if (ssize > attlen)
        ssize = attlen;
      memcpy(atttext, attbuf, ssize);
      atttext[ssize - 1] = 0;
      Free(attbuf);
    }
  }
}

void cdf_scale_add(size_t size, double *data, double addoffset, double scalefactor)
{
  bool haveAddoffset = IS_NOT_EQUAL(addoffset, 0.0);
  bool haveScalefactor = IS_NOT_EQUAL(scalefactor, 1.0);

  if (haveAddoffset && haveScalefactor)
  {
    for (size_t i = 0; i < size; ++i)
      data[i] = data[i] * scalefactor + addoffset;
  }
  else if (haveScalefactor)
  {
    for (size_t i = 0; i < size; ++i)
      data[i] *= scalefactor;
  }
  else if (haveAddoffset)
  {
    for (size_t i = 0; i < size; ++i)
      data[i] += addoffset;
  }
}

static int
cdf_time_dimid(int fileID, int ndims, ncdim_t *ncdims, int nvars, ncvar_t *ncvars)
{
  char dimname[CDI_MAX_NAME];
  for (int dimid = 0; dimid < ndims; ++dimid)
  {
    strcpy(dimname, ncdims[dimid].name);
    if (str_is_equal("time", str_to_lower(dimname)))
      return dimid;
  }

  bool check_dimids[MAX_DIMS_CDF];
  for (int i = 0; i < MAX_DIMS_CDF; ++i)
    check_dimids[i] = false;

  for (int varid = 0; varid < nvars; ++varid)
  {
    ncvar_t *ncvar = &ncvars[varid];
    if (ncvars[varid].ndims == 1)
    {
      int dimid0 = CDI_UNDEFID;
      for (int gdimid = 0; gdimid < ndims; ++gdimid)
        if (ncdims[gdimid].dimid == ncvar->dimids[0])
        {
          dimid0 = gdimid;
          break;
        }

      if (dimid0 != CDI_UNDEFID && check_dimids[dimid0] == false)
      {
        if (ncdims[dimid0].ncvarid != CDI_UNDEFID && ncdims[dimid0].ncvarid != varid)
          continue;
        check_dimids[dimid0] = true;

        char sbuf[CDI_MAX_NAME];
        for (int iatt = 0, n = ncvar->nattsNC; iatt < n; ++iatt)
        {
          sbuf[0] = 0;
          cdf_inq_attname(fileID, varid, iatt, sbuf);
          if (str_is_equal(sbuf, "units"))
          {
            cdfGetAttText(fileID, varid, "units", sizeof(sbuf), sbuf);
            if (is_time_units(str_to_lower(sbuf)))
              return dimid0;
          }
        }
      }
    }
  }

  return CDI_UNDEFID;
}

static void
init_ncdims(int ndims, ncdim_t *ncdims)
{
  for (int gdimid = 0; gdimid < ndims; gdimid++)
  {
    ncdim_t *ncdim = &ncdims[gdimid];
    ncdim->dimid = CDI_UNDEFID;
    ncdim->ncvarid = CDI_UNDEFID;
    ncdim->dimtype = CDI_UNDEFID;
    ncdim->len = 0;
    ncdim->name[0] = 0;
  }
}

static void
init_ncvars(int nvars, ncvar_t *ncvars, int ncid)
{
  for (int varid = 0; varid < nvars; varid++)
  {
    ncvar_t *ncvar = &ncvars[varid];
    ncvar->cdiVarID = CDI_UNDEFID;
    ncvar->ncid = ncid;
    ncvar->varStatus = UndefVar;
    ncvar->ignoreVar = false;
    ncvar->isLonLatMapping = false;
    ncvar->isHealpixMapping = false;
    ncvar->isCubeSphere = false;
    ncvar->isIndexAxis = false;
    ncvar->isXaxis = false;
    ncvar->isYaxis = false;
    ncvar->isZaxis = false;
    ncvar->isTaxis = false;
    ncvar->isLon = false;
    ncvar->isLat = false;
    ncvar->isClimatology = false;
    ncvar->hasCalendar = false;
    ncvar->hasFormulaterms = false;
    ncvar->printWarning = true;
    ncvar->timetype = TIME_CONSTANT;
    ncvar->param = CDI_UNDEFID;
    ncvar->code = CDI_UNDEFID;
    ncvar->tabnum = 0;
    ncvar->bounds = CDI_UNDEFID;
    ncvar->gridID = CDI_UNDEFID;
    ncvar->zaxisID = CDI_UNDEFID;
    ncvar->gridtype = CDI_UNDEFID;
    ncvar->zaxistype = CDI_UNDEFID;
    ncvar->xdim = CDI_UNDEFID;
    ncvar->ydim = CDI_UNDEFID;
    ncvar->zdim = CDI_UNDEFID;
    ncvar->xvarid = CDI_UNDEFID;
    ncvar->yvarid = CDI_UNDEFID;
    ncvar->rpvarid = CDI_UNDEFID;
    ncvar->zvarid = CDI_UNDEFID;
    ncvar->tvarid = CDI_UNDEFID;
    ncvar->ivarid = CDI_UNDEFID;
    ncvar->psvarid = CDI_UNDEFID;
    ncvar->p0varid = CDI_UNDEFID;
    ncvar->ncoordvars = 0;
    for (int i = 0; i < MAX_COORDVARS; ++i)
      ncvar->cvarids[i] = CDI_UNDEFID;
    for (int i = 0; i < MAX_COORDVARS; ++i)
      ncvar->coordvarids[i] = CDI_UNDEFID;
    for (int i = 0; i < MAX_AUXVARS; ++i)
      ncvar->auxvarids[i] = CDI_UNDEFID;
    ncvar->nauxvars = 0;
    ncvar->cellarea = CDI_UNDEFID;
    ncvar->tableID = CDI_UNDEFID;
    ncvar->truncation = 0;
    ncvar->position = 0;
    ncvar->numLPE = 0;
    ncvar->missvalDefined = false;
    ncvar->fillvalDefined = false;
    ncvar->xtype = 0;
    ncvar->gmapid = CDI_UNDEFID;
    ncvar->positive = 0;
    ncvar->ndims = 0;
    for (int i = 0; i < MAX_DIMS_CDF; ++i)
      ncvar->dimids[i] = CDI_UNDEFID;
    for (int i = 0; i < MAX_DIMS_CDF; ++i)
      ncvar->dimtypes[i] = CDI_UNDEFID;
    for (int i = 0; i < MAX_DIMS_CDF; ++i)
      ncvar->chunks[i] = 0;
    ncvar->isChunked = false;
    ncvar->chunkType = CDI_UNDEFID;
    ncvar->chunkSize = CDI_UNDEFID;
    ncvar->chunkCacheSize = 0;
    ncvar->chunkCacheNelems = 0;
    ncvar->chunkCachePreemption = 0.0;
    ncvar->gridSize = 0;
    ncvar->xSize = 0;
    ncvar->ySize = 0;
    ncvar->zSize = 0;
    ncvar->nattsNC = 0;
    ncvar->natts = 0;
    ncvar->atts = NULL;
    ncvar->vctsize = 0;
    ncvar->vct = NULL;
    ncvar->missval = 0;
    ncvar->fillval = 0;
    ncvar->addoffset = 0.0;
    ncvar->scalefactor = 1.0;
    ncvar->hasFilter = false;
    ncvar->hasDeflate = false;
    ncvar->hasSzip = false;
    ncvar->isUnsigned = false;
    ncvar->validrangeDefined = false;
    ncvar->validrange[0] = VALIDMISS;
    ncvar->validrange[1] = VALIDMISS;
    ncvar->typeOfEnsembleForecast = -1;
    ncvar->numberOfForecastsInEnsemble = -1;
    ncvar->perturbationNumber = -1;
    ncvar->unitsLen = 0;
    memset(ncvar->name, 0, CDI_MAX_NAME);
    memset(ncvar->longname, 0, CDI_MAX_NAME);
    memset(ncvar->stdname, 0, CDI_MAX_NAME);
    memset(ncvar->units, 0, CDI_MAX_NAME);
    memset(ncvar->filterSpec, 0, CDI_MAX_NAME);
  }
}

static void
cdf_set_var(ncvar_t *ncvar, int varStatus)
{
  if (ncvar->varStatus != UndefVar && ncvar->varStatus != varStatus && ncvar->printWarning)
  {
    if (!ncvar->ignoreVar)
      Warning("Inconsistent variable definition for %s!", ncvar->name);

    ncvar->printWarning = false;
    varStatus = CoordVar;
  }

  ncvar->varStatus = varStatus;
}

static void
cdf_set_dim(ncvar_t *ncvar, int dimid, int dimtype)
{
  if (ncvar->dimtypes[dimid] != CDI_UNDEFID && ncvar->dimtypes[dimid] != dimtype)
  {
    Warning("Inconsistent dimension definition for %s! dimid=%d  type=%c  newtype=%c", ncvar->name, dimid,
            axisTypeChar[ncvar->dimtypes[dimid]], axisTypeChar[dimtype]);
  }

  ncvar->dimtypes[dimid] = dimtype;
}

static void
scan_hybrid_formulaterms(int ncid, int ncfvarid, int *avarid, int *bvarid, int *psvarid, int *p0varid)
{
  *avarid = -1;
  *bvarid = -1;
  *psvarid = -1;
  *p0varid = -1;

  char attstring[1024];
  cdfGetAttText(ncid, ncfvarid, "formula_terms", sizeof(attstring), attstring);
  char *pstring = attstring;

  bool lstop = false;
  for (int i = 0; i < 4; i++)
  {
    while (isspace((int)*pstring))
      pstring++;
    if (*pstring == 0)
      break;
    char *tagname = pstring;
    while (!isspace((int)*pstring) && *pstring != 0)
      pstring++;
    if (*pstring == 0)
      lstop = true;
    *(pstring++) = 0;

    while (isspace((int)*pstring))
      pstring++;
    if (*pstring == 0)
      break;
    char *varname = pstring;
    while (!isspace((int)*pstring) && *pstring != 0)
      pstring++;
    if (*pstring == 0)
      lstop = true;
    *(pstring++) = 0;

    int dimvarid;
    int status_nc = nc_inq_varid(ncid, varname, &dimvarid);
    if (status_nc == NC_NOERR)
    {
      // clang-format off
      if      (str_is_equal(tagname, "ap:")) *avarid  = dimvarid;
      else if (str_is_equal(tagname, "a:") ) *avarid  = dimvarid;
      else if (str_is_equal(tagname, "b:") ) *bvarid  = dimvarid;
      else if (str_is_equal(tagname, "ps:")) *psvarid = dimvarid;
      else if (str_is_equal(tagname, "p0:")) *p0varid = dimvarid;
      // clang-format on
    }
    else if (!str_is_equal(tagname, "ps:"))
    {
      Warning("%s - %s", nc_strerror(status_nc), varname);
    }

    if (lstop)
      break;
  }
}

static void
readVCT(int ncid, int ndims2, size_t dimlen, size_t dimlen2, int avarid2, int bvarid2, double *vct)
{
  double *abuf = (double *)Malloc(dimlen * 2 * sizeof(double));
  double *bbuf = (double *)Malloc(dimlen * 2 * sizeof(double));
  cdf_get_var_double(ncid, avarid2, abuf);
  cdf_get_var_double(ncid, bvarid2, bbuf);

  if (ndims2 == 2)
  {
    for (size_t i = 0; i < dimlen; ++i)
    {
      vct[i] = abuf[i * 2];
      vct[i + dimlen + 1] = bbuf[i * 2];
    }
    vct[dimlen] = abuf[dimlen * 2 - 1];
    vct[dimlen * 2 + 1] = bbuf[dimlen * 2 - 1];
  }
  else
  {
    for (size_t i = 0; i < dimlen2; ++i)
    {
      vct[i] = abuf[i];
      vct[i + dimlen + 1] = bbuf[i];
    }
  }

  Free(abuf);
  Free(bbuf);
}

static bool
isHybridSigmaPressureCoordinate(int ncid, int ncvarid, ncvar_t *ncvars, const ncdim_t *ncdims)
{
  bool status = false;
  ncvar_t *ncvar = &ncvars[ncvarid];

  if (str_is_equal(ncvar->stdname, "atmosphere_hybrid_sigma_pressure_coordinate"))
  {
    CDI_Convention = CDI_CONVENTION_CF;

    status = true;
    ncvar->zaxistype = ZAXIS_HYBRID;
    // int ndims = ncvar->ndims;
    int dimid = ncvar->dimids[0];
    size_t dimlen = ncdims[dimid].len;
    int avarid1 = -1, bvarid1 = -1, psvarid1 = -1, p0varid1 = -1;
    int ncfvarid = ncvarid;
    if (ncvars[ncfvarid].hasFormulaterms)
      scan_hybrid_formulaterms(ncid, ncfvarid, &avarid1, &bvarid1, &psvarid1, &p0varid1);
    // printf("avarid1, bvarid1, psvarid1, p0varid1 %d %d %d %d\n", avarid1, bvarid1, psvarid1, p0varid1);
    if (avarid1 != -1)
      ncvars[avarid1].varStatus = CoordVar;
    if (bvarid1 != -1)
      ncvars[bvarid1].varStatus = CoordVar;
    if (psvarid1 != -1)
      ncvar->psvarid = psvarid1;
    if (p0varid1 != -1)
      ncvar->p0varid = p0varid1;

    if (ncvar->bounds != CDI_UNDEFID && ncvars[ncvar->bounds].hasFormulaterms)
    {
      ncfvarid = ncvar->bounds;
      int avarid2 = -1, bvarid2 = -1, psvarid2 = -1, p0varid2 = -1;
      if (ncvars[ncfvarid].hasFormulaterms)
        scan_hybrid_formulaterms(ncid, ncfvarid, &avarid2, &bvarid2, &psvarid2, &p0varid2);
      // printf("avarid2, bvarid2, psvarid2, p0varid2 %d %d %d %d\n", avarid2, bvarid2, psvarid2, p0varid2);
      if (avarid2 != -1 && bvarid2 != -1)
      {
        ncvars[avarid2].varStatus = CoordVar;
        ncvars[bvarid2].varStatus = CoordVar;

        int ndims2 = ncvars[avarid2].ndims;
        int dimid2 = ncvars[avarid2].dimids[0];
        size_t dimlen2 = ncdims[dimid2].len;

        if ((ndims2 == 2 && dimid == dimid2) || (ndims2 == 1 && dimlen == dimlen2 - 1))
        {
          double px = 1;
          if (p0varid1 != -1 && p0varid1 == p0varid2)
            cdf_get_var_double(ncid, p0varid2, &px);

          size_t vctsize = (dimlen + 1) * 2;
          double *vct = (double *)Malloc(vctsize * sizeof(double));

          readVCT(ncid, ndims2, dimlen, dimlen2, avarid2, bvarid2, vct);

          if (p0varid1 != -1 && IS_NOT_EQUAL(px, 1))
            for (size_t i = 0; i < dimlen + 1; ++i)
              vct[i] *= px;

          ncvar->vct = vct;
          ncvar->vctsize = vctsize;
        }
      }
    }
  }

  return status;
}

static void
cdf_set_cdi_attr(int ncid, int ncvarid, int attnum, int cdiID, int varID, bool removeFillValue)
{
  nc_type atttype;
  size_t attlen;
  char attname[CDI_MAX_NAME];

  cdf_inq_attname(ncid, ncvarid, attnum, attname);
  cdf_inq_attlen(ncid, ncvarid, attname, &attlen);
  cdf_inq_atttype(ncid, ncvarid, attname, &atttype);

  if (removeFillValue && str_is_equal("_FillValue", attname))
    return;

  if (xtypeIsInt(atttype))
  {
    int attint = 0;
    int *pattint = (attlen > 1) ? (int *)Malloc(attlen * sizeof(int)) : &attint;
    cdfGetAttInt(ncid, ncvarid, attname, attlen, pattint);
    // clang-format off
    int datatype = (atttype == NC_SHORT)  ? CDI_DATATYPE_INT16 :
                   (atttype == NC_BYTE)   ? CDI_DATATYPE_INT8 :
                   (atttype == NC_UBYTE)  ? CDI_DATATYPE_UINT8 :
                   (atttype == NC_USHORT) ? CDI_DATATYPE_UINT16 :
                   (atttype == NC_UINT)   ? CDI_DATATYPE_UINT32 :
                                            CDI_DATATYPE_INT32;
    // clang-format on
    cdiDefAttInt(cdiID, varID, attname, datatype, (int)attlen, pattint);
    if (attlen > 1)
      Free(pattint);
  }
  else if (xtypeIsInt64(atttype))
  {
    int64_t attint64 = 0;
    int64_t *pattint64 = (attlen > 1) ? (int64_t *)Malloc(attlen * sizeof(int64_t)) : &attint64;
    cdfGetAttInt64(ncid, ncvarid, attname, attlen, pattint64);
    bool defineAtts = true;
    for (size_t i = 0; i < attlen; ++i)
      if (pattint64[i] > INT_MAX)
        defineAtts = false;
    if (defineAtts)
    {
      int attint = 0;
      int *pattint = (attlen > 1) ? (int *)Malloc(attlen * sizeof(int)) : &attint;
      for (size_t i = 0; i < attlen; ++i)
        pattint[i] = (int)pattint64[i];
      cdiDefAttInt(cdiID, varID, attname, CDI_DATATYPE_INT32, (int)attlen, pattint);
      if (attlen > 1)
        Free(pattint);
    }
    if (attlen > 1)
      Free(pattint64);
  }
  else if (xtypeIsFloat(atttype))
  {
    double attflt = 0.0;
    double *pattflt = (attlen > 1) ? (double *)Malloc(attlen * sizeof(double)) : &attflt;
    cdfGetAttDouble(ncid, ncvarid, attname, attlen, pattflt);
    int datatype = (atttype == NC_FLOAT) ? CDI_DATATYPE_FLT32 : CDI_DATATYPE_FLT64;
    cdiDefAttFlt(cdiID, varID, attname, datatype, (int)attlen, pattflt);
    if (attlen > 1)
      Free(pattflt);
  }
  else if (xtypeIsText(atttype))
  {
    char attstring[8192];
    cdfGetAttText(ncid, ncvarid, attname, sizeof(attstring), attstring);
    cdiDefAttTxt(cdiID, varID, attname, (int)strlen(attstring), attstring);
  }
}

static void
cdf_print_vars(const ncvar_t *ncvars, int nvars, const char *oname)
{
  // clang-format off
  char axis[7];
  enum { TAXIS = 't', ZAXIS = 'z', EAXIS = 'e', YAXIS = 'y', XAXIS = 'x' };

  fprintf(stderr, "%s:\n", oname);

  for (int varid = 0; varid < nvars; varid++)
  {
    const ncvar_t *ncvar = &ncvars[varid];
    int ndim = 0;
    if (ncvar->varStatus == DataVar || ncvar->varStatus == UndefVar)
    {
      axis[ndim++] = (ncvar->varStatus == DataVar) ? 'v' : 'u';
      axis[ndim++] = ':';
      for (int i = 0; i < ncvar->ndims; i++)
      {
        if      (ncvar->dimtypes[i] == T_AXIS) axis[ndim++] = TAXIS;
        else if (ncvar->dimtypes[i] == Z_AXIS) axis[ndim++] = ZAXIS;
        else if (ncvar->dimtypes[i] == E_AXIS) axis[ndim++] = EAXIS;
        else if (ncvar->dimtypes[i] == Y_AXIS) axis[ndim++] = YAXIS;
        else if (ncvar->dimtypes[i] == X_AXIS) axis[ndim++] = XAXIS;
        else                                   axis[ndim++] = '?';
      }
    }
    else
    {
      axis[ndim++] = 'c';
      axis[ndim++] = ':';
      if      (ncvar->isTaxis) axis[ndim++] = TAXIS;
      else if (ncvar->isZaxis) axis[ndim++] = ZAXIS;
      else if (ncvar->isLat  ) axis[ndim++] = YAXIS;
      else if (ncvar->isYaxis) axis[ndim++] = YAXIS;
      else if (ncvar->isLon  ) axis[ndim++] = XAXIS;
      else if (ncvar->isXaxis) axis[ndim++] = XAXIS;
      else                     axis[ndim++] = '?';
    }

    axis[ndim++] = 0;

    fprintf(stderr, "%3d %3d  %-6s %s\n", varid, ndim-3, axis, ncvar->name);
  }
  // clang-format on
}

static void
cdf_scan_attr_axis(ncvar_t *ncvars, ncdim_t *ncdims, int ncvarid, const char *attstring, int nvdims, const int *dimidsp)
{
  ncvar_t *ncvar = &ncvars[ncvarid];
  int attlen = (int)strlen(attstring);

  if (nvdims == 0 && attlen == 1 && attstring[0] == 'z')
  {
    cdf_set_var(ncvar, CoordVar);
    ncvar->isZaxis = true;
    return;
  }

  if (attlen != nvdims)
    return;

  static const char accept[] = "-tTzZyYxX";
  if ((int)strspn(attstring, accept) != attlen)
    return;

  while (attlen--)
  {
    int dimtype;
    bool setVar = false;
    switch (attstring[attlen])
    {
    case 't':
    case 'T':
      if (attlen != 0)
        Warning("axis attribute 't' not on first position");
      dimtype = T_AXIS;
      break;
    case 'z':
    case 'Z':
      ncvar->zdim = dimidsp[attlen];
      dimtype = Z_AXIS;
      setVar = (ncvar->ndims == 1);
      break;
    case 'y':
    case 'Y':
      ncvar->ydim = dimidsp[attlen];
      dimtype = Y_AXIS;
      setVar = (ncvar->ndims == 1);
      break;
    case 'x':
    case 'X':
      ncvar->xdim = dimidsp[attlen];
      dimtype = X_AXIS;
      setVar = (ncvar->ndims == 1);
      break;
    default:
      continue;
    }
    cdf_set_dim(ncvar, attlen, dimtype);

    if (setVar)
    {
      cdf_set_var(ncvar, CoordVar);
      ncdims[ncvar->dimids[0]].dimtype = (ncdims[ncvar->dimids[0]].dimtype == CDI_UNDEFID) ? dimtype : CDI_UNDEFID;
    }
  }
}

static int
cdf_get_cell_varid(char *attstring, int ncid)
{
  int nc_cell_id = CDI_UNDEFID;

  char *pstring = attstring;
  while (isspace((int)*pstring))
    pstring++;
  char *cell_measures = pstring;
  while (isalnum((int)*pstring))
    pstring++;
  *(pstring++) = 0;
  while (isspace((int)*pstring))
    pstring++;
  char *cell_var = pstring;
  while (!isspace((int)*pstring) && *pstring != 0)
    pstring++;
  *(pstring++) = 0;
  /*
    printf("cell_measures >%s<\n", cell_measures);
    printf("cell_var >%s<\n", cell_var);
  */
  if (strStartsWith(cell_measures, "area"))
  {
    int nc_var_id;
    int status = nc_inq_varid(ncid, cell_var, &nc_var_id);
    if (status == NC_NOERR)
      nc_cell_id = nc_var_id;
    /*
    else
      Warning("%s - %s", nc_strerror(status), cell_var);
    */
  }

  return nc_cell_id;
}

static bool
is_valid_coordinate(ncvar_t *ncvar)
{
  bool status = true;
  if (ncvar->ndims > 1 && (str_is_equal(ncvar->name, "zg") || str_is_equal(ncvar->name, "zghalf")))
    status = false;
  return status;
}

static void
read_coordinates_vars(int ncid, char *attstring, ncvar_t *ncvar, ncvar_t *ncvars, int *nchecked_vars, char *checked_vars[],
                      int max_check_vars)
{
  bool lstop = false;
  for (int i = 0; i < MAX_COORDVARS && !lstop; i++)
  {
    while (isspace((int)*attstring))
      attstring++;
    if (*attstring == 0)
      break;
    char *varname = attstring;
    while (!isspace((int)*attstring) && *attstring != 0)
      attstring++;
    if (*attstring == 0)
      lstop = true;
    if (*(attstring - 1) == ',')
      *(attstring - 1) = 0;
    *(attstring++) = 0;

    int dimvarid;
    int status = nc_inq_varid(ncid, varname, &dimvarid);
    if (status == NC_NOERR)
    {
      if (is_valid_coordinate(&ncvars[dimvarid]))
      {
        cdf_set_var(&ncvars[dimvarid], CoordVar);
        if (!CDI_Ignore_Att_Coordinates)
        {
          ncvar->coordvarids[i] = dimvarid;
          ncvar->ncoordvars++;
        }
      }
    }
    else
    {
      if (!CDI_Ignore_Att_Coordinates)
        ncvar->ncoordvars++;

      int k;
      for (k = 0; k < *nchecked_vars; ++k)
        if (str_is_equal(checked_vars[k], varname))
          break;

      if (k == *nchecked_vars)
      {
        if (*nchecked_vars < max_check_vars)
          checked_vars[(*nchecked_vars)++] = strdup(varname);
        Warning("%s - >%s<", nc_strerror(status), varname);
      }
    }
  }
}

static void
read_auxiliary_vars(int ncid, char *attstring, ncvar_t *ncvar, ncvar_t *ncvars)
{
  bool lstop = false;
  for (int i = 0; i < MAX_AUXVARS && !lstop; i++)
  {
    while (isspace((int)*attstring))
      attstring++;
    if (*attstring == 0)
      break;
    char *varname = attstring;
    while (!isspace((int)*attstring) && *attstring != 0)
      attstring++;
    if (*attstring == 0)
      lstop = true;
    *(attstring++) = 0;

    int dimvarid;
    int status = nc_inq_varid(ncid, varname, &dimvarid);
    if (status == NC_NOERR)
    {
      cdf_set_var(&ncvars[dimvarid], CoordVar);
      //  if ( !CDI_Ignore_Att_Coordinates )
      {
        ncvar->auxvarids[i] = dimvarid;
        ncvar->nauxvars++;
      }
    }
    else
      Warning("%s - %s", nc_strerror(status), varname);
  }
}

static void
read_grid_mapping(int ncid, char *attstring, ncvar_t *ncvar, ncvar_t *ncvars)
{
  int nc_gmap_id;
  int status = nc_inq_varid(ncid, attstring, &nc_gmap_id);
  if (status == NC_NOERR)
  {
    ncvar->gmapid = nc_gmap_id;
    cdf_set_var(&ncvars[ncvar->gmapid], CoordVar);
    int nc_gmap_varid = ncvars[ncvar->gmapid].ncid;
    if (cdfCheckAttText(nc_gmap_varid, nc_gmap_id, "grid_mapping_name"))
    {
      char gridMappingName[CDI_MAX_NAME];
      cdfGetAttText(nc_gmap_varid, nc_gmap_id, "grid_mapping_name", CDI_MAX_NAME, gridMappingName);
      // if (str_is_equal(gridMappingName, "healpix")) ncvars[ncvar->gmapid].isHealPIX = true;
      if (str_is_equal(gridMappingName, "healpix"))
        ncvar->isHealpixMapping = true;
      else if (str_is_equal(gridMappingName, "latitude_longitude"))
        ncvar->isLonLatMapping = true;
    }
  }
  else
    Warning("%s - %s", nc_strerror(status), attstring);
}

static void
set_vars_chunks(int ncid, int ncvarid, int nvdims, ncvar_t *ncvar)
{
  int shuffle = 0, deflate = 0, deflateLevel = 0;
  nc_inq_var_deflate(ncid, ncvarid, &shuffle, &deflate, &deflateLevel);
  if (deflate > 0)
    ncvar->hasDeflate = true;

#ifdef HAVE_NC_DEF_VAR_SZIP
  int options_mask = 0, pixels_per_block = 0;
  nc_inq_var_szip(ncid, ncvarid, &options_mask, &pixels_per_block);
  if (options_mask && pixels_per_block)
    ncvar->hasSzip = true;
#endif
  ncvar->hasFilter = cdf_get_var_filter(ncid, ncvarid, ncvar->filterSpec, CDI_MAX_NAME);
  // if (ncvar->hasFilter) printf("filterSpec: %s=%s\n", ncvar->name, ncvar->filterSpec);

  size_t chunks[MAX_DIMS_CDF];
  int storageIn;
  if (nc_inq_var_chunking(ncid, ncvarid, &storageIn, chunks) == NC_NOERR)
  {
    if (storageIn == NC_CHUNKED)
    {
      ncvar->isChunked = true;
      for (int i = 0; i < nvdims; ++i)
        ncvar->chunks[i] = chunks[i];
      if (CDI_Debug)
      {
        fprintf(stderr, "%s: chunking %d %d %d  chunks ", ncvar->name, storageIn, NC_CONTIGUOUS, NC_CHUNKED);
        for (int i = 0; i < nvdims; ++i)
          fprintf(stderr, "%zu ", chunks[i]);
        fprintf(stderr, "\n");
      }
    }
  }

  size_t size;
  size_t nelems;
  float preemption;
  if (nc_get_var_chunk_cache(ncid, ncvarid, &size, &nelems, &preemption) == NC_NOERR)
  {
    ncvar->chunkCacheSize = size;
    ncvar->chunkCacheNelems = nelems;
    ncvar->chunkCachePreemption = preemption;
    if (CDI_Debug)
      fprintf(stderr, "%s: chunkCacheSize=%zu nelems=%zu preemption=%g\n", ncvar->name, size, nelems, preemption);
  }
}

#if defined __GNUC__ || (__GNUC__ == 4 && __GNUC_MINOR__ == 9)
#pragma GCC diagnostic push
#pragma GCC diagnostic warning "-Wstrict-overflow"
#endif

static void
read_vars_info(int nvars, ncvar_t *ncvars, int ndims, ncdim_t *ncdims, int format)
{
  for (int varid = 0; varid < nvars; varid++)
  {
    ncvar_t *ncvar = &ncvars[varid];
    cdf_inq_var(ncvar->ncid, varid, ncvar->name, &ncvar->xtype, &ncvar->ndims, ncvar->dimids, &ncvar->nattsNC);

    for (int vdimid = 0; vdimid < ncvar->ndims; ++vdimid)
      for (int gdimid = 0; gdimid < ndims; ++gdimid)
        if (ncdims[gdimid].dimid == ncvar->dimids[vdimid])
        {
          ncvar->dimids[vdimid] = gdimid;
          break;
        }

    for (int vdimid = 0; vdimid < ncvar->ndims; vdimid++)
      ncvar->dimtypes[vdimid] = -1;

    if (format == NC_FORMAT_NETCDF4_CLASSIC || format == NC_FORMAT_NETCDF4)
    {
      set_vars_chunks(ncvar->ncid, varid, ncvar->ndims, ncvar);
    }
  }
}

static void
set_vars_timetype(int nvars, ncvar_t *ncvars, int timedimid)
{
  for (int varid = 0; varid < nvars; varid++)
  {
    ncvar_t *ncvar = &ncvars[varid];
    if (ncvar->ndims > 0)
    {
      if (timedimid == ncvar->dimids[0])
      {
        ncvar->timetype = TIME_VARYING;
        cdf_set_dim(ncvar, 0, T_AXIS);
      }
      else
      {
        for (int i = 1, n = ncvar->ndims; i < n; i++)
        {
          if (timedimid == ncvar->dimids[i])
          {
            Warning("Time must be the first dimension! Unsupported array structure, skipped variable %s!", ncvar->name);
            ncvar->varStatus = CoordVar;
          }
        }
      }
    }
  }
}

static void
scan_vars_attr(int nvars, ncvar_t *ncvars, int ndims, ncdim_t *ncdims, int modelID)
{
  int nchecked_vars = 0;
  enum
  {
    max_check_vars = 9
  };
  char *checked_vars[max_check_vars];
  for (int i = 0; i < max_check_vars; ++i)
    checked_vars[i] = NULL;

  char attname[CDI_MAX_NAME];
  char attstring[8192];

  for (int ncvarid = 0; ncvarid < nvars; ncvarid++)
  {
    ncvar_t *ncvar = &ncvars[ncvarid];
    int ncid = ncvar->ncid;
    const char *name = ncvar->name;
    int nvdims = ncvar->ndims;
    nc_type xtype = ncvar->xtype;
    int nvatts = ncvar->nattsNC;

    if (ncvar->natts == 0 && nvatts > 0)
      ncvar->atts = (int *)Malloc((size_t)nvatts * sizeof(int));

    for (int iatt = 0; iatt < nvatts; ++iatt)
    {
      int nc_cell_id = CDI_UNDEFID;

      nc_type atttype;
      size_t attlen;
      cdf_inq_attname(ncid, ncvarid, iatt, attname);
      cdf_inq_atttype(ncid, ncvarid, attname, &atttype);
      cdf_inq_attlen(ncid, ncvarid, attname, &attlen);

      size_t attstringsize = sizeof(attstring);
      bool isText = xtypeIsText(atttype), isNumber = xtypeIsFloat(atttype) || xtypeIsInt(atttype);
      bool isRealization = false, isEnsembleMembers = false, isForecastInitType = false;
      if (isText)
      {
        cdfGetAttText(ncid, ncvarid, attname, sizeof(attstring), attstring);
        attstringsize = strlen(attstring) + 1;
        if (attstringsize > CDI_MAX_NAME)
          attstringsize = CDI_MAX_NAME;
      }

      if (isText && str_is_equal(attname, "long_name"))
      {
        memcpy(ncvar->longname, attstring, attstringsize);
      }
      else if (isText && str_is_equal(attname, "standard_name"))
      {
        memcpy(ncvar->stdname, attstring, attstringsize);
      }
      else if (isText && str_is_equal(attname, "units"))
      {
        ncvar->unitsLen = (int)attstringsize;
        memcpy(ncvar->units, attstring, attstringsize);
      }
      else if (isText && str_is_equal(attname, "calendar"))
      {
        ncvar->hasCalendar = true;
      }
      else if (isText && str_is_equal(attname, "param"))
      {
        int pnum = 0, pcat = 255, pdis = 255;
        sscanf(attstring, "%d.%d.%d", &pnum, &pcat, &pdis);
        ncvar->param = cdiEncodeParam(pnum, pcat, pdis);
        cdf_set_var(ncvar, DataVar);
      }
      else if (isText && str_is_equal(attname, "trunc_type"))
      {
        if (str_is_equal(attstring, "Triangular"))
          ncvar->gridtype = GRID_SPECTRAL;
      }
      else if (isText && (str_is_equal(attname, "grid_type") || str_is_equal(attname, "CDI_grid_type")))
      {
        str_to_lower(attstring);
        cdf_set_gridtype(attstring, &ncvar->gridtype);
        cdf_set_var(ncvar, DataVar);
      }
      else if (isText && str_is_equal(attname, "CDI_grid_latitudes"))
      {
        int nc_yvar_id;
        int status = nc_inq_varid(ncid, attstring, &nc_yvar_id);
        if (status == NC_NOERR)
        {
          ncvar->yvarid = nc_yvar_id;
          cdf_set_var(&ncvars[ncvar->yvarid], CoordVar);
        }
        else
          Warning("%s - %s", nc_strerror(status), attstring);

        cdf_set_var(ncvar, DataVar);
      }
      else if (isText && str_is_equal(attname, "CDI_grid_reduced_points"))
      {
        int nc_rpvar_id;
        int status = nc_inq_varid(ncid, attstring, &nc_rpvar_id);
        if (status == NC_NOERR)
        {
          ncvar->rpvarid = nc_rpvar_id;
          cdf_set_var(&ncvars[ncvar->rpvarid], CoordVar);
        }
        else
          Warning("%s - %s", nc_strerror(status), attstring);

        cdf_set_var(ncvar, DataVar);
      }
      else if (isNumber && str_is_equal(attname, "code"))
      {
        cdfGetAttInt(ncid, ncvarid, attname, 1, &ncvar->code);
        cdf_set_var(ncvar, DataVar);
      }
      else if (isNumber && str_is_equal(attname, "table"))
      {
        int tablenum;
        cdfGetAttInt(ncid, ncvarid, attname, 1, &tablenum);
        if (tablenum > 0)
        {
          ncvar->tabnum = tablenum;
          ncvar->tableID = tableInq(modelID, tablenum, NULL);
          if (ncvar->tableID == CDI_UNDEFID)
            ncvar->tableID = tableDef(modelID, tablenum, NULL);
        }
        cdf_set_var(ncvar, DataVar);
      }
      else if (isNumber && str_is_equal(attname, "CDI_grid_num_LPE"))
      {
        cdfGetAttInt(ncid, ncvarid, attname, 1, &ncvar->numLPE);
      }
      else if (isText && str_is_equal(attname, "level_type"))
      {
        str_to_lower(attstring);
        cdf_set_zaxistype(attstring, &ncvar->zaxistype);
        cdf_set_var(ncvar, DataVar);
      }
      else if (isNumber && str_is_equal(attname, "trunc_count"))
      {
        cdfGetAttInt(ncid, ncvarid, attname, 1, &ncvar->truncation);
      }
      else if (isNumber && str_is_equal(attname, "truncation"))
      {
        cdfGetAttInt(ncid, ncvarid, attname, 1, &ncvar->truncation);
      }
      else if (isNumber && str_is_equal(attname, "number_of_grid_in_reference"))
      {
        cdfGetAttInt(ncid, ncvarid, attname, 1, &ncvar->position);
      }
      else if (isNumber && str_is_equal(attname, "add_offset"))
      {
        cdfGetAttDouble(ncid, ncvarid, attname, 1, &ncvar->addoffset);
        /*
          if ( atttype != NC_BYTE && atttype != NC_SHORT && atttype != NC_INT )
          if ( ncvar->addoffset != 0 )
          Warning("attribute add_offset not supported for atttype %d", atttype);
        */
        // (also used for lon/lat) cdf_set_var(ncvar, DataVar);
      }
      else if (isNumber && str_is_equal(attname, "scale_factor"))
      {
        cdfGetAttDouble(ncid, ncvarid, attname, 1, &ncvar->scalefactor);
        /*
          if ( atttype != NC_BYTE && atttype != NC_SHORT && atttype != NC_INT )
          if ( ncvar->scalefactor != 1 )
          Warning("attribute scale_factor not supported for atttype %d", atttype);
        */
        // (also used for lon/lat) cdf_set_var(ncvar, DataVar);
      }
      else if (isText && str_is_equal(attname, "climatology"))
      {
        int ncboundsid;
        int status = nc_inq_varid(ncid, attstring, &ncboundsid);
        if (status == NC_NOERR)
        {
          ncvar->isClimatology = true;
          ncvar->bounds = ncboundsid;
          cdf_set_var(&ncvars[ncvar->bounds], CoordVar);
          cdf_set_var(ncvar, CoordVar);
        }
        else
          Warning("%s - %s", nc_strerror(status), attstring);
      }
      else if (isText && str_is_equal(attname, "bounds"))
      {
        int ncboundsid;
        int status = nc_inq_varid(ncid, attstring, &ncboundsid);
        if (status == NC_NOERR)
        {
          ncvar->bounds = ncboundsid;
          cdf_set_var(&ncvars[ncvar->bounds], CoordVar);
          cdf_set_var(ncvar, CoordVar);
        }
        else
        {
          static bool printWarning = true;
          if (printWarning)
          {
            printWarning = false;
            Warning("%s - %s", nc_strerror(status), attstring);
          }
        }
      }
      else if (isText && str_is_equal(attname, "formula_terms"))
      {
        ncvar->hasFormulaterms = true;
      }
      else if (isText && str_is_equal(attname, "cell_measures") && (nc_cell_id = cdf_get_cell_varid(attstring, ncid)) != CDI_UNDEFID)
      {
        ncvar->cellarea = nc_cell_id;
        ncvars[nc_cell_id].varStatus = CoordVar;
        cdf_set_var(ncvar, DataVar);
      }
      else if (isText && (str_is_equal(attname, "associate") || str_is_equal(attname, "coordinates")))
      {
        read_coordinates_vars(ncid, attstring, ncvar, ncvars, &nchecked_vars, checked_vars, max_check_vars);
        cdf_set_var(ncvar, DataVar);
      }
      else if (isText && str_is_equal(attname, "auxiliary_variable"))
      {
        read_auxiliary_vars(ncid, attstring, ncvar, ncvars);
        cdf_set_var(ncvar, DataVar);
      }
      else if (isText && str_is_equal(attname, "grid_mapping"))
      {
        read_grid_mapping(ncid, attstring, ncvar, ncvars);
        cdf_set_var(ncvar, DataVar);
      }
      else if (isText && str_is_equal(attname, "positive"))
      {
        str_to_lower(attstring);
        if (str_is_equal(attstring, "down"))
          ncvar->positive = POSITIVE_DOWN;
        else if (str_is_equal(attstring, "up"))
          ncvar->positive = POSITIVE_UP;

        int dimid0 = ncvar->dimids[0];
        if (ncvar->varStatus == UndefVar && (nvdims == 0 || (nvdims == 1 && ncvar->dimtypes[0] == CDI_UNDEFID && ncdims[dimid0].ncvarid == CDI_UNDEFID)))
        {
          if (nvdims == 1)
          {
            cdf_set_var(ncvar, CoordVar);
            cdf_set_dim(ncvar, 0, Z_AXIS);
            if (dimid0 < ndims)
              ncdims[dimid0].dimtype = Z_AXIS;
          }
          else if (nvdims == 0)
          {
            cdf_set_var(ncvar, CoordVar);
            ncvar->isZaxis = true;
          }
        }
        else
        {
          ncvar->atts[ncvar->natts++] = iatt;
        }
      }
      else if (isText && str_is_equal(attname, "cdi"))
      {
        if (!strcasecmp(attstring, "ignore"))
        {
          ncvar->ignoreVar = true;
          cdf_set_var(ncvar, CoordVar);
        }
      }
      else if (isText && str_is_equal(attname, "_Unsigned"))
      {
        if (!strcasecmp(attstring, "true"))
        {
          ncvar->isUnsigned = true;
          /*
          ncvar->validrangeDefined = true;
          ncvar->validrange[0] = 0;
          ncvar->validrange[1] = 255;
          */
        }
        // cdf_set_var(ncvar, DataVar);
      }
      else if (isNumber && str_is_equal(attname, "_FillValue"))
      {
        cdfGetAttDouble(ncid, ncvarid, attname, 1, &ncvar->fillval);
        ncvar->fillvalDefined = true;
        // cdf_set_var(ncvar, DataVar);
      }
      else if (isNumber && str_is_equal(attname, "missing_value"))
      {
        cdfGetAttDouble(ncid, ncvarid, attname, 1, &ncvar->missval);
        ncvar->missvalDefined = true;
        // cdf_set_var(ncvar, DataVar);
      }
      else if (isNumber && str_is_equal(attname, "valid_range") && attlen == 2)
      {
        if (ncvar->validrangeDefined == false)
        {
          bool ignoreDatatype = (xtypeIsFloat(atttype) != xtypeIsFloat(xtype));
          if (!CDI_Ignore_Valid_Range && ignoreDatatype == false)
          {
            cdfGetAttDouble(ncid, ncvarid, attname, 2, ncvar->validrange);
            ncvar->validrangeDefined = (ncvar->validrange[0] <= ncvar->validrange[1]);
            if (((int)ncvar->validrange[0]) == 0 && ((int)ncvar->validrange[1]) == 255)
              ncvar->isUnsigned = true;
            // cdf_set_var(ncvar, DataVar);
          }
          else if (ignoreDatatype)
          {
            Warning("Inconsistent data type for attribute %s:valid_range, ignored!", name);
          }
        }
      }
      else if (isNumber && str_is_equal(attname, "valid_min") && attlen == 1)
      {
        bool ignoreDatatype = (xtypeIsFloat(atttype) != xtypeIsFloat(xtype));
        if (!CDI_Ignore_Valid_Range && ignoreDatatype == false)
        {
          cdfGetAttDouble(ncid, ncvarid, attname, 1, &(ncvar->validrange)[0]);
          ncvar->validrangeDefined = true;
        }
        else if (ignoreDatatype)
        {
          Warning("Inconsistent data type for attribute %s:valid_min, ignored!", name);
        }
      }
      else if (isNumber && str_is_equal(attname, "valid_max") && attlen == 1)
      {
        bool ignoreDatatype = (xtypeIsFloat(atttype) != xtypeIsFloat(xtype));
        if (!CDI_Ignore_Valid_Range && ignoreDatatype == false)
        {
          cdfGetAttDouble(ncid, ncvarid, attname, 1, &(ncvar->validrange)[1]);
          ncvar->validrangeDefined = true;
        }
        else if (ignoreDatatype)
        {
          Warning("Inconsistent data type for attribute %s:valid_max, ignored!", name);
        }
      }
      else if (isNumber && ((isRealization = str_is_equal(attname, "realization")) || (isEnsembleMembers = str_is_equal(attname, "ensemble_members")) || (isForecastInitType = str_is_equal(attname, "forecast_init_type"))))
      {
        int temp;
        cdfGetAttInt(ncid, ncvarid, attname, 1, &temp);

        // clang-format off
        if      (isRealization)      ncvar->perturbationNumber = temp;
        else if (isEnsembleMembers)  ncvar->numberOfForecastsInEnsemble = temp;
        else if (isForecastInitType) ncvar->typeOfEnsembleForecast = temp;
        // clang-format on

        cdf_set_var(ncvar, DataVar);
      }
      else
      {
        ncvar->atts[ncvar->natts++] = iatt;
      }
    }
  }

  for (int i = 0; i < max_check_vars; ++i)
    if (checked_vars[i])
      Free(checked_vars[i]);
}
#if defined __GNUC__ || (__GNUC__ == 4 && __GNUC_MINOR__ == 9)
#pragma GCC diagnostic pop
#endif

static void
cdf_set_chunk_info(stream_t *streamptr, int nvars, ncvar_t *ncvars)
{
  int vlistID = streamptr->vlistID;
  CdfInfo *cdfInfo = &(streamptr->cdfInfo);
  for (int ncvarid = 0; ncvarid < nvars; ncvarid++)
  {
    int chunkSizeDimT = 0;
    int chunkSizeDimZ = 0;
    int chunkSizeDimY = 0;
    int chunkSizeDimX = 0;
    ncvar_t *ncvar = &ncvars[ncvarid];
    int varID = ncvar->cdiVarID;
    if (ncvar->varStatus == DataVar && ncvar->isChunked && varID != CDI_UNDEFID)
    {
      for (int i = 0, n = ncvar->ndims; i < n; ++i)
      {
        size_t chunkSize = ncvar->chunks[i];
        if (chunkSize > 1)
        {
          int dimType = ncvar->dimtypes[i];
          // clang-format off
          if      (dimType == T_AXIS && chunkSize > cdfInfo->chunkSizeDimT) cdfInfo->chunkSizeDimT = chunkSize;
          else if (dimType == Z_AXIS && chunkSize > cdfInfo->chunkSizeDimZ) cdfInfo->chunkSizeDimZ = chunkSize;

          if      (dimType == T_AXIS) chunkSizeDimT = chunkSize;
          else if (dimType == Z_AXIS) chunkSizeDimZ = chunkSize;
          else if (dimType == Y_AXIS) chunkSizeDimY = chunkSize;
          else if (dimType == X_AXIS) chunkSizeDimX = chunkSize;
          // clang-format on
        }
      }
      if ((CDI_CopyChunkSpec || chunkSizeDimT == 0) && CDI_RemoveChunkSpec == false)
      {
        if (chunkSizeDimT > 0)
          cdiDefKeyInt(vlistID, varID, CDI_KEY_CHUNKSIZE_DIMT, chunkSizeDimT);
        if (chunkSizeDimZ > 0)
          cdiDefKeyInt(vlistID, varID, CDI_KEY_CHUNKSIZE_DIMZ, chunkSizeDimZ);
        if (chunkSizeDimY > 0)
          cdiDefKeyInt(vlistID, varID, CDI_KEY_CHUNKSIZE_DIMY, chunkSizeDimY);
        if (chunkSizeDimX > 0)
          cdiDefKeyInt(vlistID, varID, CDI_KEY_CHUNKSIZE_DIMX, chunkSizeDimX);
      }
    }
  }
}

static void
verify_vars_attr(int nvars, ncvar_t *ncvars, ncdim_t *ncdims)
{
  nc_type atttype;
  size_t attlen;
  char attname[CDI_MAX_NAME];
  char attstring[8192];

  for (int ncvarid = 0; ncvarid < nvars; ncvarid++)
  {
    ncvar_t *ncvar = &ncvars[ncvarid];
    int ncid = ncvar->ncid;
    const int *dimidsp = ncvar->dimids;
    int nvdims = ncvar->ndims;
    int nvatts = ncvar->natts;

    for (int i = 0; i < nvatts; i++)
    {
      int attnum = ncvar->atts[i];
      cdf_inq_attname(ncid, ncvarid, attnum, attname);
      cdf_inq_attlen(ncid, ncvarid, attname, &attlen);
      cdf_inq_atttype(ncid, ncvarid, attname, &atttype);

      size_t attstringsize = sizeof(attstring);
      // bool isNumber = (xtypeIsFloat(atttype) || xtypeIsInt(atttype));

      bool isText = xtypeIsText(atttype);
      if (isText)
      {
        cdfGetAttText(ncid, ncvarid, attname, sizeof(attstring), attstring);
        attstringsize = strlen(attstring) + 1;
        if (attstringsize > CDI_MAX_NAME)
        {
          attstringsize = CDI_MAX_NAME;
        }

        if (str_is_equal(attname, "axis"))
        {
          cdf_scan_attr_axis(ncvars, ncdims, ncvarid, attstring, nvdims, dimidsp);
        }
        // else if (str_is_equal(attname, "standard_name")) { memcpy(ncvar->stdname, attstring, attstringsize); }
      }
    }
  }
}

static void
find_dimtypes(ncvar_t *ncvars, ncvar_t *ncvar, bool *plxdim, bool *plydim, bool *plzdim, int *plcdim)
{
  bool lxdim = false, lydim = false, lzdim = false /*, ltdim = false */;
  int lcdim = 0;
  for (int i = 0, n = ncvar->ndims; i < n; i++)
  {
    int dimtype = ncvar->dimtypes[i];
    lxdim |= (dimtype == X_AXIS);
    lydim |= (dimtype == Y_AXIS);
    lzdim |= (dimtype == Z_AXIS);
    if (ncvar->cvarids[i] != CDI_UNDEFID)
      lcdim++;
    // ltdim |= (dimtype == T_AXIS);
  }

  if (!lxdim && ncvar->xvarid != CDI_UNDEFID && ncvars[ncvar->xvarid].ndims == 0)
    lxdim = true;
  if (!lydim && ncvar->yvarid != CDI_UNDEFID && ncvars[ncvar->yvarid].ndims == 0)
    lydim = true;

  *plxdim = lxdim;
  *plydim = lydim;
  *plzdim = lzdim;
  *plcdim = lcdim;
}

static void
cdf_set_dimtype(int numVars, ncvar_t *ncvars, ncdim_t *ncdims)
{
  for (int varId = 0; varId < numVars; ++varId)
  {
    ncvar_t *ncvar = &ncvars[varId];
    if (ncvar->varStatus == DataVar)
    {
      for (int i = 0, n = ncvar->ndims; i < n; ++i)
      {
        int ncdimid = ncvar->dimids[i];
        int dimtype = ncdims[ncdimid].dimtype;
        if (dimtype >= X_AXIS && dimtype <= T_AXIS)
          cdf_set_dim(ncvar, i, dimtype);
      }

      if (CDI_Debug)
      {
        Message("var %d %s", varId, ncvar->name);
        for (int i = 0, n = ncvar->ndims; i < n; i++)
          printf("  dim%d type=%d  ", i, ncvar->dimtypes[i]);
        printf("\n");
      }
    }
  }

  for (int varId = 0; varId < numVars; ++varId)
  {
    ncvar_t *ncvar = &ncvars[varId];
    if (ncvar->varStatus == DataVar)
    {
      bool lxdim = false, lydim = false, lzdim = false /* , ltdim = false */;
      int lcdim;
      find_dimtypes(ncvars, ncvar, &lxdim, &lydim, &lzdim, &lcdim);
      int allcdims = lcdim;
      int ndims = ncvar->ndims;

      if (lxdim && (lydim || ncvar->gridtype == GRID_UNSTRUCTURED))
        for (int i = ndims - 1; i >= 0; i--)
        {
          if (ncvar->dimtypes[i] == -1 && !lzdim)
          {
            if (lcdim)
            {
              int cdimvar = ncvar->cvarids[allcdims - lcdim];
              ncvar->zvarid = cdimvar;
              lcdim--;
              ncvars[cdimvar].zaxistype = ZAXIS_CHAR;
            }
            cdf_set_dim(ncvar, i, Z_AXIS);
            lzdim = true;
            int ncdimid = ncvar->dimids[i];
            if (ncdims[ncdimid].dimtype == CDI_UNDEFID)
              ncdims[ncdimid].dimtype = Z_AXIS;
          }
        }
    }
  }

  for (int varId = 0; varId < numVars; ++varId)
  {
    ncvar_t *ncvar = &ncvars[varId];
    for (int i = 0, n = ncvar->ndims; i < n; ++i)
    {
      if (ncvar->dimtypes[i] == CDI_UNDEFID)
      {
        int ncdimid = ncvar->dimids[i];
        if (ncdims[ncdimid].dimtype == Z_AXIS)
        {
          ncvar->isZaxis = true;
          cdf_set_dim(ncvar, i, Z_AXIS);
        }
      }
    }
  }

  for (int varId = 0; varId < numVars; ++varId)
  {
    ncvar_t *ncvar = &ncvars[varId];
    if (ncvar->varStatus == DataVar)
    {
      bool lxdim = false, lydim = false, lzdim = false /*, ltdim = false */;
      int lcdim = 0;
      find_dimtypes(ncvars, ncvar, &lxdim, &lydim, &lzdim, &lcdim);
      int allcdims = lcdim;
      int ndims = ncvar->ndims;

      //   if ( ndims > 1 )
      for (int i = ndims - 1; i >= 0; i--)
      {
        if (ncvar->dimtypes[i] == -1)
        {
          int dimtype;
          if (!lxdim)
          {
            if (lcdim && ncvar->xvarid == CDI_UNDEFID)
            {
              int cdimvar = ncvar->cvarids[allcdims - lcdim];
              ncvar->xvarid = cdimvar;
              lcdim--;
            }
            dimtype = X_AXIS;
            lxdim = true;
          }
          else if (!lydim && ncvar->gridtype != GRID_UNSTRUCTURED && ncvar->isHealpixMapping == false)
          // else if ( !lydim && ! (ncvars[ncvar->xvarid].dimids[0] == ncvars[ncvar->yvarid].dimids[0] &&
          //                        ncvars[ncvar->xvarid].ndims == 1 && ncvars[ncvar->yvarid].ndims == 1))
          {
            if (lcdim && ncvar->yvarid == CDI_UNDEFID)
            {
              int cdimvar = ncvar->cvarids[allcdims - lcdim];
              ncvar->yvarid = cdimvar;
              lcdim--;
            }
            dimtype = Y_AXIS;
            lydim = true;
          }
          else if (!lzdim)
          {
            if (lcdim > 0)
            {
              int cdimvar = ncvar->cvarids[allcdims - lcdim];
              ncvar->zvarid = cdimvar;
              lcdim--;
              ncvars[cdimvar].zaxistype = ZAXIS_CHAR;
            }
            dimtype = Z_AXIS;
            lzdim = true;
          }
          else
            continue;
          cdf_set_dim(ncvar, i, dimtype);
        }
      }
      // if (lcdim > 0) Warning("Could not assign all character coordinates to data variable!");
    }
  }
}

static void
set_vardim_coord(ncvar_t *ncvar, ncdim_t *ncdim, int axisType)
{
  cdf_set_var(ncvar, CoordVar);
  cdf_set_dim(ncvar, 0, axisType);
  ncdim->dimtype = axisType;
}

// verify coordinates vars - first scan (dimname == varname)
static void
verify_coordinates_vars_1(int ncid, int ndims, ncdim_t *ncdims, ncvar_t *ncvars, int timedimid, bool *isHybridCF)
{
  for (int ncdimid = 0; ncdimid < ndims; ncdimid++)
  {
    ncdim_t *ncdim = &ncdims[ncdimid];
    int ncvarid = ncdim->ncvarid;
    if (ncvarid != -1)
    {
      ncvar_t *ncvar = &ncvars[ncvarid];
      if (ncvar->dimids[0] == timedimid)
      {
        ncvar->isTaxis = true;
        ncdim->dimtype = T_AXIS;
        continue;
      }

      if (isHybridSigmaPressureCoordinate(ncid, ncvarid, ncvars, ncdims))
      {
        *isHybridCF = true;
        continue;
      }

      if (ncvar->units[0] != 0)
      {
        if (is_lon_axis(ncvar->units, ncvar->stdname))
        {
          ncvar->isLon = true;
          set_vardim_coord(ncvar, ncdim, X_AXIS);
        }
        else if (is_lat_axis(ncvar->units, ncvar->stdname))
        {
          ncvar->isLat = true;
          set_vardim_coord(ncvar, ncdim, Y_AXIS);
        }
        else if (is_x_axis(ncvar->units, ncvar->stdname))
        {
          ncvar->isXaxis = true;
          set_vardim_coord(ncvar, ncdim, X_AXIS);
        }
        else if (is_y_axis(ncvar->units, ncvar->stdname))
        {
          ncvar->isYaxis = true;
          set_vardim_coord(ncvar, ncdim, Y_AXIS);
        }
        else if (is_pressure_units(ncvar->units))
        {
          ncvar->zaxistype = ZAXIS_PRESSURE;
        }
        else if (str_is_equal(ncvar->units, "level") || str_is_equal(ncvar->units, "1"))
        {
          // clang-format off
          if      (str_is_equal(ncvar->longname, "hybrid level at layer midpoints"))        ncvar->zaxistype = ZAXIS_HYBRID;
          else if (str_is_equal(ncvar->longname, "hybrid model level at layer midpoints"))  ncvar->zaxistype = ZAXIS_HYBRID;
          else if (strStartsWith(ncvar->longname, "hybrid level at midpoints"))             ncvar->zaxistype = ZAXIS_HYBRID;
          else if (str_is_equal(ncvar->longname, "hybrid level at layer interfaces"))       ncvar->zaxistype = ZAXIS_HYBRID_HALF;
          else if (str_is_equal(ncvar->longname, "hybrid model level at layer interfaces")) ncvar->zaxistype = ZAXIS_HYBRID_HALF;
          else if (strStartsWith(ncvar->longname, "hybrid level at interfaces"))            ncvar->zaxistype = ZAXIS_HYBRID_HALF;
          else if (str_is_equal(ncvar->units, "level"))                                     ncvar->zaxistype = ZAXIS_GENERIC;
          // clang-format on
        }
        else if (is_DBL_axis(ncvar->longname))
        {
          ncvar->zaxistype = ZAXIS_DEPTH_BELOW_LAND;
        }
        else if (is_height_units(ncvar->units))
        {
          // clang-format off
          if      (is_depth_axis(ncvar->stdname, ncvar->longname))    ncvar->zaxistype = ZAXIS_DEPTH_BELOW_SEA;
          else if (is_height_axis(ncvar->stdname, ncvar->longname))   ncvar->zaxistype = ZAXIS_HEIGHT;
          else if (is_altitude_axis(ncvar->stdname, ncvar->longname)) ncvar->zaxistype = ZAXIS_ALTITUDE;
          // clang-format on
        }
      }
      else
      {
        // clang-format off
        if      (is_reference_axis(ncvar->stdname, ncvar->longname)) ncvar->zaxistype = ZAXIS_REFERENCE;
        else if (str_is_equal(ncvar->stdname, "air_pressure"))       ncvar->zaxistype = ZAXIS_PRESSURE;
        // clang-format on
      }

      if (!ncvar->isLon && (ncvar->longname[0] != 0) && !ncvar->isLat && (ncvar->longname[1] != 0))
      {
        if (strStartsWith(ncvar->longname + 1, "ongitude"))
        {
          ncvar->isLon = true;
          set_vardim_coord(ncvar, ncdim, X_AXIS);
          continue;
        }
        else if (strStartsWith(ncvar->longname + 1, "atitude"))
        {
          ncvar->isLat = true;
          set_vardim_coord(ncvar, ncdim, Y_AXIS);
          continue;
        }
      }

      if (ncvar->zaxistype != CDI_UNDEFID)
      {
        ncvar->isZaxis = true;
        set_vardim_coord(ncvar, ncdim, Z_AXIS);
      }
    }
  }
}

// verify coordinates vars - second scan (all other variables)
static void
verify_coordinates_vars_2(stream_t *streamptr, int nvars, ncvar_t *ncvars)
{
  for (int ncvarid = 0; ncvarid < nvars; ncvarid++)
  {
    ncvar_t *ncvar = &ncvars[ncvarid];
    if (ncvar->varStatus == CoordVar)
    {
      if (ncvar->units[0] != 0)
      {
        if (is_lon_axis(ncvar->units, ncvar->stdname))
        {
          ncvar->isLon = true;
          continue;
        }
        else if (is_lat_axis(ncvar->units, ncvar->stdname))
        {
          ncvar->isLat = true;
          continue;
        }
        else if (is_x_axis(ncvar->units, ncvar->stdname))
        {
          ncvar->isXaxis = true;
          continue;
        }
        else if (is_y_axis(ncvar->units, ncvar->stdname))
        {
          ncvar->isYaxis = true;
          continue;
        }
        else if (str_is_equal(ncvar->stdname, "healpix_index"))
        {
          ncvar->isIndexAxis = true;
          continue;
        }
        else if (ncvar->zaxistype == CDI_UNDEFID && (str_is_equal(ncvar->units, "level") || str_is_equal(ncvar->units, "1")))
        {
          // clang-format off
          if      (str_is_equal(ncvar->longname, "hybrid level at layer midpoints"))        ncvar->zaxistype = ZAXIS_HYBRID;
          else if (str_is_equal(ncvar->longname, "hybrid model level at layer midpoints"))  ncvar->zaxistype = ZAXIS_HYBRID;
          else if (strStartsWith(ncvar->longname, "hybrid level at midpoints"))             ncvar->zaxistype = ZAXIS_HYBRID;
          else if (str_is_equal(ncvar->longname, "hybrid level at layer interfaces"))       ncvar->zaxistype = ZAXIS_HYBRID_HALF;
          else if (str_is_equal(ncvar->longname, "hybrid model level at layer interfaces")) ncvar->zaxistype = ZAXIS_HYBRID_HALF;
          else if (strStartsWith(ncvar->longname, "hybrid level at interfaces"))            ncvar->zaxistype = ZAXIS_HYBRID_HALF;
          else if (str_is_equal(ncvar->units, "level"))                                     ncvar->zaxistype = ZAXIS_GENERIC;
          // clang-format on
          continue;
        }
        else if (ncvar->zaxistype == CDI_UNDEFID && is_pressure_units(ncvar->units))
        {
          ncvar->zaxistype = ZAXIS_PRESSURE;
          continue;
        }
        else if (is_DBL_axis(ncvar->longname))
        {
          ncvar->zaxistype = ZAXIS_DEPTH_BELOW_LAND;
          continue;
        }
        else if (is_height_units(ncvar->units))
        {
          // clang-format off
          if      (is_depth_axis(ncvar->stdname, ncvar->longname))  ncvar->zaxistype = ZAXIS_DEPTH_BELOW_SEA;
          else if (is_height_axis(ncvar->stdname, ncvar->longname)) ncvar->zaxistype = ZAXIS_HEIGHT;
          // clang-format on
          continue;
        }
      }
      else if (str_is_equal(ncvar->stdname, "region") || str_is_equal(ncvar->stdname, "area_type") || cdfInqDatatype(streamptr, ncvar->xtype, ncvar->isUnsigned) == CDI_DATATYPE_UINT8)
      {
        ncvar->isCharAxis = true;
      }
      else if (str_is_equal(ncvar->stdname, "air_pressure"))
      {
        ncvar->zaxistype = ZAXIS_PRESSURE;
      }

      // not needed anymore for rotated grids
      if (!ncvar->isLon && (ncvar->longname[0] != 0) && !ncvar->isLat && (ncvar->longname[1] != 0))
      {
        if (strStartsWith(ncvar->longname + 1, "ongitude"))
        {
          ncvar->isLon = true;
          continue;
        }
        else if (strStartsWith(ncvar->longname + 1, "atitude"))
        {
          ncvar->isLat = true;
          continue;
        }
      }
    }
  }
}

static void
grid_set_chunktype(grid_t *grid, ncvar_t *ncvar)
{
  if (ncvar->isChunked)
  {
    int ndims = ncvar->ndims;
    size_t chunkSizeAllDims = 1;
    for (int i = 0; i < ndims; ++i)
      chunkSizeAllDims *= ncvar->chunks[i];

    size_t dimN = ncvar->chunks[ndims - 1];
    if (grid->type == GRID_UNSTRUCTURED)
    {
      size_t chunkSize = (chunkSizeAllDims == dimN) ? dimN : 0;
      ncvar->chunkType = (chunkSize == grid->size) ? CDI_CHUNK_GRID : CDI_CHUNK_AUTO;
      if (ncvar->chunkType == CDI_CHUNK_AUTO && chunkSize > 1)
        ncvar->chunkSize = (int)chunkSize;
    }
    else
    {
      if (grid->x.size > 1 && grid->y.size > 1 && ndims > 1 && grid->x.size == dimN && grid->y.size == ncvar->chunks[ndims - 2])
        ncvar->chunkType = CDI_CHUNK_GRID;
      else if (grid->x.size > 1 && grid->x.size == dimN && chunkSizeAllDims == dimN)
        ncvar->chunkType = CDI_CHUNK_LINES;
      else
        ncvar->chunkType = CDI_CHUNK_AUTO;
    }
  }
}

// define all input grids
static void
cdf_load_vals(size_t size, int ndims, int varid, ncvar_t *ncvar, double **gridvals, struct xyValGet *valsGet, bool hasTimeDim,
              bool readPart, size_t *start, size_t *count)
{
  if (CDI_Netcdf_Lazy_Grid_Load)
  {
    *valsGet = (struct xyValGet){
        .scalefactor = ncvar->scalefactor,
        .addoffset = ncvar->addoffset,
        .start = {start[0], start[1], start[2]},
        .count = {count[0], count[1], count[2]},
        .size = size,
        .datasetNCId = ncvar->ncid,
        .varNCId = varid,
        .ndims = (short)ndims,
    };
    *gridvals = cdfPendingLoad;
  }
  else
  {
    *gridvals = (double *)Malloc(size * sizeof(double));
    if (hasTimeDim || readPart)
      cdf_get_vara_double(ncvar->ncid, varid, start, count, *gridvals);
    else
      cdf_get_var_double(ncvar->ncid, varid, *gridvals);
    cdf_scale_add(size, *gridvals, ncvar->addoffset, ncvar->scalefactor);
  }
}

#ifndef USE_MPI
static void
cdf_load_cvals(size_t size, int varid, ncvar_t *ncvar, char ***gridvals, size_t dimlength)
{
  size_t startc[] = {0, 0};
  size_t countc[] = {1, size / dimlength};
  *gridvals = (char **)Malloc(dimlength * sizeof(char *));
  for (size_t i = 0; i < dimlength; i++)
  {
    (*gridvals)[i] = (char *)Malloc((size / dimlength) * sizeof(char));
    cdf_get_vara_text(ncvar->ncid, varid, startc, countc, (*gridvals)[i]);
    startc[0] = i + 1;
  }
}
#endif

static void
cdf_load_bounds(size_t size, ncvar_t *ncvar, double **gridbounds, struct cdfLazyGridIds *cellBoundsGet, bool readPart,
                size_t *start, size_t *count)
{
  if (CDI_Netcdf_Lazy_Grid_Load)
  {
    cellBoundsGet->datasetNCId = ncvar->ncid;
    cellBoundsGet->varNCId = ncvar->bounds;
    *gridbounds = cdfPendingLoad;
  }
  else
  {
    *gridbounds = (double *)Malloc(size * sizeof(double));
    if (readPart)
      cdf_get_vara_double(ncvar->ncid, ncvar->bounds, start, count, *gridbounds);
    else
      cdf_get_var_double(ncvar->ncid, ncvar->bounds, *gridbounds);
  }
}

static void
cdf_load_bounds_cube_sphere(size_t bxsize, size_t bysize, size_t size, ncvar_t *ncvar, double **gridbounds,
                            struct cdfLazyGridIds *cellBoundsGet)
{
  if (CDI_Netcdf_Lazy_Grid_Load)
  {
    cellBoundsGet->datasetNCId = ncvar->ncid;
    cellBoundsGet->varNCId = ncvar->bounds;
    *gridbounds = cdfPendingLoad;
  }
  else
  {
    float *bounds = (float *)Malloc(6 * bxsize * bysize * sizeof(float));
    cdf_get_var_float(ncvar->ncid, ncvar->bounds, bounds);

    *gridbounds = (double *)Malloc(size * sizeof(double));
    double *pbounds = *gridbounds;

    size_t m = 0;
    for (size_t k = 0; k < 6; ++k)
      for (size_t j = 0; j < (bysize - 1); ++j)
        for (size_t i = 0; i < (bxsize - 1); ++i)
        {
          size_t offset = k * bysize * bxsize;
          pbounds[m + 0] = bounds[offset + (j + 1) * bxsize + i];
          pbounds[m + 1] = bounds[offset + j * bxsize + i];
          pbounds[m + 2] = bounds[offset + j * bxsize + (i + 1)];
          pbounds[m + 3] = bounds[offset + (j + 1) * bxsize + (i + 1)];
          m += 4;
        }

    Free(bounds);
  }
}

static void
cdf_load_cellarea(size_t size, ncvar_t *ncvar, double **gridarea, struct cdfLazyGridIds *cellAreaGet)
{
  if (CDI_Netcdf_Lazy_Grid_Load)
  {
    cellAreaGet->datasetNCId = ncvar->ncid;
    cellAreaGet->varNCId = ncvar->cellarea;
    *gridarea = cdfPendingLoad;
  }
  else
  {
    *gridarea = (double *)Malloc(size * sizeof(double));
    cdf_get_var_double(ncvar->ncid, ncvar->cellarea, *gridarea);
  }
}

static void
cdf_load_cellindices(size_t size, ncvar_t *ncvar, int64_t **cellIndices)
{
  *cellIndices = (int64_t *)Malloc(size * sizeof(int64_t));
  cdf_get_var_int64(ncvar->ncid, ncvar->ivarid, *cellIndices);
}

static void
cdf_copy_grid_axis_attr(ncvar_t *ncvar, struct gridaxis_t *gridaxis)
{
  cdiDefVarKeyBytes(&gridaxis->keys, CDI_KEY_NAME, (const unsigned char *)ncvar->name, (int)strlen(ncvar->name) + 1);
  if (ncvar->longname[0])
    cdiDefVarKeyBytes(&gridaxis->keys, CDI_KEY_LONGNAME, (const unsigned char *)ncvar->longname,
                      (int)strlen(ncvar->longname) + 1);
  if (ncvar->units[0])
    cdiDefVarKeyBytes(&gridaxis->keys, CDI_KEY_UNITS, (const unsigned char *)ncvar->units, (int)strlen(ncvar->units) + 1);
#ifndef USE_MPI
  if (gridaxis->cvals)
    if (ncvar->stdname[0])
      cdiDefVarKeyBytes(&gridaxis->keys, CDI_KEY_STDNAME, (const unsigned char *)ncvar->stdname, (int)strlen(ncvar->stdname) + 1);
#endif
}

static int
cdf_get_xydimid(int ndims, int *dimids, int *dimtypes, int *xdimid, int *ydimid)
{
  int nxdims = 0, nydims = 0;
  int xdimids[2] = {-1, -1}, ydimids[2] = {-1, -1};

  for (int i = 0; i < ndims; i++)
  {
    if (dimtypes[i] == X_AXIS && nxdims < 2)
    {
      xdimids[nxdims] = dimids[i];
      nxdims++;
    }
    else if (dimtypes[i] == Y_AXIS && nydims < 2)
    {
      ydimids[nydims] = dimids[i];
      nydims++;
    }
  }

  if (nxdims == 2)
  {
    *xdimid = xdimids[1];
    *ydimid = xdimids[0];
  }
  else if (nydims == 2)
  {
    *xdimid = ydimids[1];
    *ydimid = ydimids[0];
  }
  else
  {
    *xdimid = xdimids[0];
    *ydimid = ydimids[0];
  }

  return nydims;
}

static void
cdf_check_gridtype(int *gridtype, bool isLon, bool isLat, size_t xsize, size_t ysize, grid_t *grid)
{
  if (grid->y.vals == NULL)
  {
    *gridtype = GRID_GENERIC;
    return;
  }

  if (isLat && (isLon || xsize == 0))
  {
    double yinc = 0.0;
    if (isLon && ysize > 1)
    {
      yinc = fabs(grid->y.vals[0] - grid->y.vals[1]);
      for (size_t i = 2; i < ysize; i++)
        if ((fabs(grid->y.vals[i - 1] - grid->y.vals[i]) - yinc) > (yinc / 1000))
        {
          yinc = 0;
          break;
        }
    }
    if (ysize < 10000 && IS_EQUAL(yinc, 0.0) && isGaussianLatitudes(ysize, grid->y.vals))
    {
      *gridtype = GRID_GAUSSIAN;
      grid->np = (int)(ysize / 2);
    }
    else
    {
      *gridtype = GRID_LONLAT;
    }
  }
  else
  {
    *gridtype = (isLon && !isLat && ysize == 0) ? GRID_LONLAT : GRID_GENERIC;
  }
}

static bool
cdf_read_xcoord(stream_t *streamptr, struct cdfLazyGrid *lazyGrid, ncdim_t *ncdims, ncvar_t *ncvar, int xvarid, ncvar_t *axisvar,
                size_t *xsize, size_t ysize, bool hasTimeDim, bool readPart, size_t *start, size_t *count, bool *isLon)
{
  grid_t *grid = &lazyGrid->base;
  bool skipvar = true;
  *isLon = axisvar->isLon;
  int ndims = axisvar->ndims;
  size_t size = 0;

  if (ndims == 1 && xtypeIsText(axisvar->xtype))
  {
    ncvar->varStatus = CoordVar;
    Warning("Unsupported x-coordinate type (char/string), skipped variable %s!", ncvar->name);
    return true;
  }

  int datatype = cdfInqDatatype(streamptr, axisvar->xtype, axisvar->isUnsigned);

  if ((ndims - hasTimeDim) == 2)
  {
    // Check size of 2 dimensional coordinates variables
    int dimid = axisvar->dimids[ndims - 2];
    size_t dimsize1 = ncdims[dimid].len;
    dimid = axisvar->dimids[ndims - 1];
    size_t dimsize2 = ncdims[dimid].len;

    if (datatype == CDI_DATATYPE_UINT8)
    {
      ncvar->gridtype = GRID_CHARXY;
      size = dimsize1 * dimsize2;
      skipvar = (dimsize1 != *xsize);
    }
    else
    {
      ncvar->gridtype = GRID_CURVILINEAR;
      size = (*xsize) * ysize;
      skipvar = (dimsize1 * dimsize2 != size);
    }
  }
  else if ((ndims - hasTimeDim) == 1)
  {
    size = *xsize;
    // Check size of 1 dimensional coordinates variables
    int dimid = axisvar->dimids[ndims - 1];
    size_t dimsize = ncdims[dimid].len;
    skipvar = readPart ? false : (dimsize != size);
  }
  else if (ndims == 0 && *xsize == 0)
  {
    size = *xsize = 1;
    skipvar = false;
  }
  else if (ncvar->isCubeSphere)
  {
    size = *xsize;
    skipvar = false;
  }

  if (skipvar)
  {
    Warning("Unsupported array structure, skipped variable %s!", ncvar->name);
    ncvar->varStatus = UndefVar;
    return true;
  }

  if (datatype != -1)
    grid->datatype = datatype;

  if (datatype == CDI_DATATYPE_UINT8 && !CDI_Netcdf_Lazy_Grid_Load)
  {
#ifndef USE_MPI
    cdf_load_cvals(size, xvarid, axisvar, &grid->x.cvals, *xsize);
    grid->x.clength = size / (*xsize);
#endif
  }
  else if (CDI_Read_Cell_Center)
  {
    cdf_load_vals(size, ndims, xvarid, axisvar, &grid->x.vals, &lazyGrid->xValsGet, hasTimeDim, readPart, start, count);
  }

  cdf_copy_grid_axis_attr(axisvar, &grid->x);

  return false;
}

static bool
cdf_read_ycoord(stream_t *streamptr, struct cdfLazyGrid *lazyGrid, ncdim_t *ncdims, ncvar_t *ncvar, int yvarid, ncvar_t *axisvar,
                size_t xsize, size_t *ysize, bool hasTimeDim, bool readPart, size_t *start, size_t *count, bool *isLat)
{
  grid_t *grid = &lazyGrid->base;
  bool skipvar = true;
  *isLat = axisvar->isLat;
  int ndims = axisvar->ndims;
  size_t size = 0;

  if (ndims == 1 && xtypeIsText(axisvar->xtype))
  {
    ncvar->varStatus = CoordVar;
    Warning("Unsupported y-coordinate type (char/string), skipped variable %s!", ncvar->name);
    return true;
  }

  int datatype = cdfInqDatatype(streamptr, axisvar->xtype, axisvar->isUnsigned);

  if ((ndims - hasTimeDim) == 2)
  {
    // Check size of 2 dimensional coordinates variables
    int dimid = axisvar->dimids[ndims - 2];
    size_t dimsize1 = ncdims[dimid].len;
    dimid = axisvar->dimids[ndims - 1];
    size_t dimsize2 = ncdims[dimid].len;

    if (datatype == CDI_DATATYPE_UINT8)
    {
      ncvar->gridtype = GRID_CHARXY;
      size = dimsize1 * dimsize2;
      skipvar = (dimsize1 != *ysize);
    }
    else
    {
      ncvar->gridtype = GRID_CURVILINEAR;
      size = xsize * (*ysize);
      skipvar = (dimsize1 * dimsize2 != size);
    }
  }
  else if ((ndims - hasTimeDim) == 1)
  {
    size = (*ysize) ? *ysize : xsize;
    int dimid = axisvar->dimids[ndims - 1];
    size_t dimsize = ncdims[dimid].len;
    skipvar = readPart ? false : (dimsize != size);
  }
  else if (ndims == 0 && *ysize == 0)
  {
    size = *ysize = 1;
    skipvar = false;
  }
  else if (ncvar->isCubeSphere)
  {
    size = *ysize;
    skipvar = false;
  }

  if (skipvar)
  {
    Warning("Unsupported array structure, skipped variable %s!", ncvar->name);
    ncvar->varStatus = UndefVar;
    return true;
  }

  if (datatype != -1)
    grid->datatype = datatype;

  if (datatype == CDI_DATATYPE_UINT8 && !CDI_Netcdf_Lazy_Grid_Load)
  {
#ifndef USE_MPI
    cdf_load_cvals(size, yvarid, axisvar, &grid->y.cvals, *ysize);
    grid->y.clength = size / (*ysize);
#endif
  }
  else if (CDI_Read_Cell_Center)
  {
    cdf_load_vals(size, ndims, yvarid, axisvar, &grid->y.vals, &lazyGrid->yValsGet, hasTimeDim, readPart, start, count);
  }

  cdf_copy_grid_axis_attr(axisvar, &grid->y);

  return false;
}

typedef struct
{
  long start;
  long count;
  bool readPart;
} GridPart;

static void
gridpart_init(GridPart *gridPart)
{
  gridPart->start = -1;
  gridPart->count = -1;
  gridPart->readPart = false;
}

static void
cdf_load_xbounds(struct cdfLazyGrid *lazyGrid, ncvar_t *ncvar, ncvar_t *ncvars, ncdim_t *ncdims, int timedimid, int xvarid,
                 int *vdimid, bool readPart, size_t *start, size_t *count)
{
  grid_t *grid = &lazyGrid->base;
  size_t size = grid->size;
  grid->x.flag = 1;
  int bvarid = ncvars[xvarid].bounds;
  if (bvarid != CDI_UNDEFID)
  {
    int ndims = ncvars[xvarid].ndims;
    int nbdims = ncvars[bvarid].ndims;
    if (nbdims == 2 || nbdims == 3)
    {
      if (ncvars[bvarid].dimids[0] == timedimid)
      {
        static bool ltwarn = true;
        if (ltwarn)
          Warning("Time varying grid x-bounds unsupported, skipped!");
        ltwarn = false;
      }
      else if (ncvar->isCubeSphere)
      {
        grid->nvertex = 4;
        size_t bxsize = ncdims[ncvars[bvarid].dimids[2]].len;
        size_t bysize = ncdims[ncvars[bvarid].dimids[1]].len;
        cdf_load_bounds_cube_sphere(bxsize, bysize, size * (size_t)grid->nvertex, &ncvars[xvarid], &grid->x.bounds,
                                    &lazyGrid->xBoundsGet);
      }
      else if (nbdims == ndims + 1)
      {
        *vdimid = ncvars[bvarid].dimids[nbdims - 1];
        grid->nvertex = (int)ncdims[*vdimid].len;
        if (readPart)
        {
          start[1] = 0;
          count[1] = (size_t)grid->nvertex;
        }
        cdf_load_bounds(size * (size_t)grid->nvertex, &ncvars[xvarid], &grid->x.bounds, &lazyGrid->xBoundsGet, readPart, start,
                        count);
      }
      else
      {
        static bool lwarn = true;
        if (lwarn)
          Warning("x-bounds doesn't follow the CF-Convention, skipped!");
        lwarn = false;
      }
    }
  }
}

static void
cdf_load_ybounds(struct cdfLazyGrid *lazyGrid, ncvar_t *ncvar, ncvar_t *ncvars, ncdim_t *ncdims, int timedimid, int yvarid,
                 int *vdimid, bool readPart, size_t *start, size_t *count)
{
  grid_t *grid = &lazyGrid->base;
  size_t size = grid->size;
  grid->y.flag = 1;
  int bvarid = ncvars[yvarid].bounds;
  if (bvarid != CDI_UNDEFID)
  {
    int ndims = ncvars[yvarid].ndims;
    int nbdims = ncvars[bvarid].ndims;
    if (nbdims == 2 || nbdims == 3)
    {
      if (ncvars[bvarid].dimids[0] == timedimid)
      {
        static bool ltwarn = true;
        if (ltwarn)
          Warning("Time varying grid y-bounds unsupported, skipped!");
        ltwarn = false;
      }
      else if (ncvar->isCubeSphere)
      {
        grid->nvertex = 4;
        size_t bxsize = ncdims[ncvars[bvarid].dimids[2]].len;
        size_t bysize = ncdims[ncvars[bvarid].dimids[1]].len;
        cdf_load_bounds_cube_sphere(bxsize, bysize, size * (size_t)grid->nvertex, &ncvars[yvarid], &grid->y.bounds,
                                    &lazyGrid->yBoundsGet);
      }
      else if (nbdims == ndims + 1)
      {
        if (*vdimid == CDI_UNDEFID)
        {
          *vdimid = ncvars[bvarid].dimids[nbdims - 1];
          grid->nvertex = (int)ncdims[*vdimid].len;
        }
        if (readPart)
        {
          start[1] = 0;
          count[1] = (size_t)grid->nvertex;
        }
        cdf_load_bounds(size * (size_t)grid->nvertex, &ncvars[yvarid], &grid->y.bounds, &lazyGrid->yBoundsGet, readPart, start,
                        count);
      }
      else
      {
        static bool lwarn = true;
        if (lwarn)
          Warning("y-bounds doesn't follow the CF-Convention, skipped!");
        lwarn = false;
      }
    }
  }
}

static void
cdf_load_ybounds_reduced(struct cdfLazyGrid *lazyGrid, ncvar_t *ncvar, ncvar_t *ncvars, ncdim_t *ncdims, int yvarid, int *vdimid)
{
  grid_t *grid = &lazyGrid->base;
  size_t size = grid->size;
  int dimid = ncvars[ncvar->rpvarid].dimids[0];
  size_t len = ncdims[dimid].len;
  grid->y.size = len;
  assert(len <= INT_MAX);
  grid->reducedPointsSize = (int)len;
  grid->reducedPoints = (int *)Malloc(len * sizeof(int));
  cdf_get_var_int(ncvar->ncid, ncvar->rpvarid, grid->reducedPoints);
  grid->np = ncvar->numLPE;

  int bvarid = (yvarid == CDI_UNDEFID) ? CDI_UNDEFID : ncvars[yvarid].bounds;
  if (bvarid != CDI_UNDEFID)
  {
    int nbdims = ncvars[bvarid].ndims;
    if (nbdims == 2 || nbdims == 3)
    {
      if (*vdimid == CDI_UNDEFID)
      {
        *vdimid = ncvars[bvarid].dimids[nbdims - 1];
        grid->nvertex = (int)ncdims[*vdimid].len;
      }
      cdf_load_bounds(size * (size_t)grid->nvertex, &ncvars[yvarid], &grid->y.bounds, &lazyGrid->yBoundsGet, false, NULL, NULL);
    }
  }
}

static bool
cdf_read_coordinates(stream_t *streamptr, struct cdfLazyGrid *lazyGrid, ncvar_t *ncvar, ncvar_t *ncvars, ncdim_t *ncdims,
                     int timedimid, int xvarid, int yvarid, size_t xsize, size_t ysize, int *vdimid, const GridPart *gridPart)
{
  grid_t *grid = &lazyGrid->base;
  size_t size = 0;
  size_t start[3] = {0, 0, 0}, count[3] = {1, 1, 1};
  bool readPart = false;

  grid->datatype = CDI_DATATYPE_FLT64;

  if (ncvar->gridtype == GRID_TRAJECTORY)
  {
    if (ncvar->xvarid == CDI_UNDEFID)
      Error("Longitude coordinates undefined for %s!", ncvar->name);
    if (ncvar->yvarid == CDI_UNDEFID)
      Error("Latitude coordinates undefined for %s!", ncvar->name);
  }
  else
  {
    bool hasTimeDim = false;

    if (xvarid != CDI_UNDEFID && yvarid != CDI_UNDEFID)
    {
      int ndims = ncvars[xvarid].ndims;
      if (ndims != ncvars[yvarid].ndims && !ncvars[xvarid].isCharAxis && !ncvars[yvarid].isCharAxis)
      {
        Warning("Inconsistent grid structure for variable %s!", ncvar->name);
        ncvar->xvarid = xvarid = CDI_UNDEFID;
        ncvar->yvarid = yvarid = CDI_UNDEFID;
      }
      if (ndims > 1)
      {
        if (ndims <= 3)
        {
          if (ncvars[xvarid].dimids[0] == timedimid && ncvars[yvarid].dimids[0] == timedimid)
          {
            static bool ltwarn = true;
            size_t ntsteps = 0;
            cdf_inq_dimlen(ncvar->ncid, ncdims[timedimid].dimid, &ntsteps);
            if (ltwarn && ntsteps > 1)
              Warning("Time varying grids unsupported, using grid at time step 1!");
            ltwarn = false;
            hasTimeDim = true;
            count[1] = ysize;
            count[2] = xsize;
          }
        }
        else
        {
          Warning("Unsupported grid structure for variable %s (grid dims > 2)!", ncvar->name);
          ncvar->xvarid = xvarid = CDI_UNDEFID;
          ncvar->yvarid = yvarid = CDI_UNDEFID;
        }
      }
      else if (gridPart && gridPart->readPart)
      {
        start[0] = (size_t)gridPart->start;
        count[0] = (size_t)gridPart->count;
        readPart = true;
      }
    }

    if (xvarid != CDI_UNDEFID)
    {
      if (!ncvar->isCubeSphere && (ncvars[xvarid].ndims - hasTimeDim) > 2)
      {
        Warning("Coordinates variable %s has too many dimensions (%d), skipped!", ncvars[xvarid].name, ncvars[xvarid].ndims);
        // ncvar->xvarid = CDI_UNDEFID;
        xvarid = CDI_UNDEFID;
      }
    }

    if (yvarid != CDI_UNDEFID)
    {
      if (!ncvar->isCubeSphere && (ncvars[yvarid].ndims - hasTimeDim) > 2)
      {
        Warning("Coordinates variable %s has too many dimensions (%d), skipped!", ncvars[yvarid].name, ncvars[yvarid].ndims);
        // ncvar->yvarid = CDI_UNDEFID;
        yvarid = CDI_UNDEFID;
      }
    }

    bool isLon = false, isLat = false;

    if (xvarid != CDI_UNDEFID)
      if (cdf_read_xcoord(streamptr, lazyGrid, ncdims, ncvar, xvarid, &ncvars[xvarid], &xsize, ysize, hasTimeDim, readPart, start,
                          count, &isLon))
        return true;

    if (yvarid != CDI_UNDEFID)
      if (cdf_read_ycoord(streamptr, lazyGrid, ncdims, ncvar, yvarid, &ncvars[yvarid], xsize, &ysize, hasTimeDim, readPart, start,
                          count, &isLat))
        return true;

    // clang-format off
    if      (ncvar->gridtype == GRID_UNSTRUCTURED)     size = xsize;
    else if (ncvar->gridtype == GRID_GAUSSIAN_REDUCED) size = xsize;
    else if (ysize == 0)                               size = xsize;
    else if (xsize == 0)                               size = ysize;
    else                                               size = xsize * ysize;
    // clang-format on

    if (ncvar->gridtype == CDI_UNDEFID || ncvar->gridtype == GRID_GENERIC)
      cdf_check_gridtype(&ncvar->gridtype, isLon, isLat, xsize, ysize, grid);
  }

  int gridType = grid->type;
  if (gridType != GRID_PROJECTION)
  {
    gridType = ncvar->gridtype;
  }
  else if (gridType == GRID_PROJECTION && ncvar->gridtype == GRID_LONLAT && ncvar->isLonLatMapping)
  {
    gridType = ncvar->gridtype;
  }

  switch (gridType)
  {
  case GRID_GENERIC:
  case GRID_LONLAT:
  case GRID_GAUSSIAN:
  case GRID_UNSTRUCTURED:
  case GRID_CURVILINEAR:
  case GRID_PROJECTION:
  {
    grid->size = size;
    grid->x.size = xsize;
    grid->y.size = ysize;
    if (xvarid != CDI_UNDEFID && CDI_Read_Cell_Corners)
    {
      cdf_load_xbounds(lazyGrid, ncvar, ncvars, ncdims, timedimid, xvarid, vdimid, readPart, start, count);
    }
    if (yvarid != CDI_UNDEFID && CDI_Read_Cell_Corners)
    {
      cdf_load_ybounds(lazyGrid, ncvar, ncvars, ncdims, timedimid, yvarid, vdimid, readPart, start, count);
    }

    if (ncvar->cellarea != CDI_UNDEFID)
      cdf_load_cellarea(size, ncvar, &grid->area, &lazyGrid->cellAreaGet);

    if (gridType == GRID_GAUSSIAN && ncvar->numLPE > 0)
      grid->np = ncvar->numLPE;

    break;
  }
  case GRID_HEALPIX:
  {
    grid->size = size;
    if (ncvar->ivarid != CDI_UNDEFID)
      cdf_load_cellindices(size, ncvar, &grid->indices);
    break;
  }
  case GRID_GAUSSIAN_REDUCED:
  {
    if (ncvar->numLPE > 0 && ncvar->rpvarid != CDI_UNDEFID && ncvars[ncvar->rpvarid].ndims == 1)
    {
      grid->size = size;
      cdf_load_ybounds_reduced(lazyGrid, ncvar, ncvars, ncdims, yvarid, vdimid);
    }
    break;
  }
  case GRID_SPECTRAL:
  {
    grid->size = size;
    grid->lcomplex = 1;
    grid->trunc = ncvar->truncation;
    break;
  }
  case GRID_FOURIER:
  {
    grid->size = size;
    grid->trunc = ncvar->truncation;
    break;
  }
  case GRID_TRAJECTORY:
  {
    grid->size = 1;
    break;
  }
  case GRID_CHARXY:
  {
    grid->size = size;
    grid->x.size = xsize;
    grid->y.size = ysize;
    break;
  }
  }

  // if ( grid->type != GRID_PROJECTION && grid->type != ncvar->gridtype )
  if (grid->type != gridType)
  {
    // int gridtype = ncvar->gridtype;
    grid->type = gridType;
    cdiGridTypeInit(grid, gridType, grid->size);
  }

  if (grid->size == 0)
  {
    int ndims = ncvar->ndims;
    int *dimtypes = ncvar->dimtypes;
    if (ndims == 0 || (ndims == 1 && dimtypes[0] == T_AXIS) || (ndims == 1 && dimtypes[0] == Z_AXIS) || (ndims == 2 && dimtypes[0] == T_AXIS && dimtypes[1] == Z_AXIS))
    {
      grid->type = GRID_GENERIC;
      grid->size = 1;
      grid->x.size = 0;
      grid->y.size = 0;
    }
    else
    {
      Warning("Unsupported grid, skipped variable %s!", ncvar->name);
      ncvar->varStatus = UndefVar;
      return true;
    }
  }

  return false;
}

static bool
cdf_set_unstructured_par(ncvar_t *ncvar, grid_t *grid, int *xdimid, int *ydimid, GridInfo *gridInfo, bool readPart)
{
  int ndims = ncvar->ndims;
  int *dimtypes = ncvar->dimtypes;

  int zdimid = CDI_UNDEFID;
  int xdimidx = CDI_UNDEFID, ydimidx = CDI_UNDEFID;

  for (int i = 0; i < ndims; i++)
  {
    // clang-format off
    if      (dimtypes[i] == X_AXIS) xdimidx = i;
    else if (dimtypes[i] == Y_AXIS) ydimidx = i;
    else if (dimtypes[i] == Z_AXIS) zdimid = ncvar->dimids[i];
    // clang-format on
  }

  if (*xdimid != CDI_UNDEFID && *ydimid != CDI_UNDEFID && zdimid == CDI_UNDEFID)
  {
    if (grid->x.size > grid->y.size && grid->y.size < 1000)
    {
      dimtypes[ydimidx] = Z_AXIS;
      *ydimid = CDI_UNDEFID;
      grid->size = grid->x.size;
      grid->y.size = 0;
    }
    else if (grid->y.size > grid->x.size && grid->x.size < 1000)
    {
      dimtypes[xdimidx] = Z_AXIS;
      *xdimid = *ydimid;
      *ydimid = CDI_UNDEFID;
      grid->size = grid->y.size;
      grid->x.size = grid->y.size;
      grid->y.size = 0;
    }
  }

  if (grid->size != grid->x.size)
  {
    Warning("Unsupported array structure, skipped variable %s!", ncvar->name);
    ncvar->varStatus = UndefVar;
    return true;
  }

  if (!readPart)
  {
    if (gridInfo->number_of_grid_used != CDI_UNDEFID)
      cdiDefVarKeyInt(&grid->keys, CDI_KEY_NUMBEROFGRIDUSED, gridInfo->number_of_grid_used);
    if (ncvar->position > 0)
      cdiDefVarKeyInt(&grid->keys, CDI_KEY_NUMBEROFGRIDINREFERENCE, ncvar->position);
    if (!cdiUUIDIsNull(gridInfo->uuid))
      cdiDefVarKeyBytes(&grid->keys, CDI_KEY_UUID, gridInfo->uuid, CDI_UUID_SIZE);
  }

  return false;
}

static void
cdf_read_mapping_atts(int ncid, int gmapvarid, int nvatts, int projID)
{
  if (cdfCheckAttText(ncid, gmapvarid, "grid_mapping_name"))
  {
    char attstring[CDI_MAX_NAME];
    cdfGetAttText(ncid, gmapvarid, "grid_mapping_name", CDI_MAX_NAME, attstring);
    cdiDefKeyString(projID, CDI_GLOBAL, CDI_KEY_GRIDMAP_NAME, attstring);
  }

  bool removeFillValue = true;
  for (int i = 0; i < nvatts; ++i)
    cdf_set_cdi_attr(ncid, gmapvarid, i, projID, CDI_GLOBAL, removeFillValue);
}

static void
cdf_set_grid_to_similar_vars(ncvar_t *ncvar1, ncvar_t *ncvar2, int gridtype, int xdimid, int ydimid)
{
  if (ncvar2->varStatus == DataVar && ncvar2->gridID == CDI_UNDEFID)
  {
    int xdimid2 = CDI_UNDEFID, ydimid2 = CDI_UNDEFID, zdimid2 = CDI_UNDEFID;
    int xdimidx = CDI_UNDEFID, ydimidx = CDI_UNDEFID;

    const int *dimtypes2 = ncvar2->dimtypes;
    const int *dimids2 = ncvar2->dimids;
    int ndims2 = ncvar2->ndims;
    for (int i = 0; i < ndims2; i++)
    {
      if (dimtypes2[i] == X_AXIS)
      {
        xdimid2 = dimids2[i];
        xdimidx = i;
      }
      else if (dimtypes2[i] == Y_AXIS)
      {
        ydimid2 = dimids2[i];
        ydimidx = i;
      }
      else if (dimtypes2[i] == Z_AXIS)
      {
        zdimid2 = dimids2[i];
      }
    }

    if (!ncvar2->isCubeSphere)
    {
      if (ncvar2->gridtype == CDI_UNDEFID && gridtype == GRID_UNSTRUCTURED)
      {
        if (xdimid == xdimid2 && ydimid2 != CDI_UNDEFID && zdimid2 == CDI_UNDEFID)
        {
          ncvar2->dimtypes[ydimidx] = Z_AXIS;
          ydimid2 = CDI_UNDEFID;
        }

        if (xdimid == ydimid2 && xdimid2 != CDI_UNDEFID && zdimid2 == CDI_UNDEFID)
        {
          ncvar2->dimtypes[xdimidx] = Z_AXIS;
          xdimid2 = ydimid2;
          ydimid2 = CDI_UNDEFID;
        }
      }
      else if (ncvar2->gridtype == GRID_GAUSSIAN_REDUCED && gridtype == GRID_GAUSSIAN_REDUCED)
      {
        ydimid = CDI_UNDEFID;
      }
    }

    if (xdimid == xdimid2 && (ydimid == ydimid2 || (xdimid == ydimid && ydimid2 == CDI_UNDEFID)))
    {
      bool isSameGrid = (ncvar1->xvarid == ncvar2->xvarid && ncvar1->yvarid == ncvar2->yvarid && ncvar1->position == ncvar2->position);

      // if (xvarid != -1 && ncvar2->xvarid != CDI_UNDEFID && xvarid != ncvar2->xvarid) isSameGrid = false;
      // if (yvarid != -1 && ncvar2->yvarid != CDI_UNDEFID && yvarid != ncvar2->yvarid) isSameGrid = false;

      if (isSameGrid)
      {
        if (CDI_Debug)
          Message("Same gridID %d %s", ncvar1->gridID, ncvar2->name);
        ncvar2->gridID = ncvar1->gridID;
        ncvar2->chunkType = ncvar1->chunkType;
        ncvar2->chunkSize = ncvar1->chunkSize;
        ncvar2->gridSize = ncvar1->gridSize;
        ncvar2->xSize = ncvar1->xSize;
        ncvar2->ySize = ncvar1->ySize;
      }
    }
  }
}

static void
destroy_grid(struct cdfLazyGrid *lazyGrid, grid_t *grid)
{
  if (lazyGrid)
  {
    if (CDI_Netcdf_Lazy_Grid_Load)
      cdfLazyGridDestroy(lazyGrid);
    if (grid)
    {
      grid_free(grid);
      Free(grid);
    }
  }
}

static bool
is_healpix_grid(int ncid, int gmapvarid)
{
  if (gmapvarid == CDI_UNDEFID)
    return false;
  return cdfCheckAttInt(ncid, gmapvarid, "refinement_level");
}

static int
process_grid_query(CdiQuery *query, int xdimid, int ydimid, ncvar_t *ncvar, size_t *xsize, size_t *ysize, GridPart *gridPart)
{
  // process grid query information if available
  if (query)
  {
    int numCellidx = cdiQueryNumCellidx(query);
    if (numCellidx > 0)
    {
      // if (ncvar->gridtype != GRID_UNSTRUCTURED)
      if (xdimid != CDI_UNDEFID && ydimid != CDI_UNDEFID)
      {
        Warning("Query parameter cell is only available for 1D grids, skipped variable %s!", ncvar->name);
        ncvar->varStatus = UndefVar;
        // continue;
        return -1;
      }

      size_t start = cdiQueryGetCellidx(query, 0);
      size_t count = (numCellidx == 2) ? cdiQueryGetCellidx(query, 1) : 1;
      if ((start + count) <= *xsize)
      {
        cdiQueryCellidx(query, start);
        if (numCellidx == 2)
          cdiQueryCellidx(query, count);
        *xsize = count;
        *ysize = count;
        gridPart->start = (long)start - 1;
        gridPart->count = (long)count;
        gridPart->readPart = true;
      }
    }
  }
  return 0;
}

static int
cdf_define_all_grids(stream_t *streamptr, CdfGrid *ncgrid, int vlistID, ncdim_t *ncdims, int nvars, ncvar_t *ncvars,
                     GridInfo *gridInfo)
{
  for (int ncvarid = 0; ncvarid < nvars; ++ncvarid)
  {
    ncvar_t *ncvar = &ncvars[ncvarid];
    if (ncvar->varStatus == DataVar && ncvar->gridID == CDI_UNDEFID)
    {
      GridPart gridPart;
      gridpart_init(&gridPart);
      int ndims = ncvar->ndims;
      int *dimtypes = ncvar->dimtypes;
      int vdimid = CDI_UNDEFID;
      struct addIfNewRes projAdded = {.Id = CDI_UNDEFID, .isNew = 0}, gridAdded = {.Id = CDI_UNDEFID, .isNew = 0};
      int xdimid = CDI_UNDEFID, ydimid = CDI_UNDEFID;
      int nydims = cdf_get_xydimid(ndims, ncvar->dimids, dimtypes, &xdimid, &ydimid);

      int xaxisid = (xdimid != CDI_UNDEFID) ? ncdims[xdimid].ncvarid : CDI_UNDEFID;
      int yaxisid = (ydimid != CDI_UNDEFID) ? ncdims[ydimid].ncvarid : CDI_UNDEFID;
      int xvarid = (ncvar->xvarid != CDI_UNDEFID) ? ncvar->xvarid : xaxisid;
      int yvarid = (ncvar->yvarid != CDI_UNDEFID) ? ncvar->yvarid : yaxisid;

      size_t xsize = (xdimid != CDI_UNDEFID) ? ncdims[xdimid].len : 0;
      size_t ysize = (ydimid != CDI_UNDEFID) ? ncdims[ydimid].len : 0;

      if (ydimid == CDI_UNDEFID && yvarid != CDI_UNDEFID)
      {
        if (ncvars[yvarid].ndims == 1)
        {
          ydimid = ncvars[yvarid].dimids[0];
          ysize = ncdims[ydimid].len;
        }
      }

      int gmapvarid = ncvar->gmapid;
      bool lproj = (gmapvarid != CDI_UNDEFID);
      bool isHealpixGrid = (lproj && ncvar->isHealpixMapping) ? is_healpix_grid(ncvars[gmapvarid].ncid, gmapvarid) : false;
      if (isHealpixGrid)
      {
        ncvar->gridtype = GRID_HEALPIX;
      }

      if (!lproj && xaxisid != CDI_UNDEFID && xaxisid != xvarid && yaxisid != CDI_UNDEFID && yaxisid != yvarid)
        lproj = true;

      if (ncvar->isCubeSphere && lproj && xvarid != CDI_UNDEFID && yvarid != CDI_UNDEFID && ncvars[xvarid].ndims == 3 && ncvars[yvarid].ndims == 3)
      {
        lproj = false;
        ncvar->gridtype = GRID_UNSTRUCTURED;
        xsize = xsize * ysize * 6;
        ysize = xsize;
      }

      bool lgrid = !(lproj && ncvar->xvarid == CDI_UNDEFID);

      bool isUnstructured = (xdimid != CDI_UNDEFID && xdimid == ydimid && nydims == 0);
      if ((ncvar->gridtype == CDI_UNDEFID || ncvar->gridtype == GRID_GENERIC) && isUnstructured)
        ncvar->gridtype = GRID_UNSTRUCTURED;

      struct cdfLazyGrid *lazyGrid = NULL, *lazyProj = NULL;

      {
        int gridtype = (!lgrid && !isHealpixGrid) ? GRID_PROJECTION : ncvar->gridtype;
        if (CDI_Netcdf_Lazy_Grid_Load)
        {
          cdfLazyGridRenew(&lazyGrid, gridtype);
          if (lgrid && lproj)
            cdfLazyGridRenew(&lazyProj, GRID_PROJECTION);
        }
        else
        {
          cdfBaseGridRenew(&lazyGrid, gridtype);
          if (lgrid && lproj)
            cdfBaseGridRenew(&lazyProj, GRID_PROJECTION);
        }
      }
      grid_t *grid = &lazyGrid->base;
      grid_t *proj = (lgrid && lproj) ? &lazyProj->base : NULL;

      xaxisid = (xdimid != CDI_UNDEFID) ? ncdims[xdimid].ncvarid : CDI_UNDEFID;
      yaxisid = (ydimid != CDI_UNDEFID) ? ncdims[ydimid].ncvarid : CDI_UNDEFID;

      // process grid query information if available
      if (process_grid_query(streamptr->query, xdimid, ydimid, ncvar, &xsize, &ysize, &gridPart) < 0)
        continue;

      if (cdf_read_coordinates(streamptr, lazyGrid, ncvar, ncvars, ncdims, gridInfo->timedimid, xvarid, yvarid, xsize, ysize,
                               &vdimid, &gridPart))
        continue;

      if (gridInfo->number_of_grid_used != CDI_UNDEFID && (grid->type == CDI_UNDEFID || grid->type == GRID_GENERIC) && xdimid != CDI_UNDEFID && xsize > 999)
        grid->type = GRID_UNSTRUCTURED;

      if (!ncvar->isCubeSphere && grid->type == GRID_UNSTRUCTURED)
        if (cdf_set_unstructured_par(ncvar, grid, &xdimid, &ydimid, gridInfo, gridPart.readPart))
          continue;

      if (lgrid && lproj)
      {
        int dimid;
        cdf_read_coordinates(streamptr, lazyProj, ncvar, ncvars, ncdims, gridInfo->timedimid, xaxisid, yaxisid, xsize, ysize,
                             &dimid, NULL);
      }

      if (CDI_Debug)
      {
        Message("grid: type = %d, size = %zu, nx = %zu, ny = %zu", grid->type, grid->size, grid->x.size, grid->y.size);
        if (proj)
          Message("proj: type = %d, size = %zu, nx = %zu, ny = %zu", proj->type, proj->size, proj->x.size, proj->y.size);
      }

      if (lgrid && lproj)
      {
        projAdded = cdiVlistAddGridIfNew(vlistID, proj, 2);
        grid->proj = projAdded.Id;
      }

      gridAdded = cdiVlistAddGridIfNew(vlistID, grid, 1);
      ncvar->gridID = gridAdded.Id;
      ncvar->gridSize = grid->size;
      ncvar->xSize = grid->x.size;
      ncvar->ySize = grid->y.size;

      int gridID = ncvar->gridID;

      if (lproj && gmapvarid != CDI_UNDEFID)
      {
        bool gridIsNew = lgrid ? projAdded.isNew : gridAdded.isNew;
        if (gridIsNew)
        {
          int projID = lgrid ? grid->proj : gridID;
          int ncid = ncvars[gmapvarid].ncid;
          int gmapvartype = ncvars[gmapvarid].xtype;
          int nvatts = ncvars[gmapvarid].natts;
          cdiDefKeyInt(projID, CDI_GLOBAL, CDI_KEY_GRIDMAP_VARTYPE, gmapvartype);
          const char *gmapvarname = ncvars[gmapvarid].name;
          cdf_read_mapping_atts(ncid, gmapvarid, nvatts, projID);
          cdiDefKeyString(projID, CDI_GLOBAL, CDI_KEY_GRIDMAP_VARNAME, gmapvarname);
          gridVerifyProj(projID);
        }
      }

      if (grid->type == GRID_UNSTRUCTURED && gridInfo->gridfile[0] != 0 && !gridPart.readPart)
        gridDefReference(gridID, gridInfo->gridfile);

      if (ncvar->isChunked)
        grid_set_chunktype(grid, ncvar);

      int gridindex = vlistGridIndex(vlistID, gridID);
      if (gridPart.readPart)
      {
        ncgrid[gridindex].start = gridPart.start;
        ncgrid[gridindex].count = gridPart.count;
      }
      ncgrid[gridindex].gridID = gridID;
      if (grid->type == GRID_TRAJECTORY)
      {
        ncgrid[gridindex].ncIdVec[CDF_VARID_X] = xvarid;
        ncgrid[gridindex].ncIdVec[CDF_VARID_Y] = yvarid;
      }
      else
      {
        if (xdimid != CDI_UNDEFID)
          ncgrid[gridindex].ncIdVec[CDF_DIMID_X] = ncdims[xdimid].dimid;
        if (ydimid != CDI_UNDEFID)
          ncgrid[gridindex].ncIdVec[CDF_DIMID_Y] = ncdims[ydimid].dimid;
        if (ncvar->isCubeSphere)
          ncgrid[gridindex].ncIdVec[CDF_DIMID_E] = ncdims[ncvar->dimids[ndims - 3]].dimid;
      }

      if (xdimid == CDI_UNDEFID && ydimid == CDI_UNDEFID && grid->size == 1)
        gridDefHasDims(gridID, CoordVar);

      int xaxisVarID = (ncvar->gridtype == GRID_HEALPIX) ? CDI_XAXIS : CDI_XAXIS;
      if (xdimid != CDI_UNDEFID)
        cdiDefKeyString(gridID, xaxisVarID, CDI_KEY_DIMNAME, ncdims[xdimid].name);
      if (ydimid != CDI_UNDEFID)
        cdiDefKeyString(gridID, CDI_YAXIS, CDI_KEY_DIMNAME, ncdims[ydimid].name);
      if (vdimid != CDI_UNDEFID)
        cdiDefKeyString(gridID, CDI_GLOBAL, CDI_KEY_VDIMNAME, ncdims[vdimid].name);

      if (xvarid != CDI_UNDEFID && ncvars[xvarid].stdname[0])
        cdiDefKeyString(gridID, CDI_XAXIS, CDI_KEY_STDNAME, ncvars[xvarid].stdname);
      if (yvarid != CDI_UNDEFID && ncvars[yvarid].stdname[0])
        cdiDefKeyString(gridID, CDI_YAXIS, CDI_KEY_STDNAME, ncvars[yvarid].stdname);

      if (CDI_Debug)
        Message("gridID %d %d %s", gridID, ncvarid, ncvar->name);

      for (int ncvarid2 = ncvarid + 1; ncvarid2 < nvars; ncvarid2++)
        cdf_set_grid_to_similar_vars(ncvar, &ncvars[ncvarid2], grid->type, xdimid, ydimid);

      if (gridAdded.isNew)
        lazyGrid = NULL;
      if (projAdded.isNew)
        lazyProj = NULL;

      if (lazyGrid)
        destroy_grid(lazyGrid, grid);
      if (lazyProj)
        destroy_grid(lazyProj, proj);
    }
  }

  return 0;
}

// define all input zaxes
static int
cdf_define_all_zaxes(stream_t *streamptr, int vlistID, ncdim_t *ncdims, int nvars, ncvar_t *ncvars, size_t vctsize_echam,
                     double *vct_echam, unsigned char *uuidOfVGrid)
{
  size_t vctsize = vctsize_echam;
  double *vct = vct_echam;

  for (int ncvarid = 0; ncvarid < nvars; ncvarid++)
  {
    ncvar_t *ncvar = &ncvars[ncvarid];
    if (ncvar->varStatus == DataVar && ncvar->zaxisID == CDI_UNDEFID)
    {
      bool isScalar = false;
      int zdimid = CDI_UNDEFID;
      int zvarid = CDI_UNDEFID;
      size_t zsize = 1;
      int psvarid = -1;
      int p0varid = -1;

      int positive = 0;
      int ndims = ncvar->ndims;

      if (ncvar->zvarid != -1 && ncvars[ncvar->zvarid].ndims == 0)
      {
        zvarid = ncvar->zvarid;
        isScalar = true;
      }
      else
      {
        for (int i = 0; i < ndims; i++)
        {
          if (ncvar->dimtypes[i] == Z_AXIS)
            zdimid = ncvar->dimids[i];
        }

        if (zdimid != CDI_UNDEFID)
        {
          // zvarid = ncdims[zdimid].ncvarid;
          zvarid = (ncvar->zvarid != CDI_UNDEFID) ? ncvar->zvarid : ncdims[zdimid].ncvarid;
          zsize = ncdims[zdimid].len;
        }
      }

      if (CDI_Debug)
        Message("nlevs = %zu", zsize);

      double *zvar = NULL;
      char **zcvals = NULL;
      size_t zclength = 0;

      int zaxisType = CDI_UNDEFID;
      if (zvarid != CDI_UNDEFID)
        zaxisType = ncvars[zvarid].zaxistype;
      if (zaxisType == CDI_UNDEFID)
        zaxisType = ZAXIS_GENERIC;

      int zdatatype = CDI_DATATYPE_FLT64;
      double *lbounds = NULL;
      double *ubounds = NULL;

      char *pname = NULL, *plongname = NULL, *punits = NULL, *pstdname = NULL;
      if (zvarid != CDI_UNDEFID)
      {
        positive = ncvars[zvarid].positive;
        pname = ncvars[zvarid].name;
        plongname = ncvars[zvarid].longname;
        punits = ncvars[zvarid].units;
        pstdname = ncvars[zvarid].stdname;
        // clang-format off
	    if      (ncvars[zvarid].xtype == NC_FLOAT) zdatatype = CDI_DATATYPE_FLT32;
	    else if (ncvars[zvarid].xtype == NC_INT)   zdatatype = CDI_DATATYPE_INT32;
	    else if (ncvars[zvarid].xtype == NC_SHORT) zdatatype = CDI_DATATYPE_INT16;
        // clang-format on
#ifndef USE_MPI
        if (zaxisType == ZAXIS_CHAR && ncvars[zvarid].ndims == 2)
        {
          zdatatype = CDI_DATATYPE_UINT8;
          zclength = ncdims[ncvars[zvarid].dimids[1]].len;
          cdf_load_cvals(zsize * zclength, zvarid, ncvar, &zcvals, zsize);
        }
#endif

        if ((zaxisType == ZAXIS_HYBRID || zaxisType == ZAXIS_HYBRID_HALF) && ncvars[zvarid].vct)
        {
          vct = ncvars[zvarid].vct;
          vctsize = ncvars[zvarid].vctsize;

          if (ncvars[zvarid].psvarid != -1)
            psvarid = ncvars[zvarid].psvarid;
          if (ncvars[zvarid].p0varid != -1)
            p0varid = ncvars[zvarid].p0varid;
        }

        if (zaxisType != ZAXIS_CHAR)
        {
          zvar = (double *)Malloc(zsize * sizeof(double));
          cdf_get_var_double(ncvars[zvarid].ncid, zvarid, zvar);
        }

        int boundsId = ncvars[zvarid].bounds;
        if (boundsId != CDI_UNDEFID)
        {
          int nbdims = ncvars[boundsId].ndims;
          if (nbdims == 2 || isScalar)
          {
            size_t nlevel = isScalar ? 1 : ncdims[ncvars[boundsId].dimids[0]].len;
            int nvertex = (int)ncdims[ncvars[boundsId].dimids[1 - isScalar]].len;
            if (nlevel == zsize && nvertex == 2)
            {
              lbounds = (double *)Malloc(4 * nlevel * sizeof(double));
              ubounds = lbounds + nlevel;
              double *zbounds = lbounds + 2 * nlevel;
              cdf_get_var_double(ncvars[zvarid].ncid, boundsId, zbounds);
              for (size_t i = 0; i < nlevel; ++i)
              {
                lbounds[i] = zbounds[i * 2];
                ubounds[i] = zbounds[i * 2 + 1];
              }
            }
          }
        }
      }
      else
      {
        pname = (zdimid != CDI_UNDEFID) ? ncdims[zdimid].name : NULL;

        if (zsize == 1 && zdimid == CDI_UNDEFID)
        {
          zaxisType = (ncvar->zaxistype != CDI_UNDEFID) ? ncvar->zaxistype : ZAXIS_SURFACE;
          // if ( pname )
          {
            zvar = (double *)Malloc(sizeof(double));
            zvar[0] = 0;
          }
        }
      }

      if (zsize > INT_MAX)
      {
        Warning("Size limit exceeded for z-axis dimension (limit=%d)!", INT_MAX);
        return CDI_EDIMSIZE;
      }

      ncvar->zaxisID = varDefZaxis(vlistID, zaxisType, (int)zsize, zvar, (const char **)zcvals, zclength, lbounds, ubounds,
                                   (int)vctsize, vct, pname, plongname, punits, zdatatype, 1, 0, -1);
      ncvar->zSize = zsize;

      int zaxisID = ncvar->zaxisID;

      if (CDI_CMOR_Mode && zsize == 1 && zaxisType != ZAXIS_HYBRID)
        zaxisDefScalar(zaxisID);

      if (pstdname && *pstdname)
        cdiDefKeyBytes(zaxisID, CDI_GLOBAL, CDI_KEY_STDNAME, (const unsigned char *)pstdname, (int)strlen(pstdname) + 1);

      if (!cdiUUIDIsNull(uuidOfVGrid))
        cdiDefKeyBytes(zaxisID, CDI_GLOBAL, CDI_KEY_UUID, uuidOfVGrid, CDI_UUID_SIZE);

      if (zaxisType == ZAXIS_HYBRID)
      {
        if (psvarid != -1)
          cdiDefKeyString(zaxisID, CDI_GLOBAL, CDI_KEY_PSNAME, ncvars[psvarid].name);
        if (p0varid != -1)
        {
          double px = 1;
          cdf_get_var_double(ncvars[p0varid].ncid, p0varid, &px);
          cdiDefKeyFloat(zaxisID, CDI_GLOBAL, CDI_KEY_P0VALUE, px);
          cdiDefKeyString(zaxisID, CDI_GLOBAL, CDI_KEY_P0NAME, ncvars[p0varid].name);
        }
      }

      if (positive > 0)
        zaxisDefPositive(zaxisID, positive);
      if (isScalar)
        zaxisDefScalar(zaxisID);

      if (zdimid != CDI_UNDEFID)
        cdiDefKeyString(zaxisID, CDI_GLOBAL, CDI_KEY_DIMNAME, ncdims[zdimid].name);

      if (zvar)
        Free(zvar);
      if (zcvals)
      {
        for (size_t i = 0; i < zsize; i++)
          Free(zcvals[i]);
        Free(zcvals);
      }
      if (lbounds)
        Free(lbounds);

      if (zvarid != CDI_UNDEFID)
      {
        int ncid = ncvars[zvarid].ncid;
        int nvatts = ncvars[zvarid].natts;
        for (int iatt = 0; iatt < nvatts; ++iatt)
        {
          int attnum = ncvars[zvarid].atts[iatt];
          cdf_set_cdi_attr(ncid, zvarid, attnum, zaxisID, CDI_GLOBAL, false);
        }
      }

      int zaxisindex = vlistZaxisIndex(vlistID, zaxisID);
      streamptr->cdfInfo.zaxisIdVec[zaxisindex] = zdimid >= 0 ? ncdims[zdimid].dimid : zdimid;

      if (CDI_Debug)
        Message("zaxisID %d %d %s", zaxisID, ncvarid, ncvar->name);

      for (int ncvarid2 = ncvarid + 1; ncvarid2 < nvars; ncvarid2++)
        if (ncvars[ncvarid2].varStatus == DataVar && ncvars[ncvarid2].zaxisID == CDI_UNDEFID /*&& ncvars[ncvarid2].zaxistype == CDI_UNDEFID*/)
        {
          int zvarid2 = CDI_UNDEFID;
          if (ncvars[ncvarid2].zvarid != CDI_UNDEFID && ncvars[ncvars[ncvarid2].zvarid].ndims == 0)
            zvarid2 = ncvars[ncvarid2].zvarid;

          int zdimid2 = CDI_UNDEFID;
          ndims = ncvars[ncvarid2].ndims;
          for (int i = 0; i < ndims; i++)
          {
            if (ncvars[ncvarid2].dimtypes[i] == Z_AXIS)
              zdimid2 = ncvars[ncvarid2].dimids[i];
          }

          if (zdimid == zdimid2 /* && zvarid == zvarid2 */)
          {
            if ((zdimid != CDI_UNDEFID && ncvars[ncvarid2].zaxistype == CDI_UNDEFID) || (zdimid == CDI_UNDEFID && zvarid != CDI_UNDEFID && zvarid == zvarid2) || (zdimid == CDI_UNDEFID && zaxisType == ncvars[ncvarid2].zaxistype) || (zdimid == CDI_UNDEFID && zvarid2 == CDI_UNDEFID && ncvars[ncvarid2].zaxistype == CDI_UNDEFID))
            {
              if (CDI_Debug)
                Message("zaxisID %d %d %s", zaxisID, ncvarid2, ncvars[ncvarid2].name);
              ncvars[ncvarid2].zaxisID = zaxisID;
            }
          }
        }
    }
  }

  return 0;
}

struct cdf_varinfo
{
  int varid;
  const char *name;
};

static int
cdf_cmp_varname(const void *s1, const void *s2)
{
  const struct cdf_varinfo *x = (const struct cdf_varinfo *)s1, *y = (const struct cdf_varinfo *)s2;
  return strcmp(x->name, y->name);
}

static void
cdf_sort_varnames(int *varids, int nvars, const ncvar_t *ncvars)
{
  struct cdf_varinfo *varInfo = (struct cdf_varinfo *)Malloc((size_t)nvars * sizeof(struct cdf_varinfo));

  for (int varID = 0; varID < nvars; varID++)
  {
    int ncvarid = varids[varID];
    varInfo[varID].varid = ncvarid;
    varInfo[varID].name = ncvars[ncvarid].name;
  }
  qsort(varInfo, (size_t)nvars, sizeof(varInfo[0]), cdf_cmp_varname);
  for (int varID = 0; varID < nvars; varID++)
  {
    varids[varID] = varInfo[varID].varid;
  }
  Free(varInfo);
  if (CDI_Debug)
    for (int i = 0; i < nvars; i++)
      Message("sorted varids[%d] = %d", i, varids[i]);
}

static void
cdf_define_code_and_param(int vlistID, int varID)
{
  if (vlistInqVarCode(vlistID, varID) == -varID - 1)
  {
    char name[CDI_MAX_NAME];
    name[0] = 0;
    vlistInqVarName(vlistID, varID, name);
    size_t len = strlen(name);
    if (len > 3 && isdigit((int)name[3]))
    {
      if (strStartsWith(name, "var"))
        vlistDefVarCode(vlistID, varID, atoi(name + 3));
    }
    else if (len > 4 && isdigit((int)name[4]))
    {
      if (strStartsWith(name, "code"))
        vlistDefVarCode(vlistID, varID, atoi(name + 4));
    }
    else if (len > 5 && isdigit((int)name[5]))
    {
      if (strStartsWith(name, "param"))
      {
        int pnum = -1, pcat = 255, pdis = 255;
        sscanf(name + 5, "%d.%d.%d", &pnum, &pcat, &pdis);
        vlistDefVarParam(vlistID, varID, cdiEncodeParam(pnum, pcat, pdis));
      }
    }
  }
}

static void
cdf_define_institut_and_model_id(int vlistID, int varID)
{
  int varInstID = vlistInqVarInstitut(vlistID, varID);
  int varModelID = vlistInqVarModel(vlistID, varID);
  int varTableID = vlistInqVarTable(vlistID, varID);
  int code = vlistInqVarCode(vlistID, varID);
  if (CDI_Default_TableID != CDI_UNDEFID)
  {
    char name[CDI_MAX_NAME];
    name[0] = 0;
    char longname[CDI_MAX_NAME];
    longname[0] = 0;
    char units[CDI_MAX_NAME];
    units[0] = 0;
    tableInqEntry(CDI_Default_TableID, code, -1, name, longname, units);
    if (name[0])
    {
      cdiDeleteKey(vlistID, varID, CDI_KEY_NAME);
      cdiDeleteKey(vlistID, varID, CDI_KEY_LONGNAME);
      cdiDeleteKey(vlistID, varID, CDI_KEY_UNITS);

      if (varTableID != CDI_UNDEFID)
      {
        cdiDefKeyString(vlistID, varID, CDI_KEY_NAME, name);
        if (longname[0])
          cdiDefKeyString(vlistID, varID, CDI_KEY_LONGNAME, longname);
        if (units[0])
          cdiDefKeyString(vlistID, varID, CDI_KEY_UNITS, units);
      }
      else
      {
        varTableID = CDI_Default_TableID;
      }
    }

    if (CDI_Default_ModelID != CDI_UNDEFID)
      varModelID = CDI_Default_ModelID;
    if (CDI_Default_InstID != CDI_UNDEFID)
      varInstID = CDI_Default_InstID;
  }
  if (varInstID != CDI_UNDEFID)
    vlistDefVarInstitut(vlistID, varID, varInstID);
  if (varModelID != CDI_UNDEFID)
    vlistDefVarModel(vlistID, varID, varModelID);
  if (varTableID != CDI_UNDEFID)
    vlistDefVarTable(vlistID, varID, varTableID);
}

static inline size_t
size_of_dim_chunks(size_t n, size_t c)
{
  return (n / c + (n % c > 0)) * c;
}

static size_t
calc_chunk_cache_size(int timedimid, ncvar_t *ncvar)
{
  size_t nx = 0, ny = 0, nz = 0;
  size_t cx = 0, cy = 0, cz = 0;
  for (int i = 0; i < ncvar->ndims; i++)
  {
    int dimtype = ncvar->dimtypes[i];
    // clang-format off
    if      (dimtype == Z_AXIS) { cz = ncvar->chunks[i]; nz = ncvar->zSize; }
    else if (dimtype == Y_AXIS) { cy = ncvar->chunks[i]; ny = ncvar->ySize; }
    else if (dimtype == X_AXIS) { cx = ncvar->chunks[i]; nx = ncvar->xSize; }
    // clang-format on
  }

  size_t numSteps = (ncvar->dimids[0] == timedimid) ? ncvar->chunks[0] : 1;
  size_t chunkCacheSize = numSteps;
  if (nz > 0 && cz > 0)
    chunkCacheSize *= (numSteps == 1) ? cz : size_of_dim_chunks(nz, cz);

  if (chunkCacheSize == 1)
    return 0; // no chunk cache needed because the full field is read

  if (ny > 0 && cy > 0)
    chunkCacheSize *= size_of_dim_chunks(ny, cy);
  if (nx > 0 && cx > 0)
    chunkCacheSize *= size_of_dim_chunks(nx, cx);

  chunkCacheSize *= cdf_xtype_to_numbytes(ncvar->xtype);

  if (CDI_Chunk_Cache_Max > 0 && chunkCacheSize > CDI_Chunk_Cache_Max)
    chunkCacheSize = CDI_Chunk_Cache_Max;

  return chunkCacheSize;
}

static void
cdf_set_var_chunk_cache(ncvar_t *ncvar, int ncvarid, size_t chunkCacheSize)
{
  if (CDI_Debug || CDI_Chunk_Cache_Info)
    Message("%s: chunkCacheSize=%zu", ncvar->name, chunkCacheSize);
  nc_set_var_chunk_cache(ncvar->ncid, ncvarid, chunkCacheSize, ncvar->chunkCacheNelems, ncvar->chunkCachePreemption);
}

// define all input data variables
static void
cdf_define_all_vars(stream_t *streamptr, int vlistID, int instID, int modelID, int nvars, int num_ncvars, ncvar_t *ncvars,
                    ncdim_t *ncdims, int timedimid)
{
  int *varids = (int *)Malloc((size_t)nvars * sizeof(int));
  int n = 0;
  for (int ncvarid = 0; ncvarid < num_ncvars; ncvarid++)
    if (ncvars[ncvarid].varStatus == DataVar)
      varids[n++] = ncvarid;

  if (CDI_Debug)
    for (int i = 0; i < nvars; i++)
      Message("varids[%d] = %d", i, varids[i]);

  if (streamptr->sortname)
    cdf_sort_varnames(varids, nvars, ncvars);

  for (int varID1 = 0; varID1 < nvars; varID1++)
  {
    int ncvarid = varids[varID1];
    ncvar_t *ncvar = &ncvars[ncvarid];
    int gridID = ncvar->gridID;
    int zaxisID = ncvar->zaxisID;

    stream_new_var(streamptr, gridID, zaxisID, CDI_UNDEFID);
    int varID = vlistDefVar(vlistID, gridID, zaxisID, ncvar->timetype);
    ncvar->cdiVarID = varID;

    if (ncvar->hasFilter)
      cdiDefKeyString(vlistID, varID, CDI_KEY_FILTERSPEC_IN, ncvar->filterSpec);
    if (ncvar->hasFilter)
      vlistDefVarCompType(vlistID, varID, CDI_COMPRESS_FILTER);
    if (ncvar->hasDeflate)
      vlistDefVarCompType(vlistID, varID, CDI_COMPRESS_ZIP);
    if (ncvar->hasSzip)
      vlistDefVarCompType(vlistID, varID, CDI_COMPRESS_SZIP);
    if (ncvar->isChunked)
    {
      if (ncvar->chunkType != CDI_UNDEFID)
        cdiDefKeyInt(vlistID, varID, CDI_KEY_CHUNKTYPE, ncvar->chunkType);
      if (ncvar->chunkSize > 1)
        cdiDefKeyInt(vlistID, varID, CDI_KEY_CHUNKSIZE, ncvar->chunkSize);

      size_t cacheSize = calc_chunk_cache_size(timedimid, ncvar);
      if (CDI_Chunk_Cache_In >= 0)
      {
        cacheSize = (size_t)CDI_Chunk_Cache_In;
      }
      cdf_set_var_chunk_cache(ncvar, ncvarid, cacheSize);
    }

    streamptr->vars[varID1].defmiss = false;
    streamptr->vars[varID1].ncvarid = ncvarid;

    cdiDefKeyString(vlistID, varID, CDI_KEY_NAME, ncvar->name);
    if (ncvar->param != CDI_UNDEFID)
      vlistDefVarParam(vlistID, varID, ncvar->param);
    if (ncvar->code != CDI_UNDEFID)
      vlistDefVarCode(vlistID, varID, ncvar->code);
    if (ncvar->code != CDI_UNDEFID)
      vlistDefVarParam(vlistID, varID, cdiEncodeParam(ncvar->code, ncvar->tabnum, 255));
    if (ncvar->longname[0])
      cdiDefKeyString(vlistID, varID, CDI_KEY_LONGNAME, ncvar->longname);
    if (ncvar->stdname[0])
      cdiDefKeyString(vlistID, varID, CDI_KEY_STDNAME, ncvar->stdname);
    if (ncvar->unitsLen > 0)
      cdiDefKeyString(vlistID, varID, CDI_KEY_UNITS, ncvar->units);

    if (ncvar->validrangeDefined)
      vlistDefVarValidrange(vlistID, varID, ncvar->validrange);

    if (IS_NOT_EQUAL(ncvar->addoffset, 0.0))
      cdiDefKeyFloat(vlistID, varID, CDI_KEY_ADDOFFSET, ncvar->addoffset);
    if (IS_NOT_EQUAL(ncvar->scalefactor, 1.0))
      cdiDefKeyFloat(vlistID, varID, CDI_KEY_SCALEFACTOR, ncvar->scalefactor);

    vlistDefVarDatatype(vlistID, varID, cdfInqDatatype(streamptr, ncvar->xtype, ncvar->isUnsigned));

    vlistDefVarInstitut(vlistID, varID, instID);
    vlistDefVarModel(vlistID, varID, modelID);
    if (ncvar->tableID != CDI_UNDEFID)
      vlistDefVarTable(vlistID, varID, ncvar->tableID);

    if (ncvar->fillvalDefined == false && ncvar->missvalDefined)
    {
      ncvar->fillvalDefined = true;
      ncvar->fillval = ncvar->missval;
    }

    if (ncvar->fillvalDefined)
      vlistDefVarMissval(vlistID, varID, ncvar->fillval);

    if (CDI_Debug)
      Message("varID = %d  gridID = %d  zaxisID = %d", varID, vlistInqVarGrid(vlistID, varID), vlistInqVarZaxis(vlistID, varID));

    int gridindex = vlistGridIndex(vlistID, gridID);
    const CdfGrid *ncGrid = &(streamptr->cdfInfo.cdfGridVec[gridindex]);
    int xdimid = ncGrid->ncIdVec[CDF_DIMID_X];
    int ydimid = ncGrid->ncIdVec[CDF_DIMID_Y];

    int zaxisindex = vlistZaxisIndex(vlistID, zaxisID);
    int zdimid = streamptr->cdfInfo.zaxisIdVec[zaxisindex];

    int ndims = ncvar->ndims;
    static const int ipow10[4] = {1, 10, 100, 1000};

    int iodim = (ncvar->timetype != TIME_CONSTANT);

    const int *dimids = ncvar->dimids;

    int ixyz = 0;
    if ((ndims - iodim) <= 2 && (ydimid == xdimid || ydimid == CDI_UNDEFID))
    {
      ixyz = (xdimid == ncdims[dimids[ndims - 1]].dimid) ? 321 : 213;
    }
    else
    {
      for (int idim = iodim; idim < ndims; idim++)
      {
        int dimid = ncdims[dimids[idim]].dimid;
        // clang-format off
        if      (xdimid == dimid) ixyz += 1 * ipow10[ndims - idim - 1];
        else if (ydimid == dimid) ixyz += 2 * ipow10[ndims - idim - 1];
        else if (zdimid == dimid) ixyz += 3 * ipow10[ndims - idim - 1];
        // clang-format on
      }
    }

    if (ncvar->isCubeSphere)
      ixyz = 0;
    vlistDefVarXYZ(vlistID, varID, ixyz);
    /*
    printf("ixyz %d\n", ixyz);
    printf("ndims %d\n", ncvar->ndims);
    for (int i = 0; i < ncvar->ndims; ++i)
      printf("dimids: %d %d\n", i, dimids[i]);
    printf("xdimid, ydimid %d %d\n", xdimid, ydimid);
    */
    if (ncvar->numberOfForecastsInEnsemble != -1)
    {
      cdiDefKeyInt(vlistID, varID, CDI_KEY_NUMBEROFFORECASTSINENSEMBLE, ncvar->numberOfForecastsInEnsemble);
      cdiDefKeyInt(vlistID, varID, CDI_KEY_PERTURBATIONNUMBER, ncvar->perturbationNumber);
      if (ncvar->numberOfForecastsInEnsemble != -1)
        cdiDefKeyInt(vlistID, varID, CDI_KEY_TYPEOFENSEMBLEFORECAST, ncvar->typeOfEnsembleForecast);
    }
  }

  for (int varID = 0; varID < nvars; varID++)
  {
    int ncvarid = varids[varID];
    ncvar_t *ncvar = &ncvars[ncvarid];
    int ncid = ncvar->ncid;
    int nvatts = ncvar->natts;
    for (int iatt = 0; iatt < nvatts; ++iatt)
      cdf_set_cdi_attr(ncid, ncvarid, ncvar->atts[iatt], vlistID, varID, false);

    if (ncvar->atts)
      Free(ncvar->atts);
    if (ncvar->vct)
      Free(ncvar->vct);

    ncvar->atts = NULL;
    ncvar->vct = NULL;
  }

  // release mem of not freed attributes
  for (int ncvarid = 0; ncvarid < num_ncvars; ncvarid++)
  {
    ncvar_t *ncvar = &ncvars[ncvarid];
    if (ncvar->atts)
      Free(ncvar->atts);
    ncvar->atts = NULL;
  }

  if (varids)
    Free(varids);

  for (int varID = 0; varID < nvars; varID++)
    cdf_define_code_and_param(vlistID, varID);
  for (int varID = 0; varID < nvars; varID++)
    cdf_define_institut_and_model_id(vlistID, varID);
}

static void
cdf_copy_attint(int fileID, int vlistID, nc_type xtype, size_t attlen, char *attname)
{
  int attint[8];
  int *pattint = (attlen > 8) ? (int *)Malloc(attlen * sizeof(int)) : attint;
  cdfGetAttInt(fileID, NC_GLOBAL, attname, attlen, pattint);
  int datatype = (xtype == NC_SHORT) ? CDI_DATATYPE_INT16 : CDI_DATATYPE_INT32;
  cdiDefAttInt(vlistID, CDI_GLOBAL, attname, datatype, (int)attlen, pattint);
  if (attlen > 8)
    Free(pattint);
}

static void
cdf_copy_attflt(int fileID, int vlistID, nc_type xtype, size_t attlen, char *attname)
{
  double attflt[8];
  double *pattflt = (attlen > 8) ? (double *)Malloc(attlen * sizeof(double)) : attflt;
  cdfGetAttDouble(fileID, NC_GLOBAL, attname, attlen, pattflt);
  int datatype = (xtype == NC_FLOAT) ? CDI_DATATYPE_FLT32 : CDI_DATATYPE_FLT64;
  cdiDefAttFlt(vlistID, CDI_GLOBAL, attname, datatype, (int)attlen, pattflt);
  if (attlen > 8)
    Free(pattflt);
}

static void
check_cube_sphere(int vlistID, int nvars, ncvar_t *ncvars, ncdim_t *ncdims)
{
  bool isGeosData = false;
  int numNames = 4;
  const char *attnames[] = {"additional_vars", "file_format_version", "gridspec_file", "grid_mapping_name"};
  const char *grid_mapping = "gnomonic cubed-sphere";
  char attstring[256];
  int nf_dimid = -1, ncontact_dimid = -1;

  int numFound = 0;
  for (int i = 0; i < numNames; ++i)
    if (0 == cdiInqAttTxt(vlistID, CDI_GLOBAL, attnames[i], (int)sizeof(attstring), attstring))
      numFound++;

  if (numFound == numNames && strStartsWith(attstring, grid_mapping))
  {
    for (int i = 0; i < numNames; ++i)
      cdiDelAtt(vlistID, CDI_GLOBAL, attnames[i]);

    const char *nf_name = "nf";
    const char *ncontact_name = "ncontact";
    for (int varid = 0; varid < nvars; ++varid)
    {
      ncvar_t *ncvar = &ncvars[varid];
      if (ncvar->ndims == 1)
      {
        int dimid = ncvar->dimids[0];
        if (ncdims[dimid].len == 6 && str_is_equal(nf_name, ncvar->name))
        {
          isGeosData = true;
          nf_dimid = ncvar->dimids[0];
        }
        if (ncdims[dimid].len == 4 && str_is_equal(ncontact_name, ncvar->name))
          ncontact_dimid = ncvar->dimids[0];
      }
      if (isGeosData && ncontact_dimid != -1)
        break;
    }
  }

  if (isGeosData)
  {
    ncdims[nf_dimid].dimtype = E_AXIS;
    for (int varid = 0; varid < nvars; ++varid)
    {
      ncvar_t *ncvar = &ncvars[varid];
      if (str_is_equal("orientation", ncvar->name) || str_is_equal("anchor", ncvar->name) || str_is_equal("contacts", ncvar->name))
        cdf_set_var(ncvar, CoordVar);
    }

    for (int varid = 0; varid < nvars; ++varid)
    {
      ncvar_t *ncvar = &ncvars[varid];
      int ndims = ncvar->ndims;
      if (ndims >= 3 && ncvar->dimids[ndims - 3] == nf_dimid && ncvar->ncoordvars == 2 && ncvar->gmapid != -1)
        ncvar->isCubeSphere = true;
    }

    int xvarid = -1, yvarid = -1;
    int xboundsid = -1, yboundsid = -1;
    for (int varid = 0; varid < nvars; ++varid)
    {
      ncvar_t *ncvar = &ncvars[varid];
      int ndims = ncvar->ndims;
      if (ndims == 3)
      {
        // clang-format off
        if      (str_is_equal("lons", ncvar->name)) xvarid = varid;
        else if (str_is_equal("lats", ncvar->name)) yvarid = varid;
        else if (str_is_equal("corner_lons", ncvar->name)) xboundsid = varid;
        else if (str_is_equal("corner_lats", ncvar->name)) yboundsid = varid;
        // clang-format on
      }
      if (xvarid != -1 && xboundsid != -1 && yvarid != -1 && yboundsid != -1)
      {
        cdf_set_var(&ncvars[xboundsid], CoordVar);
        cdf_set_var(&ncvars[yboundsid], CoordVar);
        ncvars[xvarid].bounds = xboundsid;
        ncvars[yvarid].bounds = yboundsid;
        break;
      }
    }
  }

  if (CDI_Debug)
    Message("isGeosData %d", isGeosData);
}

static void
cdf_scan_global_attr(int fileID, int vlistID, int ngatts, int *instID, int *modelID, bool *ucla_les, unsigned char *uuidOfVGrid,
                     GridInfo *gridInfo)
{
  nc_type xtype = 0;
  size_t attlen = 0;
  char attname[CDI_MAX_NAME] = {0};

  for (int iatt = 0; iatt < ngatts; iatt++)
  {
    cdf_inq_attname(fileID, NC_GLOBAL, iatt, attname);
    cdf_inq_atttype(fileID, NC_GLOBAL, attname, &xtype);
    cdf_inq_attlen(fileID, NC_GLOBAL, attname, &attlen);

    if (xtypeIsText(xtype))
    {
      enum
      {
        attstringsize = 65636
      };
      char attstring[attstringsize] = {0};

      cdfGetAttText(fileID, NC_GLOBAL, attname, attstringsize, attstring);

      size_t attstrlen = strlen(attstring);

      if (attlen > 0 && attstring[0] != 0)
      {
        if (str_is_equal(attname, "institution"))
        {
          *instID = institutInq(0, 0, NULL, attstring);
          if (*instID == CDI_UNDEFID)
            *instID = institutDef(0, 0, NULL, attstring);

          cdiDefAttTxt(vlistID, CDI_GLOBAL, attname, (int)attstrlen, attstring);
        }
        else if (str_is_equal(attname, "source"))
        {
          *modelID = modelInq(-1, 0, attstring);
          if (*modelID == CDI_UNDEFID)
            *modelID = modelDef(-1, 0, attstring);

          cdiDefAttTxt(vlistID, CDI_GLOBAL, attname, (int)attstrlen, attstring);
        }
        else if (str_is_equal(attname, "Source") && strStartsWith(attstring, "UCLA-LES"))
        {
          *ucla_les = true;
          cdiDefAttTxt(vlistID, CDI_GLOBAL, attname, (int)attstrlen, attstring);
        }
        /*
        else if ( str_is_equal(attname, "Conventions") )
          {
          }
        */
        else if (str_is_equal(attname, "_NCProperties"))
        {
        }
        else if (str_is_equal(attname, "CDI"))
        {
        }
        else if (str_is_equal(attname, "CDO"))
        {
        }
        else if (str_is_equal(attname, "grid_file_uri"))
        {
          memcpy(gridInfo->gridfile, attstring, attstrlen + 1);
        }
        else if (attstrlen == 36 && str_is_equal(attname, "uuidOfHGrid"))
        {
          cdiStr2UUID(attstring, gridInfo->uuid);
        }
        else if (attstrlen == 36 && str_is_equal(attname, "uuidOfVGrid"))
        {
          cdiStr2UUID(attstring, uuidOfVGrid);
        }
        else
        {
          if (str_is_equal(attname, "ICON_grid_file_uri") && gridInfo->gridfile[0] == 0)
            memcpy(gridInfo->gridfile, attstring, attstrlen + 1);

          cdiDefAttTxt(vlistID, CDI_GLOBAL, attname, (int)attstrlen, attstring);
        }
      }
      else
      {
        cdiDefAttTxt(vlistID, CDI_GLOBAL, attname, (int)attstrlen, attstring);
      }
    }
    else if (xtype == NC_SHORT || xtype == NC_INT)
    {
      if (str_is_equal(attname, "number_of_grid_used"))
      {
        gridInfo->number_of_grid_used = CDI_UNDEFID;
        cdfGetAttInt(fileID, NC_GLOBAL, attname, 1, &gridInfo->number_of_grid_used);
      }
      else
      {
        cdf_copy_attint(fileID, vlistID, xtype, attlen, attname);
      }
    }
    else if (xtype == NC_FLOAT || xtype == NC_DOUBLE)
    {
      cdf_copy_attflt(fileID, vlistID, xtype, attlen, attname);
    }
  }
}

static int
find_leadtime(int nvars, ncvar_t *ncvars, int timedimid)
{
  int leadtime_id = CDI_UNDEFID;

  for (int ncvarid = 0; ncvarid < nvars; ncvarid++)
  {
    ncvar_t *ncvar = &ncvars[ncvarid];
    if (ncvar->ndims == 1 && timedimid == ncvar->dimids[0])
      if (ncvar->stdname[0] && str_is_equal(ncvar->stdname, "forecast_period"))
      {
        leadtime_id = ncvarid;
        break;
      }
  }

  return leadtime_id;
}

static void
find_time_vars(int nvars, ncvar_t *ncvars, ncdim_t *ncdims, int timedimid, stream_t *streamptr, bool *timeHasUnits,
               bool *timeHasBounds, bool *timeClimatology)
{
  int ncvarid;

  if (timedimid == CDI_UNDEFID)
  {
    char timeUnitsStr[CDI_MAX_NAME];

    for (ncvarid = 0; ncvarid < nvars; ncvarid++)
    {
      ncvar_t *ncvar = &ncvars[ncvarid];
      if (ncvar->ndims == 0 && ncvar->units[0] && str_is_equal(ncvar->name, "time"))
      {
        strcpy(timeUnitsStr, ncvar->units);
        str_to_lower(timeUnitsStr);

        if (is_time_units(timeUnitsStr))
        {
          streamptr->basetime.ncvarid = ncvarid;
          break;
        }
      }
    }
  }
  else
  {
    bool hasTimeVar = false;

    if (ncdims[timedimid].ncvarid != CDI_UNDEFID)
    {
      streamptr->basetime.ncvarid = ncdims[timedimid].ncvarid;
      hasTimeVar = true;
    }

    for (ncvarid = 0; ncvarid < nvars; ncvarid++)
    {
      ncvar_t *ncvar = &ncvars[ncvarid];
      if (ncvarid != streamptr->basetime.ncvarid && ncvar->ndims == 1 && timedimid == ncvar->dimids[0] && !xtypeIsText(ncvar->xtype) && is_timeaxis_units(ncvar->units))
      {
        ncvar->varStatus = CoordVar;

        if (!hasTimeVar)
        {
          hasTimeVar = true;
          streamptr->basetime.ncvarid = ncvarid;
        }
        else
        {
          Warning("Found more than one time variable, skipped variable %s!", ncvar->name);
        }
      }
    }

    if (hasTimeVar == false) // search for WRF time description
    {
      for (ncvarid = 0; ncvarid < nvars; ncvarid++)
      {
        ncvar_t *ncvar = &ncvars[ncvarid];
        if (ncvarid != streamptr->basetime.ncvarid && ncvar->ndims == 2 && timedimid == ncvar->dimids[0] && xtypeIsText(ncvar->xtype) && (ncdims[ncvar->dimids[1]].len == 19 || ncdims[ncvar->dimids[1]].len == 64))
        {
          ncvar->isTaxis = true;
          streamptr->basetime.ncvarid = ncvarid;
          streamptr->basetime.isWRF = true;
          break;
        }
      }
    }

    // time varID
    ncvarid = streamptr->basetime.ncvarid;

    if (ncvarid == CDI_UNDEFID && ncdims[timedimid].len > 0)
      Warning("Time variable >%s< not found!", ncdims[timedimid].name);
  }

  // time varID
  ncvarid = streamptr->basetime.ncvarid;

  if (ncvarid != CDI_UNDEFID && streamptr->basetime.isWRF == false)
  {
    ncvar_t *ncvar = &ncvars[ncvarid];
    if (ncvar->units[0] != 0)
      *timeHasUnits = true;

    if (ncvar->bounds != CDI_UNDEFID)
    {
      int nbdims = ncvars[ncvar->bounds].ndims;
      if (nbdims == 2)
      {
        int len = (int)ncdims[ncvars[ncvar->bounds].dimids[nbdims - 1]].len;
        if (len == 2 && timedimid == ncvars[ncvar->bounds].dimids[0])
        {
          *timeHasBounds = true;
          streamptr->basetime.ncvarboundsid = ncvar->bounds;
          if (ncvar->isClimatology)
            *timeClimatology = true;
        }
      }
    }
  }
}

static void
read_vct_echam(int fileID, int nvars, ncvar_t *ncvars, ncdim_t *ncdims, double **vct, size_t *pvctsize)
{
  // find ECHAM VCT
  int nvcth_id = CDI_UNDEFID, vcta_id = CDI_UNDEFID, vctb_id = CDI_UNDEFID;
  // int p0_id = CDI_UNDEFID;

  for (int ncvarid = 0; ncvarid < nvars; ncvarid++)
  {
    ncvar_t *ncvar = &ncvars[ncvarid];
    const char *name = ncvar->name;
    if (ncvar->ndims == 1)
    {
      size_t len = strlen(name);
      if (len == 4 && name[0] == 'h' && name[1] == 'y')
      {
        if (name[2] == 'a' && name[3] == 'i') // hyai
        {
          vcta_id = ncvarid;
          nvcth_id = ncvar->dimids[0];
          ncvar->varStatus = CoordVar;
        }
        else if (name[2] == 'b' && name[3] == 'i') // hybi
        {
          vctb_id = ncvarid;
          nvcth_id = ncvar->dimids[0];
          ncvar->varStatus = CoordVar;
        }
        else if ((name[2] == 'a' || name[2] == 'b') && name[3] == 'm')
        {
          ncvar->varStatus = CoordVar; // hyam or hybm
        }
      }
    }
    /*
    else if (ncvar->ndims == 0)
      {
        size_t len = strlen(name);
        if (len == 2 && name[0] == 'P' && name[1] == '0') p0_id = ncvarid;
      }
    */
  }

  // read VCT
  if (nvcth_id != CDI_UNDEFID && vcta_id != CDI_UNDEFID && vctb_id != CDI_UNDEFID)
  {
    size_t vctsize = 2 * ncdims[nvcth_id].len;
    *vct = (double *)Malloc(vctsize * sizeof(double));
    cdf_get_var_double(fileID, vcta_id, *vct);
    cdf_get_var_double(fileID, vctb_id, *vct + vctsize / 2);
    *pvctsize = vctsize;
    /*
    if (p0_id != CDI_UNDEFID)
      {
        double p0;
        cdf_get_var_double(fileID, p0_id, &p0);
      }
    */
  }
}

static void
cdf_set_ucla_dimtype(int ndims, ncdim_t *ncdims, ncvar_t *ncvars)
{
  for (int ncdimid = 0; ncdimid < ndims; ncdimid++)
  {
    ncdim_t *ncdim = &ncdims[ncdimid];
    int ncvarid = ncdim->ncvarid;
    if (ncvarid != -1)
    {
      ncvar_t *ncvar = &ncvars[ncvarid];
      if (ncdim->dimtype == CDI_UNDEFID && ncvar->units[0] == 'm')
      {
        // clang-format off
        if      (ncvar->name[0] == 'x') ncdim->dimtype = X_AXIS;
        else if (ncvar->name[0] == 'y') ncdim->dimtype = Y_AXIS;
        else if (ncvar->name[0] == 'z') ncdim->dimtype = Z_AXIS;
        // clang-format on
      }
    }
  }
}

static int
cdf_check_variables(stream_t *streamptr, int nvars, ncvar_t *ncvars, size_t ntsteps, int timedimid)
{
  for (int ncvarid = 0; ncvarid < nvars; ncvarid++)
  {
    ncvar_t *ncvar = &ncvars[ncvarid];
    if (ncvar->isTaxis && ncvar->ndims == 2)
    {
      ncvar->varStatus = CoordVar;
      continue;
    }

    if (ncvar->varStatus == UndefVar && ncvar->ndims > 1 && timedimid != CDI_UNDEFID && timedimid == ncvar->dimids[0])
      cdf_set_var(ncvar, DataVar);

    if (ncvar->varStatus == UndefVar)
    {
      if (ncvar->ndims == 0)
        cdf_set_var(ncvar, nvars == 1 ? DataVar : CoordVar);
      else if (ncvar->ndims > 0)
        cdf_set_var(ncvar, DataVar);
      else
      {
        ncvar->varStatus = CoordVar;
        Warning("Variable %s has an unknown type, skipped!", ncvar->name);
      }
    }

    if (ncvar->varStatus == CoordVar)
      continue;

    if ((ncvar->ndims > 4 && !ncvar->isCubeSphere) || ncvar->ndims > 5)
    {
      ncvar->varStatus = CoordVar;
      Warning("%d dimensional variables are not supported, skipped variable %s!", ncvar->ndims, ncvar->name);
      continue;
    }

    if (((ncvar->ndims == 4 && !ncvar->isCubeSphere) || ncvar->ndims == 5) && timedimid == CDI_UNDEFID)
    {
      ncvar->varStatus = CoordVar;
      Warning("%d dimensional variables without time dimension are not supported, skipped variable %s!", ncvar->ndims, ncvar->name);
      continue;
    }

    if (xtypeIsText(ncvar->xtype))
    {
      ncvar->varStatus = CoordVar;
      Warning("Unsupported data type (char/string), skipped variable %s!", ncvar->name);
      continue;
    }

    if (cdfInqDatatype(streamptr, ncvar->xtype, ncvar->isUnsigned) == -1)
    {
      ncvar->varStatus = CoordVar;
      Warning("Unsupported data type, skipped variable %s!", ncvar->name);
      continue;
    }

    if (timedimid != CDI_UNDEFID && ntsteps == 0 && ncvar->ndims > 0)
    {
      if (timedimid == ncvar->dimids[0])
      {
        ncvar->varStatus = CoordVar;
        Warning("Number of time steps undefined, skipped variable %s!", ncvar->name);
        continue;
      }
    }
  }

  return timedimid;
}

static void
cdfVerifyVars(int nvars, ncvar_t *ncvars
              //, ncdim_t *ncdims
)
{
  for (int ncvarid = 0; ncvarid < nvars; ncvarid++)
  {
    ncvar_t *ncvar = &ncvars[ncvarid];
    if (ncvar->varStatus == DataVar && ncvar->ndims > 0)
    {
      int ndims = 0;
      for (int i = 0; i < ncvar->ndims; ++i)
      {
        // clang-format off
        if      (ncvar->dimtypes[i] == T_AXIS) ndims++;
        else if (ncvar->dimtypes[i] == E_AXIS) ndims++;
        else if (ncvar->dimtypes[i] == Z_AXIS) ndims++;
        else if (ncvar->dimtypes[i] == Y_AXIS) ndims++;
        else if (ncvar->dimtypes[i] == X_AXIS) ndims++;
        // clang-format on
      }

      if (ncvar->ndims != ndims)
      {
        ncvar->varStatus = CoordVar;
        Warning("Inconsistent number of dimensions, skipped variable %s!", ncvar->name);
      }
      /* check failed with cdo option --cmor
      int zdimid = -1;
      for (int i = 0; i < ncvar->ndims; ++i)
        {
          if (ncvar->dimtypes[i] == Z_AXIS) zdimid = ncvar->dimids[i];
        }

      int zaxisID = ncvar->zaxisID;
      if (zaxisInqScalar(zaxisID) && zdimid != -1)
        {
          ncvar->varStatus = CoordVar;
          Warning("Unsupported dimension >%s<, skipped variable %s!", ncdims[zdimid].name, ncvar->name);
        }
      */
    }
  }
}

static CdiDateTime
wrf_read_timestep(int fileID, int nctimevarid, size_t tsID)
{
  enum
  {
    // position of terminator separating date and time from rest of
    dateTimeSepPos = 19,
    dateTimeStrSize = 128,
  };
  size_t start[2] = {tsID, 0}, count[2] = {1, dateTimeSepPos};
  char stvalue[dateTimeStrSize];
  stvalue[0] = 0;
  cdf_get_vara_text(fileID, nctimevarid, start, count, stvalue);
  stvalue[dateTimeSepPos] = 0;

  int year = 1, month = 1, day = 1, hour = 0, minute = 0, second = 0;
  if (strlen(stvalue) == dateTimeSepPos)
    sscanf(stvalue, "%d-%d-%d_%d:%d:%d", &year, &month, &day, &hour, &minute, &second);
  return cdiDateTime_set(cdiEncodeDate(year, month, day), cdiEncodeTime(hour, minute, second));
}

static double
get_timevalue(int fileID, int nctimevarid, size_t ncStepIndex, double *timevarBuffer)
{
  double timevalue = 0.0;

  if (timevarBuffer)
  {
    timevalue = timevarBuffer[ncStepIndex];
  }
  else
  {
    cdf_get_var1_double(fileID, nctimevarid, &ncStepIndex, &timevalue);
  }

  if (timevalue >= NC_FILL_DOUBLE || timevalue < -NC_FILL_DOUBLE)
    timevalue = 0.0;

  return timevalue;
}

static void
cdf_read_timesteps(size_t numTimesteps, stream_t *streamptr, taxis_t *taxis0)
{
  streamptr->curTsID = 0;
  streamptr->rtsteps = 1;

  if (numTimesteps == 0)
  {
    cdi_create_timesteps(numTimesteps, streamptr);
    cdf_create_records(streamptr, 0);
  }
  else
  {
    int fileID = streamptr->fileID;
    int nctimevarid = streamptr->basetime.ncvarid;
    int nctimeboundsid = streamptr->basetime.ncvarboundsid;
    bool hasTimesteps = (nctimevarid != CDI_UNDEFID /*&& streamptr->basetime.hasUnits*/);

    int *ncStepIndices = (int *)Malloc(numTimesteps * sizeof(int));
    for (size_t tsID = 0; tsID < numTimesteps; ++tsID)
      ncStepIndices[tsID] = (int)tsID;

    CdiDateTime *vDateTimeList = NULL;

    if (hasTimesteps)
    {
      vDateTimeList = (CdiDateTime *)Malloc(numTimesteps * sizeof(CdiDateTime));

      if (streamptr->basetime.isWRF)
      {
        for (size_t tsID = 0; tsID < numTimesteps; ++tsID)
          vDateTimeList[tsID] = wrf_read_timestep(fileID, nctimevarid, tsID);
      }
      else if (streamptr->basetime.hasUnits)
      {
        double *timevarBuffer = (double *)Malloc(numTimesteps * sizeof(double));
        cdf_get_var_double(fileID, nctimevarid, timevarBuffer);
        for (size_t tsID = 0; tsID < numTimesteps; ++tsID)
          vDateTimeList[tsID] = cdi_decode_timeval(get_timevalue(fileID, nctimevarid, tsID, timevarBuffer), taxis0);
        if (timevarBuffer)
          Free(timevarBuffer);
      }
      else
      {
        hasTimesteps = false;
      }
    }

    // process time query information if available
    CdiQuery *query = streamptr->query;
    if (query && cdiQueryNumStepidx(query) > 0)
    {
      // currently, the query interface does not support more than INT_MAX-1 steps!
      assert(numTimesteps < INT_MAX);
      for (size_t tsID = 0; tsID < numTimesteps; ++tsID)
        if (cdiQueryStepidx(query, (int)tsID + 1) < 0)
          ncStepIndices[tsID] = -1;
    }

    size_t numSteps = 0;
    for (size_t tsID = 0; tsID < numTimesteps; ++tsID)
      numSteps += (ncStepIndices[tsID] >= 0);

    cdi_create_timesteps(numSteps, streamptr);

    for (size_t tsID = 0, stepID = 0; tsID < numTimesteps; ++tsID)
    {
      if (ncStepIndices[tsID] >= 0)
      {
        streamptr->tsteps[stepID].ncStepIndex = ncStepIndices[tsID];
        cdf_create_records(streamptr, stepID);

        taxis_t *taxis = &streamptr->tsteps[stepID].taxis;
        ptaxisCopy(taxis, taxis0);

        if (hasTimesteps)
          taxis->vDateTime = vDateTimeList[tsID];

        stepID++;
      }
    }

    if (ncStepIndices)
      Free(ncStepIndices);
    if (vDateTimeList)
      Free(vDateTimeList);

    if (hasTimesteps)
    {
      if (nctimeboundsid != CDI_UNDEFID)
      {
        enum
        {
          numBnds = 2,
          tbNdims = 2
        };

        for (size_t tsID = 0; tsID < numSteps; ++tsID)
        {
          size_t ncStepIndex = (size_t)streamptr->tsteps[tsID].ncStepIndex;
          taxis_t *taxis = &streamptr->tsteps[tsID].taxis;
          size_t start[tbNdims] = {ncStepIndex, 0};
          size_t count[tbNdims] = {1, numBnds};
          double timeBnds[numBnds];
          cdf_get_vara_double(fileID, nctimeboundsid, start, count, timeBnds);
          for (size_t i = 0; i < numBnds; ++i)
            if (timeBnds[i] >= NC_FILL_DOUBLE || timeBnds[i] < -NC_FILL_DOUBLE)
              timeBnds[i] = 0.0;

          taxis->vDateTime_lb = cdi_decode_timeval(timeBnds[0], taxis);
          taxis->vDateTime_ub = cdi_decode_timeval(timeBnds[1], taxis);
        }
      }

      int leadtimeid = streamptr->basetime.leadtimeid;
      if (leadtimeid != CDI_UNDEFID)
      {
        for (size_t tsID = 0; tsID < numSteps; ++tsID)
        {
          size_t ncStepIndex = (size_t)streamptr->tsteps[tsID].ncStepIndex;
          taxis_t *taxis = &streamptr->tsteps[tsID].taxis;
          cdi_set_forecast_period(get_timevalue(fileID, leadtimeid, ncStepIndex, NULL), taxis);
        }
      }
    }
  }
}

static void
stream_set_ncdims(stream_t *streamptr, int ndims, ncdim_t *ncdims)
{
  int n = (ndims > MAX_DIMS_PS) ? MAX_DIMS_PS : ndims;
  CdfInfo *cdfInfo = &(streamptr->cdfInfo);
  cdfInfo->ncNumDims = n;
  for (int i = 0; i < n; i++)
    cdfInfo->ncDimIdVec[i] = ncdims[i].dimid;
  for (int i = 0; i < n; i++)
    cdfInfo->ncDimLenVec[i] = ncdims[i].len;
}

static void
set_ncdim_ids(int fileID, int ndims, ncdim_t *ncdims)
{
  if (ndims)
  {
    int gdimid = 0;
    for (int i = 0; i < NC_MAX_DIMS; ++i)
    {
      if (nc_inq_dimlen(fileID, i, NULL) == NC_NOERR)
      {
        ncdims[gdimid++].dimid = i;
        if (gdimid == ndims)
          break;
      }
    }
  }
}

static void
read_ncdims(int fileID, int ndims, ncdim_t *ncdims)
{
  for (int gdimid = 0; gdimid < ndims; gdimid++)
  {
    cdf_inq_dimlen(fileID, ncdims[gdimid].dimid, &ncdims[gdimid].len);
    cdf_inq_dimname(fileID, ncdims[gdimid].dimid, ncdims[gdimid].name);
  }
}

static void
check_ncgroups(int fileID)
{
  int numgrps = 0;
  int ncids[NC_MAX_VARS];
  char gname[CDI_MAX_NAME];
  nc_inq_grps(fileID, &numgrps, ncids);
  for (int i = 0; i < numgrps; ++i)
  {
    int ncid = ncids[i];
    nc_inq_grpname(ncid, gname);
    int gndims, gnvars, gngatts, gunlimdimid;
    cdf_inq(ncid, &gndims, &gnvars, &gngatts, &gunlimdimid);

    if (CDI_Debug)
      Message("%s: ndims %d, nvars %d, ngatts %d", gname, gndims, gnvars, gngatts);
  }
  if (numgrps)
    Warning("NetCDF4 groups not supported! Found %d root group%s.", numgrps, (numgrps > 1) ? "s" : "");
}

static void
find_coordinates_vars(int ndims, ncdim_t *ncdims, int nvars, ncvar_t *ncvars)
{
  for (int gdimid = 0; gdimid < ndims; gdimid++)
  {
    for (int varid = 0; varid < nvars; varid++)
    {
      ncvar_t *ncvar = &ncvars[varid];
      if (ncvar->ndims == 1 && gdimid == ncvar->dimids[0] && ncdims[gdimid].ncvarid == CDI_UNDEFID)
      {
        if (str_is_equal(ncvar->name, ncdims[gdimid].name))
        {
          ncdims[gdimid].ncvarid = varid;
          ncvar->varStatus = CoordVar;
        }
      }
    }
  }
}

// set time dependent data vars
static void
find_varying_data_vars1d(int timedimid, int nvars, ncvar_t *ncvars)
{
  for (int ncvarid = 0; ncvarid < nvars; ncvarid++)
  {
    ncvar_t *ncvar = &ncvars[ncvarid];
    if (ncvar->ndims == 1)
    {
      if (timedimid != CDI_UNDEFID && timedimid == ncvar->dimids[0])
      {
        if (ncvar->varStatus != CoordVar)
          cdf_set_var(ncvar, DataVar);
      }
      else
      {
        //  if ( ncvar->varStatus != DataVar ) cdf_set_var(ncvar, CoordVar);
      }
      // if ( ncvar->varStatus != DataVar ) cdf_set_var(ncvar, CoordVar);
    }
  }
}

static void
set_coordinates_varids(int numVars, ncvar_t *ncvars)
{
  for (int varId = 0; varId < numVars; varId++)
  {
    ncvar_t *ncvar = &ncvars[varId];
    if (ncvar->varStatus == DataVar && ncvar->ncoordvars)
    {
      for (int i = 0; i < ncvar->ncoordvars; i++)
      {
        int coordVarId = ncvar->coordvarids[i];
        if (coordVarId != CDI_UNDEFID)
        {
          ncvar_t *coordVar = &ncvars[coordVarId];
          // clang-format off
          if      (coordVar->isLon || coordVar->isXaxis) ncvar->xvarid = coordVarId;
          else if (coordVar->isLat || coordVar->isYaxis) ncvar->yvarid = coordVarId;
          else if (coordVar->isZaxis)                    ncvar->zvarid = coordVarId;
          else if (coordVar->isTaxis)                    ncvar->tvarid = coordVarId;
          else if (coordVar->isCharAxis)                 ncvar->cvarids[i] = coordVarId;
          else if (coordVar->isIndexAxis)                ncvar->ivarid = coordVarId;
          else if (coordVar->printWarning)
          {
            Warning("Coordinates variable %s can't be assigned!", coordVar->name);
            coordVar->printWarning = false;
          }
          // clang-format on
        }
      }
    }
  }
}

static void
process_var_query(CdiQuery *query, int nvars, ncvar_t *ncvars)
{
  // process var query information if available
  if (query && cdiQueryNumNames(query) > 0)
  {
    for (int ncvarid = 0; ncvarid < nvars; ++ncvarid)
    {
      ncvar_t *ncvar = &ncvars[ncvarid];
      if (ncvar->varStatus == DataVar && cdiQueryName(query, ncvar->name) < 0)
        ncvar->varStatus = CoordVar;
    }
  }
}

int cdfInqContents(stream_t *streamptr)
{
  GridInfo gridInfo;
  gridInfo.gridfile[0] = 0;
  memset(gridInfo.uuid, 0, CDI_UUID_SIZE);
  gridInfo.number_of_grid_used = CDI_UNDEFID;
  gridInfo.timedimid = CDI_UNDEFID;

  int vlistID = streamptr->vlistID;
  int fileID = streamptr->fileID;

  if (CDI_Debug)
    Message("streamID = %d, fileID = %d", streamptr->self, fileID);

  int ndims = 0, nvars = 0, ngatts = 0, unlimdimid = 0;
  cdf_inq(fileID, &ndims, &nvars, &ngatts, &unlimdimid);

  if (CDI_Debug)
    Message("root: ndims %d, nvars %d, ngatts %d", ndims, nvars, ngatts);

  // alloc ncdims
  ncdim_t *ncdims = ndims ? (ncdim_t *)Malloc((size_t)ndims * sizeof(ncdim_t)) : NULL;
  init_ncdims(ndims, ncdims);
  set_ncdim_ids(fileID, ndims, ncdims);
  read_ncdims(fileID, ndims, ncdims);

  int format = 0;
  nc_inq_format(fileID, &format);
  if (format == NC_FORMAT_NETCDF4)
    check_ncgroups(fileID);

  if (nvars == 0)
  {
    Warning("No arrays found!");
    return CDI_EUFSTRUCT;
  }

  // alloc ncvars
  ncvar_t *ncvars = (ncvar_t *)Malloc((size_t)nvars * sizeof(ncvar_t));
  init_ncvars(nvars, ncvars, fileID);

  read_vars_info(nvars, ncvars, ndims, ncdims, format);
  find_coordinates_vars(ndims, ncdims, nvars, ncvars);

  // scan global attributes
  int instID = CDI_UNDEFID;
  int modelID = CDI_UNDEFID;
  bool ucla_les = false;
  unsigned char uuidOfVGrid[CDI_UUID_SIZE] = {0};
  cdf_scan_global_attr(fileID, vlistID, ngatts, &instID, &modelID, &ucla_les, uuidOfVGrid, &gridInfo);

  // find time dim
  int timedimid = (unlimdimid >= 0) ? unlimdimid : cdf_time_dimid(fileID, ndims, ncdims, nvars, ncvars);
  streamptr->basetime.ncdimid = timedimid;

  size_t ntsteps = (timedimid == CDI_UNDEFID) ? 0 : ncdims[timedimid].len;
  if (ntsteps > INT_MAX)
  {
    Warning("Size limit exceeded for time dimension (limit=%d)!", INT_MAX);
    return CDI_EDIMSIZE;
  }

  if (CDI_Debug)
    Message("Number of timesteps = %zu", ntsteps);
  if (CDI_Debug)
    Message("Time dimid = %d", streamptr->basetime.ncdimid);

  // set T_AXIS dimtype
  for (int gdimid = 0; gdimid < ndims; gdimid++)
  {
    if (timedimid == gdimid)
      ncdims[gdimid].dimtype = T_AXIS;
  }

  stream_set_ncdims(streamptr, ndims, ncdims);

  if (CDI_Debug)
    cdf_print_vars(ncvars, nvars, "scan_vars_attr");

  // scan attributes of all variables
  set_vars_timetype(nvars, ncvars, timedimid);
  scan_vars_attr(nvars, ncvars, ndims, ncdims, modelID);
  verify_vars_attr(nvars, ncvars, ncdims);

  if (CDI_Convert_Cubesphere)
    check_cube_sphere(vlistID, nvars, ncvars, ncdims);

  if (CDI_Debug)
    cdf_print_vars(ncvars, nvars, "find_varying_data_vars1d");

  find_varying_data_vars1d(timedimid, nvars, ncvars);

  // find time vars
  bool timeHasUnits = false;
  bool timeHasBounds = false;
  bool timeClimatology = false;
  find_time_vars(nvars, ncvars, ncdims, timedimid, streamptr, &timeHasUnits, &timeHasBounds, &timeClimatology);

  int leadtime_id = find_leadtime(nvars, ncvars, timedimid);
  if (leadtime_id != CDI_UNDEFID)
    ncvars[leadtime_id].varStatus = CoordVar;

  // check ncvars
  timedimid = cdf_check_variables(streamptr, nvars, ncvars, ntsteps, timedimid);

  // verify coordinates vars - first scan (dimname == varname)
  bool isHybridCF = false;
  verify_coordinates_vars_1(fileID, ndims, ncdims, ncvars, timedimid, &isHybridCF);

  // verify coordinates vars - second scan (all other variables)
  verify_coordinates_vars_2(streamptr, nvars, ncvars);

  if (CDI_Debug)
    cdf_print_vars(ncvars, nvars, "verify_coordinate_vars");

  if (ucla_les)
    cdf_set_ucla_dimtype(ndims, ncdims, ncvars);

  /*
  for (int ncdimid = 0; ncdimid < ndims; ncdimid++)
    {
      int ncvarid = ncdims[ncdimid].ncvarid;
      if (ncvarid != -1)
        {
          printf("coord var %d %s %s\n", ncvarid, ncvar->name, ncvar->units);
          if (ncdims[ncdimid].dimtype == X_AXIS) printf("coord var %d %s is x dim\n", ncvarid, ncvar->name);
          if (ncdims[ncdimid].dimtype == Y_AXIS) printf("coord var %d %s is y dim\n", ncvarid, ncvar->name);
          if (ncdims[ncdimid].dimtype == Z_AXIS) printf("coord var %d %s is z dim\n", ncvarid, ncvar->name);
          if (ncdims[ncdimid].dimtype == T_AXIS) printf("coord var %d %s is t dim\n", ncvarid, ncvar->name);

          if (ncvar->isLon) printf("coord var %d %s is lon\n", ncvarid, ncvar->name);
          if (ncvar->isLat) printf("coord var %d %s is lat\n", ncvarid, ncvar->name);
          if (ncvar->isZaxis) printf("coord var %d %s is lev\n", ncvarid, ncvar->name);
        }
    }
  */

  set_coordinates_varids(nvars, ncvars);

  cdf_set_dimtype(nvars, ncvars, ncdims);

  // read ECHAM VCT if present
  size_t vctsize = 0;
  double *vct = NULL;
  if (!isHybridCF)
    read_vct_echam(fileID, nvars, ncvars, ncdims, &vct, &vctsize);

  // process var query information if available
  process_var_query(streamptr->query, nvars, ncvars);

  if (CDI_Debug)
    cdf_print_vars(ncvars, nvars, "cdf_define_all_grids");

  // define all grids
  gridInfo.timedimid = timedimid;
  int status = cdf_define_all_grids(streamptr, streamptr->cdfInfo.cdfGridVec, vlistID, ncdims, nvars, ncvars, &gridInfo);
  if (status < 0)
    return status;

  // define all zaxes
  status = cdf_define_all_zaxes(streamptr, vlistID, ncdims, nvars, ncvars, vctsize, vct, uuidOfVGrid);
  if (vct)
    Free(vct);
  if (status < 0)
    return status;

  // verify vars
  cdfVerifyVars(nvars, ncvars);

  // select vars
  int nvarsData = 0;
  for (int ncvarid = 0; ncvarid < nvars; ncvarid++)
    if (ncvars[ncvarid].varStatus == DataVar)
      nvarsData++;

  if (CDI_Debug)
    Message("time varid = %d", streamptr->basetime.ncvarid);
  if (CDI_Debug)
    Message("ntsteps = %zu", ntsteps);
  if (CDI_Debug)
    Message("nvarsData = %d", nvarsData);

  if (nvarsData == 0)
  {
    streamptr->ntsteps = 0;
    Warning("No data arrays found!");
    return CDI_EUFSTRUCT;
  }

  if (ntsteps == 0 && streamptr->basetime.ncdimid == CDI_UNDEFID && streamptr->basetime.ncvarid != CDI_UNDEFID)
    ntsteps = 1;

  // define all data variables
  cdf_define_all_vars(streamptr, vlistID, instID, modelID, nvarsData, nvars, ncvars, ncdims, timedimid);

  cdf_set_chunk_info(streamptr, nvars, ncvars);

  // time varID
  int nctimevarid = streamptr->basetime.ncvarid;

  if (nctimevarid != CDI_UNDEFID && (!timeHasUnits || streamptr->basetime.isWRF))
    ncvars[nctimevarid].units[0] = 0;
  if (nctimevarid != CDI_UNDEFID && timeHasUnits)
    streamptr->basetime.hasUnits = true;

  taxis_t taxis0;
  ptaxisInit(&taxis0);

  if (timeHasUnits)
  {
    if (set_base_time(ncvars[nctimevarid].units, &taxis0) == 1)
    {
      nctimevarid = CDI_UNDEFID;
      streamptr->basetime.ncvarid = CDI_UNDEFID;
      streamptr->basetime.hasUnits = false;
    }

    if (leadtime_id != CDI_UNDEFID && taxis0.type == TAXIS_RELATIVE)
    {
      streamptr->basetime.leadtimeid = leadtime_id;
      taxis0.type = TAXIS_FORECAST;

      int timeunit = (ncvars[leadtime_id].units[0] != 0) ? scan_time_units(ncvars[leadtime_id].units) : -1;
      if (timeunit == -1)
        timeunit = taxis0.unit;
      taxis0.fc_unit = timeunit;
    }
  }

  if (timeHasBounds)
  {
    taxis0.hasBounds = true;
    if (timeClimatology)
      taxis0.climatology = true;
  }

  if (nctimevarid != CDI_UNDEFID)
  {
    ptaxisDefName(&taxis0, ncvars[nctimevarid].name);
    if (ncvars[nctimevarid].longname[0])
      ptaxisDefLongname(&taxis0, ncvars[nctimevarid].longname);
    if (ncvars[nctimevarid].units[0])
      ptaxisDefUnits(&taxis0, ncvars[nctimevarid].units);

    int xtype = ncvars[nctimevarid].xtype;
    int datatype = (xtype == NC_INT) ? CDI_DATATYPE_INT32 : ((xtype == NC_FLOAT) ? CDI_DATATYPE_FLT32 : CDI_DATATYPE_FLT64);
    ptaxisDefDatatype(&taxis0, datatype);
  }

  int calendar = CDI_UNDEFID;
  if (nctimevarid != CDI_UNDEFID && ncvars[nctimevarid].hasCalendar)
  {
    char attstring[1024];
    cdfGetAttText(fileID, nctimevarid, "calendar", sizeof(attstring), attstring);
    str_to_lower(attstring);
    calendar = attribute_to_calendar(attstring);
  }

  if (streamptr->basetime.isWRF)
    taxis0.type = TAXIS_ABSOLUTE;

  int taxisID;
  if (taxis0.type == TAXIS_FORECAST)
  {
    taxisID = taxisCreate(TAXIS_FORECAST);
  }
  else if (taxis0.type == TAXIS_RELATIVE)
  {
    taxisID = taxisCreate(TAXIS_RELATIVE);
  }
  else
  {
    taxisID = taxisCreate(TAXIS_ABSOLUTE);
    if (!timeHasUnits)
    {
      taxisDefTunit(taxisID, TUNIT_DAY);
      taxis0.unit = TUNIT_DAY;
    }
  }

  if (calendar == CDI_UNDEFID && taxis0.type != TAXIS_ABSOLUTE)
    calendar = CALENDAR_STANDARD;

#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ > 5)
#pragma GCC diagnostic push
#pragma GCC diagnostic warning "-Wstrict-overflow"
#endif
  if (calendar != CDI_UNDEFID)
  {
    taxis0.calendar = calendar;
    taxisDefCalendar(taxisID, calendar);
  }
#if __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ > 5)
#pragma GCC diagnostic pop
#endif

  vlistDefTaxis(vlistID, taxisID);

  cdf_read_timesteps(ntsteps, streamptr, &taxis0);
  taxisDestroyKernel(&taxis0);

  // free ncdims
  if (ncdims)
    Free(ncdims);

  // free ncvars
  if (ncvars)
  {
    for (int ncvarid = 0; ncvarid < nvars; ncvarid++)
    {
      ncvar_t *ncvar = &ncvars[ncvarid];
      if (ncvar->atts)
        Free(ncvar->atts);
      if (ncvar->vct)
        Free(ncvar->vct);
    }
    Free(ncvars);
  }

  return 0;
}

int cdfInqTimestep(stream_t *streamptr, int tsID)
{
  if (tsID < 0 || tsID >= streamptr->ntsteps)
    Error("tsID=%d out of range (0-%d)!", tsID, streamptr->ntsteps - 1);

  streamptr->curTsID = tsID;
  int numRecs = streamptr->tsteps[tsID].nrecs;

  return numRecs;
}

#endif
/*
 * Local Variables:
 * c-file-style: "Java"
 * c-basic-offset: 2
 * indent-tabs-mode: nil
 * show-trailing-whitespace: t
 * require-trailing-newline: t
 * End:
 */
