/*
* Copyright (c) 2002-2012, California Institute of Technology.
* All rights reserved.  Based on Government Sponsored Research under contracts NAS7-1407 and/or NAS7-03001.

* Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:
*   1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
*   2. Redistributions in binary form must reproduce the above copyright notice,
*      this list of conditions and the following disclaimer in the documentation and/or other materials provided with the distribution.
*   3. Neither the name of the California Institute of Technology (Caltech), its operating division the Jet Propulsion Laboratory (JPL),
*      the National Aeronautics and Space Administration (NASA), nor the names of its contributors may be used to
*      endorse or promote products derived from this software without specific prior written permission.

* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
* INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
* IN NO EVENT SHALL THE CALIFORNIA INSTITUTE OF TECHNOLOGY BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
* EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
* STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
* EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

/******************************************************************************
* $Id$
*
* Project:  Meta Raster File Format Driver Implementation, Dataset
* Purpose:  Implementation of GDAL dataset
*
* Author:   Lucian Plesea, Lucian.Plesea@jpl.nasa.gov, lplesea@esri.com
*
******************************************************************************
*
*
*
*
*
****************************************************************************/

#include "marfa.h"
#include <gdal_priv.h>
#include <assert.h>

#include <vector>

// Sleep is not portable and not covered in GDAL as far as I can tell
// So we define MRF_sleep_ms, in milliseconds, not very accurate unfortunately
#if defined(WIN32)
// Unfortunately this defines all sorts of garbage
#include <windows.h>
#define MRF_sleep_ms(t) Sleep(t)
#else // Assume linux
// Unfortunately this defines all sorts of garbage
#include <unistd.h>
// Usleep is in usec
#define MRF_sleep_ms(t) usleep(t*1000)
#endif

using std::vector;
using std::string;

// Initialize as invalid
GDALMRFDataset::GDALMRFDataset()
{
    //		     X0   Xx   Xy  Y0    Yx   Yy   
    double gt[6] = { 0.0, 1.0, 0.0, 0.0, 0.0, 1.0 };

    ILImage img;

    memcpy(GeoTransform, gt, sizeof(gt));
    bGeoTransformValid = FALSE;
    ifp.FP = dfp.FP = 0;
    pbuffer = 0;
    pbsize = 0;
    bdirty = 0;
    scale = 0; // Unset
    hasVersions = false;
    level = -1;
    tile = ILSize();
    cds = NULL;
    poSrcDS = NULL;
    poColorTable = NULL;
    bCrystalized = TRUE; // Assume not in create mode
}

void GDALMRFDataset::SetPBuffer(unsigned int sz)
{
    pbuffer = CPLRealloc(pbuffer, sz);
    pbsize = (pbuffer == 0) ? 0 : sz;
}

GDALMRFDataset::~GDALMRFDataset()

{   // Make sure everything gets written
    FlushCache();
    if (ifp.FP)
	VSIFCloseL(ifp.FP);
    if (dfp.FP)
	VSIFCloseL(dfp.FP);
    delete cds;
    delete poSrcDS;
    delete poColorTable;

    // CPLFree ignores being called with NULL
    CPLFree(pbuffer);
    pbsize = 0;
}

/*
 *\brief Called before the IRaster IO gets called
 *
 *
 *
 */
CPLErr GDALMRFDataset::AdviseRead(int nXOff, int nYOff, int nXSize, int nYSize,
    int nBufXSize, int nBufYSize,
    GDALDataType eDT,
    int nBandCount, int *panBandList,
    char **papszOptions)
{
    CPLDebug("MRF_IO", "AdviseRead %d, %d, %d, %d, bufsz %d,%d,%d\n",
	nXOff, nYOff, nXSize, nYSize, nBufXSize, nBufYSize, nBandCount);
    return CE_None;
}

/*
 *\brief Format specifc RasterIO, may be bypassed by BlockBasedRasterIO by setting
 * GDAL_FORCE_CACHING to Yes, in which case the band ReadBlock and WriteBLock are called
 * directly
 *
 *
 */
CPLErr GDALMRFDataset::IRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff, int nXSize, int nYSize,
    void *pData, int nBufXSize, int nBufYSize, GDALDataType eBufType,
    int nBandCount, int *panBandMap,
    int nPixelSpace, int nLineSpace, int nBandSpace)
{
    CPLDebug("MRF_IO", "IRasterIO %s, %d, %d, %d, %d, bufsz %d,%d,%d strides P %d, L %d, B %d \n",
	eRWFlag == GF_Write ? "Write" : "Read",
	nXOff, nYOff, nXSize, nYSize, nBufXSize, nBufYSize, nBandCount,
	nPixelSpace, nLineSpace, nBandSpace);

    //
    // Call the parent implementation, which splits it into bands and calls their IRasterIO
    // 
    return GDALPamDataset::IRasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize, pData, nBufXSize, nBufYSize,
	eBufType, nBandCount, panBandMap, nPixelSpace, nLineSpace, nBandSpace);
}


/**
*\brief Build some overviews
*
*  if nOverviews is 0, erase the overviews (reduce to base image only)
*/

CPLErr GDALMRFDataset::IBuildOverviews(
    const char * pszResampling,
    int nOverviews, int * panOverviewList,
    int nBands, int * panBandList,
    GDALProgressFunc pfnProgress, void * pProgressData)

{
    CPLErr       eErr = CE_None;

    CPLDebug("MRF_OVERLAY", "IBuildOverviews %d, bands %d\n", nOverviews, nBands);

    /* -------------------------------------------------------------------- */
    /*      If we don't have read access, then create the overviews         */
    /*      externally.                                                     */
    /*      Copied from the GTIFF driver, but doesn't work, just prints a   */
    /*      "not supported" message                                         */
    /*      Don't really know how to use the overview system                */
    /*                                                                      */
    /* -------------------------------------------------------------------- */
    if (GetAccess() != GA_Update)
    {
	CPLDebug("MRF", "File open read-only, creating overviews externally.");

	return GDALDataset::IBuildOverviews(
	    pszResampling, nOverviews, panOverviewList,
	    nBands, panBandList, pfnProgress, pProgressData);
    }

    /* -------------------------------------------------------------------- */
    /*      If zero overviews were requested, we need to clear all          */
    /*      existing overviews.                                             */
    /*      This should just clear the index file                           */
    /*      Right now it just fails or does nothing                         */
    /* -------------------------------------------------------------------- */

    if (nOverviews == 0)
    {
	if (current.size.l == 0)
	    return GDALDataset::IBuildOverviews(
	    pszResampling, nOverviews, panOverviewList,
	    nBands, panBandList, pfnProgress, pProgressData);
	else
	    return CleanOverviews();
    }

    // Array of source bands
    GDALRasterBand **papoBandList =
	(GDALRasterBand **)CPLCalloc(sizeof(void*), nBands);
    // Array of destination bands
    GDALRasterBand **papoOverviewBandList =
	(GDALRasterBand **)CPLCalloc(sizeof(void*), nBands);
    // Triple level pointer, that's what GDAL ROMB wants
    GDALRasterBand ***papapoOverviewBands =
	(GDALRasterBand ***)CPLCalloc(sizeof(void*), nBands);

    try {  // Throw an error code, to make sure memory gets freed properly
	// Modify the metadata file if it doesn't already have the Rset model set
	if (0.0 == scale) {
	    CPLXMLNode *config = ReadConfig();
	    try {
		const char* model = CPLGetXMLValue(config, "Rsets.model", "uniform");
		if (!EQUAL(model, "uniform")) {
		    CPLError(CE_Failure, CPLE_AppDefined,
			"MRF:IBuildOverviews, Overviews not implemented for model %s", model);
		    throw CE_Failure;
		}

		// The scale value is the same as first overview
		scale = strtod(CPLGetXMLValue(config, "Rsets.scale",
		    CPLString().Printf("%d", panOverviewList[0]).c_str()), 0);

		// Initialize the empty overlays, all of them for a given scale
		// They could already exist, in which case they are not erased
		GIntBig idxsize = AddOverviews(int(scale));
		if (!CheckFileSize(current.idxfname, idxsize, GA_Update)) {
		    CPLError(CE_Failure, CPLE_AppDefined, "MRF: Can't extend index file");
		    return CE_Failure;
		}

		//  Set the uniform node, in case it was not set before, and save the new configuration
		CPLSetXMLValue(config, "Rsets.#model", "uniform");
		CPLSetXMLValue(config, "Rsets.#scale", CPLString().Printf("%g", scale).c_str());

		if (!WriteConfig(config)) {
		    CPLError(CE_Failure, CPLE_AppDefined, "MRF: Can't rewrite the metadata file");
		    return CE_Failure;
		}
		CPLDestroyXMLNode(config);
		config = 0;
	    }
	    catch (CPLErr e) {
		if (config)
		    CPLDestroyXMLNode(config);
		throw e; // Rethrow
	    }
	}

	for (int i = 0; i < nOverviews; i++) {

	    // Verify that scales are reasonable, val/scale has to be an integer
	    if (!IsPower(panOverviewList[i], scale)) {
		CPLError(CE_Warning, CPLE_AppDefined,
		    "MRF:IBuildOverviews, overview factor %d is not a power of %f",
		    panOverviewList[i], scale);
		continue;
	    };

	    int srclevel = -0.5 + logb(panOverviewList[i], scale);
	    GDALMRFRasterBand *b = static_cast<GDALMRFRasterBand *>(GetRasterBand(1));

	    // Warn for requests for invalid levels
	    if (srclevel >= b->GetOverviewCount()) {
		CPLError(CE_Warning, CPLE_AppDefined,
		    "MRF:IBuildOverviews, overview factor %d is not valid for this dataset",
		    panOverviewList[i]);
		continue;
	    }

	    // Generate the overview using the previous level as the source

	    // Use "avg" flag to trigger the internal average sampling
	    if (EQUAL("avg", pszResampling)) {

		// Internal, using PatchOverview
		if (srclevel > 0)
		    b = static_cast<GDALMRFRasterBand *>(b->GetOverview(srclevel - 1));

		eErr = PatchOverview(0, 0, b->nBlocksPerRow, b->nBlocksPerColumn, srclevel, 0);
		if (eErr == CE_Failure)
		    throw eErr;

	    }
	    else {
		//
		// Use the GDAL method, which is slightly different for bilinear interpolation
		// and also handles nearest mode
		//
		//
		for (int iBand = 0; iBand < nBands; iBand++) {
		    // This is the base level
		    papoBandList[iBand] = GetRasterBand(panBandList[iBand]);
		    // Set up the destination
		    papoOverviewBandList[iBand] =
			papoBandList[iBand]->GetOverview(srclevel);

		    // Use the previous level as the source, the overviews are 0 based
		    // thus an extra -1
		    if (srclevel > 0)
			papoBandList[iBand] = papoBandList[iBand]->GetOverview(srclevel - 1);

		    // Hook it up, via triple pointer level
		    papapoOverviewBands[iBand] = &(papoOverviewBandList[iBand]);
		}

		//
		// Ready, generate this overview
		// Note that this function has a bug in GDAL, the block stepping is incorect
		// It can generate multiple overview in one call, 
		// Could rewrite this loop so this function only gets called once
		//
		GDALRegenerateOverviewsMultiBand(nBands, papoBandList,
		    1, papapoOverviewBands,
		    pszResampling, pfnProgress, pProgressData);
	    }
	}
    }
    catch (CPLErr e) {
	eErr = e;
    }

    CPLFree(papapoOverviewBands);
    CPLFree(papoOverviewBandList);
    CPLFree(papoBandList);

    return eErr;
}

/*
*\brief blank separated list to vector of doubles
*/
static void list2vec(std::vector<double> &v, const char *pszList) {
    if ((pszList == NULL) || (pszList[0] == 0)) return;
    char **papszTokens = CSLTokenizeString2(pszList, " \t\n\r",
	CSLT_STRIPLEADSPACES | CSLT_STRIPENDSPACES);
    v.clear();
    for (int i = 0; i < CSLCount(papszTokens); i++)
	v.push_back(CPLStrtod(papszTokens[i], NULL));
    CSLDestroy(papszTokens);
}

void GDALMRFDataset::SetNoDataValue(const char *pszVal) {
    list2vec(vNoData, pszVal);
}

void GDALMRFDataset::SetMinValue(const char *pszVal) {
    list2vec(vMin, pszVal);
}

void GDALMRFDataset::SetMaxValue(const char *pszVal) {
    list2vec(vMax, pszVal);
}

/**
*\brief Idenfity a MRF file, lightweight
*
* Lightweight test, otherwise Open gets called.
*
*/
int GDALMRFDataset::Identify(GDALOpenInfo *poOpenInfo)

{
    CPLString fn(poOpenInfo->pszFilename);
    if (fn.find(":MRF:") != string::npos)
	return TRUE;
    if (poOpenInfo->nHeaderBytes >= 10)
	fn = (char *)poOpenInfo->pabyHeader;
    return EQUALN(fn.c_str(), "<MRF_META>", 10);
}


/**
*
*\Brief Read the XML config tree, from file
*  Caller is responsible for freeing the memory
*
* @param pszFilename the file to open.
* @return NULL on failure, or the document tree on success.
*
*/
CPLXMLNode *GDALMRFDataset::ReadConfig() const
{
    if (fname[0] == '<')
	return CPLParseXMLString(fname);
    else
	return CPLParseXMLFile(fname);
}

/**
*\Brief Write the XML config tree
* Caller is responsible for correctness of data
* and for freeing the memory
*
* @param config The document tree to write
* @return TRUE on success, FALSE otherwise
*/
int GDALMRFDataset::WriteConfig(CPLXMLNode *config)
{
    if (fname[0] == '<') return FALSE;
    return CPLSerializeXMLTreeToFile(config, fname);
}

static void
split(vector<string> & theStringVector,  // Altered/returned value
const string &theString,
size_t start = 0,
const  char theDelimiter = ' ')
{
    size_t end = theString.find(theDelimiter, start);
    if (string::npos == end) {
	theStringVector.push_back(theString.substr(start));
	return;
    }
    theStringVector.push_back(theString.substr(start, end - start));
    split(theStringVector, theString, end + 1, theDelimiter);
}

// Returns the number following the prefix if it exists in one of the vector strings
// Otherwise it returns the default
static int getnum(const vector<string> &theStringVector, const char prefix, int default) {
    for (int i = 0; i < theStringVector.size(); i++)
	if (theStringVector[i][0] == prefix)
	    return atoi(theStringVector[i].c_str() + 1);
    return default;
}

/**
*\Brief Open a MRF file
*
*/
GDALDataset *GDALMRFDataset::Open(GDALOpenInfo *poOpenInfo)

{
    CPLXMLNode *config = NULL;
    CPLErr ret = CE_None;
    const char* pszFileName = poOpenInfo->pszFilename;

    int level = -1; // All levels
    int version = 0; // Current
    int z_dimension = 1;
    string fn; // Used to parse and adjust the file name

    // Different ways to open it
    if (poOpenInfo->nHeaderBytes >= 10 &&
	EQUALN((const char *)poOpenInfo->pabyHeader, "<MRF_META>", 10)) // Regular file name
	config = CPLParseXMLFile(pszFileName);
    else {
	if (EQUALN(pszFileName, "<MRF_META>", 10)) // Content as file name
	    config = CPLParseXMLString(pszFileName);
	else
	{ // Try Ornate file name
	    fn = pszFileName;
	    size_t pos = fn.find(":MRF:");
	    if (string::npos != pos) { // Tokenize and pick known options
		vector<string> tokens;
		split(tokens, fn, pos + 5, ':');
		level = getnum(tokens, 'L', -1);
		version = getnum(tokens, 'V', 0);
		z_dimension = getnum(tokens, 'Z', 1);
		fn.resize(pos); // Cut the ornamentations
		pszFileName = fn.c_str();
		config = CPLParseXMLFile(pszFileName);
	    }
	}
    }

    if (!config)
	return NULL;

    GDALMRFDataset *ds = new GDALMRFDataset();
    ds->fname = pszFileName;
    ds->eAccess = poOpenInfo->eAccess;
    ds->level = level;

    if (level != -1) {
	// Open the whole dataset, then pick one level
	ds->cds = new GDALMRFDataset();
	ds->cds->fname = pszFileName;
	ds->cds->eAccess = ds->eAccess;
	ret = ds->cds->Initialize(config);
	if (ret == CE_None)
	    ret = ds->LevelInit(level);
    }
    else
	ret = ds->Initialize(config);

    CPLDestroyXMLNode(config);

    if (ret != CE_None) {
	delete ds;
	return NULL;
    }

    // Open a single version
    if (version != 0)
	ret = ds->SetVersion(version);

    if (ret != CE_None) {
	delete ds;
	return NULL;
    }

    // If not set by the band, get a pageSizeBytes buffer
    if (ds->GetPBufferSize() == 0)
	ds->SetPBuffer(ds->current.pageSizeBytes);

    // Tell PAM what our real file name is, to help it find the aux.xml
    ds->SetPhysicalFilename(pszFileName);
    // Don't mess with metadata after this, otherwise PAM will re-write the aux.xml
    ds->TryLoadXML();
    return ds;
}

// Adjust the band images with the right offset, then adjust the sizes
CPLErr GDALMRFDataset::SetVersion(int version) {
    if (!hasVersions || version > verCount) {
	CPLError(CE_Failure, CPLE_AppDefined, "GDAL MRF: Version number error!");
	return CE_Failure;
    }
    // Size of one version index
    for (int bcount = 1; bcount <= nBands; bcount++) {
	GDALMRFRasterBand *srcband = (GDALMRFRasterBand *)GetRasterBand(bcount);
	srcband->img.idxoffset += idxSize*verCount;
	for (int l = 0; l < srcband->GetOverviewCount(); l++) {
	    GDALMRFRasterBand *band = (GDALMRFRasterBand *)srcband->GetOverview(l);
	    band->img.idxoffset += idxSize*verCount;
	}
    }
    hasVersions = 0;
    return CE_None;
}

CPLErr GDALMRFDataset::LevelInit(const int l) {

    // Test that this level does exist
    if (l < 0 || l >= cds->GetRasterBand(1)->GetOverviewCount()) {
	CPLError(CE_Failure, CPLE_AppDefined, "GDAL MRF: Overview not present!");
	return CE_Failure;
    }

    GDALMRFRasterBand *srcband = (GDALMRFRasterBand *)cds->GetRasterBand(1)->GetOverview(l);
    // Copy the sizes from this level
    current = full = srcband->img;
    current.size.c = cds->current.size.c;
    scale = cds->scale;
    SetProjection(cds->GetProjectionRef());

    SetMetadataItem("INTERLEAVE", OrderName(current.order), "IMAGE_STRUCTURE");
    SetMetadataItem("COMPRESSION", CompName(current.comp), "IMAGE_STRUCTURE");

    for (int i = 0; i < 6; i++)
	GeoTransform[i] = cds->GeoTransform[i];
    for (int i = 0; i < l; i++) {
	GeoTransform[1] /= scale;
	GeoTransform[5] /= scale;
    }

    nRasterXSize = current.size.x;
    nRasterYSize = current.size.y;
    nBands = current.size.c;
    hasVersions = cds->hasVersions;
    verCount = cds->verCount;

    bGeoTransformValid = TRUE;

    // Add the bands, copy constructor so they can be closed independently
    for (int i = 1; i <= nBands; i++) {
	GDALMRFLRasterBand *band = new GDALMRFLRasterBand((GDALMRFRasterBand *)
	    cds->GetRasterBand(i)->GetOverview(l));

	SetBand(i, band);
	band->SetColorInterpretation(band->GetColorInterpretation());
    }

    return CE_None;
}

// Is the string positive or not
inline bool on(const char *pszValue) {
    if (!pszValue || pszValue[0] == 0)
	return false;
    return (EQUAL(pszValue, "ON") || EQUAL(pszValue, "TRUE") || EQUAL(pszValue, "YES"));
}

/**
*\brief Initialize the image structure and the dataset from the XML Raster node
*
* @param image, the structure to be initialized
* @param config, the Raster node of the xml structure
* @param ds, the parent dataset, some things get inherited
*
* The structure should be initialized with the default values as much as possible
*
*/

static CPLErr Init_Raster(ILImage &image, GDALMRFDataset *ds, CPLXMLNode *defimage)
{
    CPLXMLNode *node; // temporary
    if (!defimage) {
	CPLError(CE_Failure, CPLE_AppDefined, "GDAL MRF: Can't find raster info");
	return CE_Failure;
    }

    // Size is mandatory
    node = CPLGetXMLNode(defimage, "Size");
    if (!node) {
	CPLError(CE_Failure, CPLE_AppDefined, "No size defined");
	return CE_Failure;
    }

    image.size = ILSize(
	static_cast<int>(getXMLNum(node, "x", -1)),
	static_cast<int>(getXMLNum(node, "y", -1)),
	static_cast<int>(getXMLNum(node, "z", 1)),
	static_cast<int>(getXMLNum(node, "c", 1)),
	0);
    // Basic checks
    if (image.size.x < 1 || image.size.y < 1) {
	CPLError(CE_Failure, CPLE_AppDefined, "Need at least x,y size");
	return CE_Failure;
    }

    //  Pagesize, defaults to 512,512,1,c
    image.pagesize = ILSize(
	MIN(512, image.size.x),
	MIN(512, image.size.y),
	1,
	image.size.c);

    node = CPLGetXMLNode(defimage, "PageSize");
    if (node) image.pagesize = ILSize(
	static_cast<int>(getXMLNum(node, "x", image.pagesize.x)),
	static_cast<int>(getXMLNum(node, "y", image.pagesize.y)),
	static_cast<int>(getXMLNum(node, "z", image.pagesize.z)),
	static_cast<int>(getXMLNum(node, "c", image.pagesize.c)));

    // Orientation, some other systems might support something 
    //   if (!EQUAL(CPLGetXMLValue(defimage,"Orientation","TL"), "TL")) {
    //// GDAL only handles Top Left Images
    //CPLError(CE_Failure, CPLE_AppDefined, "GDAL MRF: Only Top-Left orientation is supported");
    //return CE_Failure;
    //   }

    // Page Encoding, defaults to PNG
    image.comp = CompToken(CPLGetXMLValue(defimage, "Compression", "PNG"));

    if (image.comp == IL_ERR_COMP) {
	CPLError(CE_Failure, CPLE_AppDefined,
	    "GDAL MRF: Compression %s is unknown",
	    CPLGetXMLValue(defimage, "Compression", NULL));
	return CE_Failure;
    }

    // Is there a palette?
    //
    // GDAL only supports RGB+A palette, the other modes don't work
    //
    // Format is
    // <Palette>
    //   <Size>N</Size> : Optional
    //   <Model>RGBA|RGB|CMYK|HSV|HLS|L</Model> :mandatory
    //   <Entry idx=i c1=v1 c2=v2 c3=v3 c4=v4/> :Optional
    //   <Entry .../>
    // </Palette>
    // the idx attribute is optional, it autoincrements
    // The entries are actually vertices, interpolation takes place inside
    // The palette starts initialized with zeros
    // HSV and HLS are the similar, with c2 and c3 swapped
    // RGB or RGBA are same
    // 

    if ((image.pagesize.c == 1) && (NULL != (node = CPLGetXMLNode(defimage, "Palette")))) {
	int entries = static_cast<int>(getXMLNum(node, "Size", 255));
	GDALPaletteInterp eInterp = GPI_RGB;
	// A flag to convert from HLS to HSV
	CPLString pModel = CPLGetXMLValue(node, "Model", "RGB");

	if ((entries > 0) && (entries < 257)) {
	    int start_idx, end_idx;
	    GDALColorEntry ce_start = { 0, 0, 0, 255 }, ce_end = { 0, 0, 0, 255 };

	    // Create it and initialize it to nothing
	    GDALColorTable *poColorTable = new GDALColorTable(eInterp);
	    poColorTable->CreateColorRamp(0, &ce_start, entries - 1, &ce_end);
	    // Read the values
	    CPLXMLNode *p = CPLGetXMLNode(node, "Entry");
	    if (p) {
		// Initialize the first entry
		ce_start = GetXMLColorEntry(p);
		start_idx = static_cast<int>(getXMLNum(p, "idx", 0));
		if (start_idx < 0) {
		    CPLError(CE_Failure, CPLE_AppDefined,
			"GDAL MRF: Palette index %d not allowed", start_idx);
		    delete poColorTable;
		    return CE_Failure;
		}
		poColorTable->SetColorEntry(start_idx, &ce_start);
		while (NULL != (p = SearchXMLSiblings(p, "Entry"))) {
		    // For every entry, create a ramp
		    ce_end = GetXMLColorEntry(p);
		    end_idx = static_cast<int>(getXMLNum(p, "idx", start_idx + 1));
		    if ((end_idx <= start_idx) || (start_idx >= entries)) {
			CPLError(CE_Failure, CPLE_AppDefined,
			    "GDAL MRF: Index Error at index %d", end_idx);
			delete poColorTable;
			return CE_Failure;
		    }
		    poColorTable->CreateColorRamp(start_idx, &ce_start,
			end_idx, &ce_end);
		    ce_start = ce_end;
		    start_idx = end_idx;
		}
	    }

	    ds->SetColorTable(poColorTable);
	}
	else {
	    CPLError(CE_Failure, CPLE_AppDefined, "GDAL MRF: Palette definition error");
	    return CE_Failure;
	}
    }

    // Order of increment
    image.order = OrderToken(CPLGetXMLValue(defimage, "Order",
	(image.pagesize.c != image.size.c) ? "BAND" : "PIXEL"));
    if (image.order == IL_ERR_ORD) {
	CPLError(CE_Failure, CPLE_AppDefined, "GDAL MRF: Order %s is unknown",
	    CPLGetXMLValue(defimage, "Order", NULL));
	return CE_Failure;
    }

    image.quality = atoi(CPLGetXMLValue(defimage, "Quality", "85"));

    if (image.quality < 0 && image.quality>99) {
	CPLError(CE_Warning, CPLE_AppDefined, "GDAL MRF: Quality setting error, using default of 85");
	image.quality = 85;
    }

    // Data Type, use GDAL Names
    image.dt = GDALGetDataTypeByName(
	CPLGetXMLValue(defimage, "DataType", GDALGetDataTypeName(image.dt)));
    if (image.dt == GDT_Unknown) {
	CPLError(CE_Failure, CPLE_AppDefined, "GDAL MRF: Image has wrong type");
	return CE_Failure;
    }

    // Check the endianess if needed, assume host order
    if (is_Endianess_Dependent(image.dt, image.comp))
	image.nbo = on(CPLGetXMLValue(defimage, "NetByteOrder", "No"));

    CPLXMLNode *DataValues = CPLGetXMLNode(defimage, "DataValues");
    if (NULL != DataValues) {
	const char *pszValue;
	pszValue = CPLGetXMLValue(DataValues, "NoData", 0);
	if (pszValue) ds->SetNoDataValue(pszValue);
	pszValue = CPLGetXMLValue(DataValues, "min", 0);
	if (pszValue) ds->SetMinValue(pszValue);
	pszValue = CPLGetXMLValue(DataValues, "max", 0);
	if (pszValue) ds->SetMaxValue(pszValue);
    }

    // Calculate the page size in bytes
    image.pageSizeBytes = GDALGetDataTypeSize(image.dt) / 8 *
	image.pagesize.x * image.pagesize.y * image.pagesize.z * image.pagesize.c;

    // Calculate the page count, including the total for the level
    image.pagecount = pcount(image.size, image.pagesize);

    // Data File Name and offset
    image.datfname = getFname(defimage, "DataFile", ds->GetFname(), ILComp_Ext[image.comp]);
    image.dataoffset = static_cast<int>(
	getXMLNum(CPLGetXMLNode(defimage, "DataFile"), "offset", 0));

    // Index File Name and offset
    image.idxfname = getFname(defimage, "IndexFile", ds->GetFname(), ".idx");
    image.idxoffset = static_cast<int>(
	getXMLNum(CPLGetXMLNode(defimage, "IndexFile"), "offset", 0));

    return CE_None;
}

char      **GDALMRFDataset::GetFileList()
{
    char** papszFileList = 0;

    // Add the header file name if it is real
    VSIStatBufL  sStat;
    if (VSIStatExL(fname, &sStat, VSI_STAT_EXISTS_FLAG) == 0)
	papszFileList = CSLAddString(papszFileList, fname);

    // These two should be real
    // We don't really want to add these files, since they will be erased when an mrf is overwritten
    // This collides with the concept that the data file never shrinks.  Same goes with the index, in case
    // we just want to add things to it.
    //    papszFileList = CSLAddString( papszFileList, full.datfname);
    //    papszFileList = CSLAddString( papszFileList, full.idxfname);
    //    if (!source.empty())
    //	papszFileList = CSLAddString( papszFileList, source);

    return papszFileList;
}

// Returns the dataset index file or null
VSILFILE *GDALMRFDataset::IdxFP() {
    if (ifp.FP != NULL)
	return ifp.FP;
    char *mode = "rb";
    ifp.acc = GF_Read;

    if (eAccess == GA_Update || !source.empty()) {
	mode = bCrystalized ? "r+b" : "w+b";
	ifp.acc = GF_Write;
    }

    ifp.FP = VSIFOpenL(current.idxfname, mode);

    int expected_size = idxSize;
    if (clonedSource) expected_size *= 2;

    // Got it open or it doesn't need one
    //
    // The NONE compression could have two modes, without an index (implicit order and size) or with index
    // But we can't tell them apart as of now.  So the only mode working now is the indexed one
    // 
    // if (ifp.FP || current.comp == IL_NONE)
    if (NULL != ifp.FP) {

	if (!bCrystalized && !CheckFileSize(current.idxfname, expected_size, GA_Update)) {
	    CPLError(CE_Failure, CPLE_AppDefined, "Can't extend the cache index file %s",
		current.idxfname.c_str());
	    return NULL;
	}

	if (source.empty())
	    return ifp.FP;

	// Make sure the index is large enough before proceeding
	// Timeout in .1 seconds, can't really guarantee the accuracy
	// So this is about half second, more than sufficient
	int timeout = 5;
	do {
	    if (CheckFileSize(current.idxfname, expected_size, GA_ReadOnly))
		return ifp.FP;
	    MRF_sleep_ms(100);
	} while (--timeout);

	// If we get here it is a time-out
	CPLError(CE_Failure, CPLE_AppDefined,
	    "GDAL MRF: Timeout on fetching cloned index file %s\n", current.idxfname.c_str());
	return NULL;
    }

    // Error if this is not a caching MRF
    if (source.empty()) {
	CPLError(CE_Failure, CPLE_AppDefined,
	    "GDAL MRF: Can't open index file %s\n", current.idxfname.c_str());
	return NULL;
    }

    // Caching/Cloning MRF and index could be read only
    // Is this actually works, we should try again, maybe somebody else just created the file?
    mode = "rb";
    ifp.acc = GF_Read;
    ifp.FP = VSIFOpenL(current.idxfname, mode);
    if (NULL != ifp.FP)
	return ifp.FP;

    // Caching and index file absent, create it
    // Due to a race, multiple processes might do this at the same time, but that is fine
    ifp.FP = VSIFOpenL(current.idxfname, "wb");
    if (NULL == ifp.FP) {
	CPLError(CE_Failure, CPLE_AppDefined, "Can't create the MRF cache index file %s",
	    current.idxfname.c_str());
	return NULL;
    }
    VSIFCloseL(ifp.FP);
    ifp.FP = NULL;

    // Make it large enough for caching and for cloning
    if (!CheckFileSize(current.idxfname, expected_size, GA_Update)) {
	CPLError(CE_Failure, CPLE_AppDefined, "Can't extend the cache index file %s",
	    current.idxfname.c_str());
	return NULL;
    }

    // Try opening it again in rw mode so we can read and write into it
    mode = "r+b";
    ifp.acc = GF_Write;
    ifp.FP = VSIFOpenL(current.idxfname.c_str(), mode);

    if (NULL == ifp.FP) {
	CPLError(CE_Failure, CPLE_AppDefined,
	    "GDAL MRF: Can't reopen cache index file %s\n", full.idxfname.c_str());
	return NULL;
    }
    return ifp.FP;
}

//
// Returns the dataset data file or null 
// Data file is opened either in Read or Append mode, never in straight write
//
VSILFILE *GDALMRFDataset::DataFP() {
    if (dfp.FP != NULL)
	return dfp.FP;
    char *mode = "rb";
    dfp.acc = GF_Read;

    // Open it for writing if updating or if caching
    if (eAccess == GA_Update || !source.empty()) {
	mode = "a+b";
	dfp.acc = GF_Write;
    }

    dfp.FP = VSIFOpenL(current.datfname.c_str(), mode);
    if (dfp.FP)
	return dfp.FP;

    // It could be a caching MRF
    if (source.empty()) {
	CPLError(CE_Failure, CPLE_AppDefined,
	    "GDAL MRF: Can't open data file %s\n", current.datfname.c_str());
	return dfp.FP;
    }

    // Cloud be there but read only, remember it was open that way
    mode = "rb";
    dfp.acc = GF_Read;
    dfp.FP = VSIFOpenL(current.datfname.c_str(), mode);
    if (NULL != dfp.FP) {
	CPLDebug("MRF_IO", "Opened %s RO mode %s\n", current.datfname.c_str(), mode);
	return dfp.FP;
    }

    // We should have created it above with "a+b"
    CPLError(CE_Failure, CPLE_AppDefined,
	"GDAL MRF: Can't open data file %s\n", current.datfname.c_str());
    return dfp.FP;
};

// Builds an XML tree from the current MRF.  If written to a file it becomes an MRF
CPLXMLNode * GDALMRFDataset::BuildConfig()
{
    CPLXMLNode *config = CPLCreateXMLNode(NULL, CXT_Element, "MRF_META");

    if (!source.empty()) {
	CPLXMLNode *CS = CPLCreateXMLNode(config, CXT_Element, "CachedSource");
	// Should wrap the string in CDATA, in case it is XML
	CPLXMLNode *S = CPLCreateXMLElementAndValue(CS, "Source", source);
	if (clonedSource)
	    CPLSetXMLValue(S, "#clone", "true");
    }

    // Use the full size
    CPLXMLNode *raster = CPLCreateXMLNode(config, CXT_Element, "Raster");
    XMLSetAttributeVal(raster, "Size", full.size, "%.0f");

    if (full.comp != IL_PNG)
	CPLCreateXMLElementAndValue(raster, "Compression", CompName(full.comp));

    if (full.dt != GDT_Byte)
	CPLCreateXMLElementAndValue(raster, "DataType", GDALGetDataTypeName(full.dt));

    if (vNoData.size() || vMin.size() || vMax.size()) {
	CPLXMLNode *values = CPLCreateXMLNode(raster, CXT_Element, "DataValues");
	XMLSetAttributeVal(values, "NoData", vNoData);
	XMLSetAttributeVal(values, "min", vMin);
	XMLSetAttributeVal(values, "max", vMax);
    }

    // palette, if we have one
    if (poColorTable != NULL) {
	CPLXMLNode *pal = CPLCreateXMLNode(raster, CXT_Element, "Palette");
	int sz = poColorTable->GetColorEntryCount();
	if (sz != 256)
	    XMLSetAttributeVal(pal, "Size", poColorTable->GetColorEntryCount());
	// RGB or RGBA for now
	for (int i = 0; i < sz; i++) {
	    CPLXMLNode *entry = CPLCreateXMLNode(pal, CXT_Element, "Entry");
	    const GDALColorEntry *ent = poColorTable->GetColorEntry(i);
	    // No need to set the index, it is always from 0 no size-1
	    XMLSetAttributeVal(entry, "c1", ent->c1);
	    XMLSetAttributeVal(entry, "c2", ent->c2);
	    XMLSetAttributeVal(entry, "c3", ent->c3);
	    if (ent->c4 != 255)
		XMLSetAttributeVal(entry, "c4", ent->c4);
	}
    }

    if (is_Endianess_Dependent(full.dt, full.comp)) // Need to set the order
	CPLCreateXMLElementAndValue(raster, "NetByteOrder",
	(full.nbo || NET_ORDER) ? "TRUE" : "FALSE");

    if (full.quality > 0 && full.quality != 85)
	CPLCreateXMLElementAndValue(raster, "Quality",
	CPLString().Printf("%d", full.quality));

    XMLSetAttributeVal(raster, "PageSize", full.pagesize, "%.0f");
    // Done with the raster node

    if (scale) {
	CPLCreateXMLNode(config, CXT_Element, "Rsets");
	CPLSetXMLValue(config, "Rsets.#model", "uniform");
	CPLSetXMLValue(config, "Rsets.#scale",
	    CPLString().Printf("%d", scale));
    }
    CPLXMLNode *gtags = CPLCreateXMLNode(config, CXT_Element, "GeoTags");

    // Do we have an affine transform different from identity?
    double gt[6];
    if ((GetGeoTransform(gt) == CE_None) &&
	(gt[0] != 0 || gt[1] != 1 || gt[2] != 0 ||
	gt[3] != 0 || gt[4] != 0 || gt[5] != 1))
    {
	double minx = gt[0];
	double maxx = gt[1] * full.size.x + minx;
	double maxy = gt[3];
	double miny = gt[5] * full.size.y + maxy;
	CPLXMLNode *bbox = CPLCreateXMLNode(gtags, CXT_Element, "BoundingBox");
	XMLSetAttributeVal(bbox, "minx", minx);
	XMLSetAttributeVal(bbox, "miny", miny);
	XMLSetAttributeVal(bbox, "maxx", maxx);
	XMLSetAttributeVal(bbox, "maxy", maxy);
    }

    const char *pszProj = GetProjectionRef();
    if (pszProj && (!EQUAL(pszProj, "")))
	CPLCreateXMLElementAndValue(gtags, "Projection", pszProj);

    if (optlist.size()) {
	CPLString options;
	for (int i = 0; i < optlist.size(); i++) {
	    options += optlist[i];
	    options += ' ';
	}
	options.resize(options.size() - 1);
	CPLCreateXMLElementAndValue(config, "Options", options);
    }

    return config;
}


/**
* \Brief Populates the dataset variables from the XML definition
*
*
*/
CPLErr GDALMRFDataset::Initialize(CPLXMLNode *config)

{
    // We only need a basic initialization here, usually gets overwritten by the image params
    full.dt = GDT_Byte;
    full.hasNoData = false;
    full.NoDataValue = 0;
    Quality = 85;

    CPLErr ret = Init_Raster(full, this, CPLGetXMLNode(config, "Raster"));

    hasVersions = on(CPLGetXMLValue(config, "Raster.versioned", "no"));

    Quality = full.quality;
    if (CE_None != ret)
	return ret;

    // Bounding box
    CPLXMLNode *bbox = CPLGetXMLNode(config, "GeoTags.BoundingBox");
    if (NULL != bbox) {
	double x0, x1, y0, y1;

	x0 = atof(CPLGetXMLValue(bbox, "minx", "0"));
	x1 = atof(CPLGetXMLValue(bbox, "maxx", "1"));
	y1 = atof(CPLGetXMLValue(bbox, "maxy", "1"));
	y0 = atof(CPLGetXMLValue(bbox, "miny", "0"));

	GeoTransform[0] = x0;
	GeoTransform[1] = (x1 - x0) / full.size.x;
	GeoTransform[2] = 0;
	GeoTransform[3] = y1;
	GeoTransform[4] = 0;
	GeoTransform[5] = (y0 - y1) / full.size.y;
	bGeoTransformValid = TRUE;
    }

    SetProjection(CPLGetXMLValue(config, "GeoTags.Projection", ""));

    // Copy the full size to current, data and index are not yet open
    current = full;
    // Bands can be used to overwrite from the whole c size
    current.size.c = static_cast<int>(getXMLNum(config, "Bands", current.size.c));

    // Dataset metadata setup
    SetMetadataItem("INTERLEAVE", OrderName(current.order), "IMAGE_STRUCTURE");
    SetMetadataItem("COMPRESSION", CompName(current.comp), "IMAGE_STRUCTURE");
    if (is_Endianess_Dependent(current.dt, current.comp))
	SetMetadataItem("NETBYTEORDER", current.nbo ? "TRUE" : "FALSE", "IMAGE_STRUCTURE");

    // Open the files for the current image, either RW or RO
    nRasterXSize = current.size.x;
    nRasterYSize = current.size.y;
    nBands = current.size.c;

    if (!nBands || !nRasterXSize || !nRasterYSize) {
	CPLError(CE_Failure, CPLE_AppDefined, "GDAL MRF: Image size missing");
	return CE_Failure;
    }

    // Pick up the source data image, if there is one
    source = CPLStrdup(CPLGetXMLValue(config, "CachedSource.Source", 0));
    // Is it a clone?
    clonedSource = on(CPLGetXMLValue(config, "CachedSource.Source.clone", "no"));
    // Pick up the options, if any
    optlist.Assign(CSLTokenizeString2(CPLGetXMLValue(config, "Options", 0),
	" \t\n\r", CSLT_STRIPLEADSPACES | CSLT_STRIPENDSPACES));

    // Load all the options in the IMAGE_STRUCTURE metadata
    for (int i = 0; i < optlist.Count(); i++) {
	CPLString s(optlist[i]);
	s.resize(s.find_first_of(":="));
	const char *key = s.c_str();
	SetMetadataItem(key, optlist.FetchNameValue(key), "IMAGE_STRUCTURE");
    }

    // We have the options, so we can call rasterband
    CPLXMLNode *rsets = CPLGetXMLNode(config, "Rsets");
    for (int i = 1; i <= nBands; i++) {
	// The subimages are low resolution copies of the current one.
	GDALMRFRasterBand *band = newMRFRasterBand(this, current, i);
	GDALColorInterp ci = GCI_Undefined;

	switch (nBands) {
	case 1:
	case 2:
	    ci = (i == 1) ? GCI_GrayIndex : GCI_AlphaBand;
	    break;
	case 3:
	case 4:
	    if (i < 3)
		ci = (i == 1) ? GCI_RedBand : GCI_GreenBand;
	    else
		ci = (i == 3) ? GCI_BlueBand : GCI_AlphaBand;
	}

	if (GetColorTable())
	    ci = GCI_PaletteIndex;

	if (optlist.FetchBoolean("MULTISPECTRAL", FALSE))
	    ci = GCI_Undefined;

	band->SetColorInterpretation(ci);
	SetBand(i, band);
    }

    if (NULL != rsets && NULL != rsets->psChild) {
	// We have rsets 

	// Regular spaced overlays, until everything fits in a single tile
	if (EQUAL("uniform", CPLGetXMLValue(rsets, "model", "uniform"))) {
	    scale = getXMLNum(rsets, "scale", 2.0);
	    if (scale <= 1) {
		CPLError(CE_Failure, CPLE_AppDefined, "MRF: zoom factor less than unit not allowed");
		return CE_Failure;
	    }
	    // Looks like there are overlays
	    AddOverviews(int(scale));
	}
	else {
	    CPLError(CE_Failure, CPLE_AppDefined, "Unknown Rset definition");
	    return CE_Failure;
	}

    }

    // Just in case we need it
    idxSize = IdxSize(full, scale);

    if (hasVersions) { // It has versions, but how many?
	verCount = 0; // Assume it only has one
	VSIStatBufL statb;
	//  If the file exists, compute the last version number
	if (0 == VSIStatL(full.idxfname, &statb))
	    verCount = int(statb.st_size / idxSize - 1);
    }

    return CE_None;
}

static inline bool has_path(const CPLString &name)
{
    return name.find_first_of("/\\") != string::npos;
}

// Is the name a relative file name?
static inline bool is_relative(const CPLString &name)
{
    return (!has_path(name) || name[0] == '.' || (name[1] == ':' && isalpha(name[0])));
}

// Add the folder part of path to the begining of the source, if it is relative
static inline void addpath(CPLString &name, const CPLString &path)
{
    if (is_relative(name))
	name = path.substr(0, path.find_last_of("/\\") + 1) + name;
}

/**
*\Brief Get the source dataset, open it if necessary
*/
GDALDataset *GDALMRFDataset::GetSrcDS() {
    if (poSrcDS) return poSrcDS;
    if (source.empty())	return 0;
    // add the path from the current MRF if the source doesn't have one
    if (is_relative(source) && has_path(fname))
	addpath(source, fname);
    poSrcDS = (GDALDataset *)GDALOpenShared(source.c_str(), GA_ReadOnly);
    if (0 == source.find("<MRF_META>") && has_path(fname))
    {// XML MRF source, might need to patch the file names
	GDALMRFDataset *psDS = reinterpret_cast<GDALMRFDataset *>(poSrcDS);
	addpath(psDS->current.datfname, fname);
	addpath(psDS->current.idxfname, fname);
    }
    return poSrcDS;
}

/**
*\Brief Add or verify that all overlays exits
*
* @return size of the index file
*/

GIntBig GDALMRFDataset::AddOverviews(int scale) {
    // Fit the overlays
    ILImage img = full;
    while (1 != img.pagecount.x*img.pagecount.y)
    {
	// Adjust the offsets for indices
	img.idxoffset += sizeof(ILIdx)*img.pagecount.l;
	img.size.x = pcount(img.size.x, scale);
	img.size.y = pcount(img.size.y, scale);
	img.size.l++; // Increment the level
	img.pagecount = pcount(img.size, img.pagesize);
	// Create and register the the overviews for each band
	for (int i = 1; i <= nBands; i++) {
	    GDALMRFRasterBand *b = (GDALMRFRasterBand *)GetRasterBand(i);
	    if (!(b->GetOverview(img.size.l - 1)))
		b->AddOverview(newMRFRasterBand(this, img, i, img.size.l));
	}
    }

    // Last adjustment, should be a single set of c and z tiles
    return img.idxoffset + sizeof(ILIdx)*img.pagecount.l;
}

// Try to implement CreateCopy using Create
GDALDataset *GDALMRFDataset::CreateCopy(const char *pszFilename,
    GDALDataset *poSrcDS, int bStrict, char **papszOptions,
    GDALProgressFunc pfnProgress, void *pProgressData)
{
    ILImage img;

    int x = poSrcDS->GetRasterXSize();
    int y = poSrcDS->GetRasterYSize();
    int nBands = poSrcDS->GetRasterCount();
    GDALRasterBand *poSrcBand1 = poSrcDS->GetRasterBand(1);

    GDALDataType dt = poSrcBand1->GetRasterDataType();
    GDALMRFDataset *poDS = NULL;

    // Have our own options, to modify as we want
    char **options = CSLDuplicate(papszOptions);

    const char *pszValue = poSrcDS->GetMetadataItem("INTERLEAVE", "IMAGE_STRUCTURE");
    options = CSLAddIfMissing(options, "INTERLEAVE", pszValue ? pszValue : "PIXEL");

    try {
	poDS = reinterpret_cast<GDALMRFDataset *>(
	    Create(pszFilename, x, y, nBands, dt, options));

	if (poDS->bCrystalized)
	    throw CPLString("Create call failed");

	img = poDS->current; // Deal with the current one here


	// Copy data values from source
	int bHas;
	double dfData;
	for (int i = 0; i < poDS->nBands; i++) {
	    dfData = poSrcDS->GetRasterBand(i + 1)->GetNoDataValue(&bHas);
	    if (bHas)
		poDS->vNoData.push_back(dfData);
	}

	for (int i = 0; i < poDS->nBands; i++) {
	    dfData = poSrcDS->GetRasterBand(i + 1)->GetMinimum(&bHas);
	    if (bHas)
		poDS->vMin.push_back(dfData);
	}

	for (int i = 0; i < poDS->nBands; i++) {
	    dfData = poSrcDS->GetRasterBand(i + 1)->GetMaximum(&bHas);
	    if (bHas)
		poDS->vMax.push_back(dfData);
	}

	// Geotags
	double gt[6];
	if (CE_None == poSrcDS->GetGeoTransform(gt))
	    poDS->SetGeoTransform(gt);

	const char *pszProj = poSrcDS->GetProjectionRef();
	if (pszProj && pszProj[0])
	    poDS->SetProjection(pszProj);

	// Color palette if we only have one band
	if (1 == nBands && GCI_PaletteIndex == poSrcBand1->GetColorInterpretation())
	    poDS->SetColorTable(poSrcBand1->GetColorTable()->Clone());

	// Finally write the XML in the right file name
	poDS->Crystalize();
    }
    catch (CPLString(e)) {
	if (poDS)
	    delete poDS;
	CPLError(CE_Failure, CPLE_ObjectNull, e.c_str());
	poDS = NULL;
    }

    CSLDestroy(options);

    // If copy is disabled, we're done, we just created an empty MRF
    if (on(CSLFetchNameValue(papszOptions, "NOCOPY")))
	return poDS;

    // Use the GDAL copy call
    // Need to flag the dataset as compressed (COMPRESSED=TRUE) to force block writes
    // This might not be what we want, if the input and out order is truly separate
    char **papszCWROptions = CSLDuplicate(0);
    papszCWROptions = CSLAddNameValue(papszCWROptions, "COMPRESSED", "TRUE");
    CPLErr err = GDALDatasetCopyWholeRaster((GDALDatasetH)poSrcDS,
	(GDALDatasetH)poDS, papszCWROptions, pfnProgress, pProgressData);

    CSLDestroy(papszCWROptions);

    if (CE_Failure == err) {
	delete poDS;
	// Maybe clean up the files that might have been created here?
	return NULL;
    }

    return poDS;
}

/**
 *\Brief Create an MRF file from an existing dataset
 */
GDALDataset *GDALMRFDataset::CreateCopy_(const char *pszFilename,
    GDALDataset *poSrcDS, int bStrict, char **papszOptions,
    GDALProgressFunc pfnProgress, void *pProgressData)
{
    const char *pszValue;
    GDALColorTable *poColorTable = NULL;

    ILImage img;

    // Defaults
    int nXSize = poSrcDS->GetRasterXSize();
    int nYSize = poSrcDS->GetRasterYSize();
    int nBands = poSrcDS->GetRasterCount();

    img.size = ILSize(nXSize, nYSize, 1, nBands);
    // Set some defaults
    ILCompression comp = IL_PNG;
    // Most formats can't handle more than 4 bands interleaved (JPEG,PNG)
    ILOrder ord = (nBands < 5) ? IL_Interleaved : IL_Separate;
    ILSize page(512, 512, 1, 1);
    int quality = 85;
    bool nbo = NET_ORDER;

    // Use the info from the input image
    // Use the poSrcDS or the first band to get info about the dataset
    GDALRasterBand *poPBand = poSrcDS->GetRasterBand(1);
    GDALDataType dt = poPBand->GetRasterDataType();

    // Use the blocks from the input image if it is reasonable, otherwise stick to the default
    int srcXBlk, srcYBlk;
    poPBand->GetBlockSize(&srcXBlk, &srcYBlk);
    // Ignore the line blocking that TIF emulates
    if (srcYBlk <= 2) srcYBlk = nYSize;
    if ((srcXBlk != nXSize) && (srcYBlk != nYSize)) {
	page.x = srcXBlk;
	page.y = srcYBlk;
    }

    // This could be a cached source file
    CPLString source(CPLStrdup(CSLFetchNameValue(papszOptions, "CACHEDSOURCE")));

    int clonedSource = CSLFetchBoolean(papszOptions, "CLONE", 0);

    // Get freeform params
    CPLString options(CPLStrdup(CSLFetchNameValue(papszOptions, "OPTIONS")));

    // Except if the BLOCKSIZE BLOCKXSIZE and BLOCKYSIZE are set
    pszValue = CSLFetchNameValue(papszOptions, "BLOCKSIZE");
    if (pszValue != NULL) page.x = page.y = atoi(pszValue);
    pszValue = CSLFetchNameValue(papszOptions, "BLOCKXSIZE");
    if (pszValue != NULL) page.x = atoi(pszValue);
    pszValue = CSLFetchNameValue(papszOptions, "BLOCKYSIZE");
    if (pszValue != NULL) page.y = atoi(pszValue);

    // Get the quality setting
    pszValue = CSLFetchNameValue(papszOptions, "QUALITY");
    if (pszValue != NULL)
	quality = atoi(pszValue);

    if (quality < 0 || quality > 99) {
	CPLError(CE_Warning, CPLE_AppDefined,
	    "GDAL MRF: Quality setting should be between 0 and 99, using 85");
	quality = 85;
    }

    // If the source image has a NoDataValue, min or max, we keep them
    CPLString NoData;
    CPLString Min;
    CPLString Max;
    int bHas;
    double dfData;

    for (int i = 0; i < nBands; i++) {
	dfData = poSrcDS->GetRasterBand(i + 1)->GetNoDataValue(&bHas);
	if (bHas)
	    NoData.append(PrintDouble(dfData) + " ");
    }

    for (int i = 0; i < nBands; i++) {
	dfData = poSrcDS->GetRasterBand(i + 1)->GetMinimum(&bHas);
	if (bHas)
	    Min.append(PrintDouble(dfData) + " ");
    }

    for (int i = 0; i < nBands; i++) {
	dfData = poSrcDS->GetRasterBand(i + 1)->GetMaximum(&bHas);
	if (bHas)
	    Max.append(PrintDouble(dfData) + " ");
    }

    // Network byte order requested?
    nbo = on(CSLFetchNameValue(papszOptions, "NETBYTEORDER"));

    // Use the source compression if we understand it
    comp = CompToken(poPBand->GetMetadataItem("COMPRESSION", "IMAGE_STRUCTURE"), comp);

    // Input options, overrides
    pszValue = CSLFetchNameValue(papszOptions, "COMPRESS");
    if (pszValue && IL_ERR_COMP == (comp = CompToken(pszValue))) {
	CPLError(CE_Warning, CPLE_AppDefined, "GDAL MRF: Compression %s is unknown, "
	    "using PNG", pszValue);
	comp = IL_PNG;
    }

    // Order from source, may be overwritten by options
    pszValue = poPBand->GetMetadataItem("INTERLEAVE", "IMAGE_STRUCTURE");
    if (0 != CSLFetchNameValue(papszOptions, "INTERLEAVE"))
	pszValue = CSLFetchNameValue(papszOptions, "INTERLEAVE");

    if (pszValue && IL_ERR_ORD == (ord = OrderToken(pszValue))) {
	CPLError(CE_Warning, CPLE_AppDefined, "GDAL MRF: Interleave %s is unknown", pszValue);
	return NULL;
    }


    // Error checks and synchronizations
#if defined(LERC)
    if (comp == IL_LERC)
	ord = IL_Separate;
#endif

    // If interleaved model is requested and no page size is set,
    // use the number of bands
    if (nBands > 1 && IL_Interleaved == ord)
	page.c = nBands;

    // Check compression based limitations
    if (1 != page.c) {
	if (IL_PNG == comp && page.c > 4) {
	    CPLError(CE_Failure, CPLE_AppDefined,
		"GDAL MRF: PNG can't handle %d pixel interleaved bands\n", page.c);
	    return NULL;
	}
	if (IL_JPEG == comp && ((2 == page.c) || (page.c > 4))) {
	    CPLError(CE_Failure, CPLE_AppDefined,
		"GDAL MRF: JPEG can't handle %d pixel interleaved bands\n", page.c);
	    return NULL;
	}
#if defined(LERC)
	if (IL_LERC == comp && (page.c != 3 || dt != GDT_Byte)) {
	    CPLError(CE_Failure, CPLE_AppDefined,
		"GDAL MRF: LERC can't handle interleaved bands\n");
	    return NULL;
	}
#endif
    }

    // Check data type for certain compressions
    if ((IL_JPEG == comp) && (dt != GDT_Byte)) { // For now
	CPLError(CE_Failure, CPLE_AppDefined, "GDAL MRF: JPEG compression only supports byte data");
	return NULL;
    }
    else if ((IL_PNG == comp) && (dt != GDT_Byte) && (dt != GDT_Int16) && (dt != GDT_UInt16)) {
	CPLError(CE_Failure, CPLE_AppDefined,
	    "GDAL MRF: PNG only supports 8 and 16 bits of data, format is %s", GDALGetDataTypeName(dt));
	return NULL;
    }

    // default file names
    CPLString fname_data(getFname(pszFilename, ILComp_Ext[comp]));
    CPLString fname_idx(getFname(pszFilename, ".idx"));

    // Get the color palette if we only have one band
    if (1 == nBands && GCI_PaletteIndex == poPBand->GetColorInterpretation())
	poColorTable = poPBand->GetColorTable()->Clone();

    // Check for format is PPNG and we don't have a palette
    // TODO: create option to build a palette, using the syntax from VRT LUT
    if ((poColorTable == NULL) && (comp == IL_PPNG)) {
	comp = IL_PNG;
	CPLError(CE_Warning, CPLE_AppDefined,
	    "GDAL MRF: PPNG needs a palette based input, switching to PNG");
    }

    int factor = 0;
    pszValue = CSLFetchNameValue(papszOptions, "UNIFORM_SCALE");
    if (pszValue != NULL)
	factor = atoi(pszValue);

    img.pagesize = page;

    // Build the XML file

    CPLXMLNode *config = CPLCreateXMLNode(NULL, CXT_Element, "MRF_META");
    if (!source.empty()) {
	CPLXMLNode *CS = CPLCreateXMLNode(config, CXT_Element, "CachedSource");
	// Should wrap the string in CDATA, in case it is XML
	CPLXMLNode *S = CPLCreateXMLElementAndValue(CS, "Source", source.c_str());
	if (clonedSource)
	    CPLSetXMLValue(S, "#clone", "true");
    }

    CPLXMLNode *raster = CPLCreateXMLNode(config, CXT_Element, "Raster");
    XMLSetAttributeVal(raster, "Size", ILSize(nXSize, nYSize, 1, nBands), "%.0f");

    if (comp != IL_PNG)
	CPLCreateXMLElementAndValue(raster, "Compression", CompName(comp));

    if (dt != GDT_Byte)
	CPLCreateXMLElementAndValue(raster, "DataType", GDALGetDataTypeName(dt));

    if (NoData.size() || Min.size() || Max.size()) {
	CPLXMLNode *values = CPLCreateXMLNode(raster, CXT_Element, "DataValues");
	if (NoData.size()) {
	    CPLCreateXMLNode(values, CXT_Attribute, "NoData");
	    CPLSetXMLValue(values, "NoData", NoData.c_str());
	}
	if (Min.size()) {
	    CPLCreateXMLNode(values, CXT_Attribute, "min");
	    CPLSetXMLValue(values, "min", Min.c_str());
	}
	if (Max.size()) {
	    CPLCreateXMLNode(values, CXT_Attribute, "max");
	    CPLSetXMLValue(values, "max", Max.c_str());
	}
    }

    // palette, if we have one
    if (poColorTable != NULL) {
	CPLXMLNode *pal = CPLCreateXMLNode(raster, CXT_Element, "Palette");
	int sz = poColorTable->GetColorEntryCount();
	if (sz != 256)
	    XMLSetAttributeVal(pal, "Size", poColorTable->GetColorEntryCount());
	// Should also check and set the colormodel, RGBA for now
	for (int i = 0; i < sz; i++) {
	    CPLXMLNode *entry = CPLCreateXMLNode(pal, CXT_Element, "Entry");
	    const GDALColorEntry *ent = poColorTable->GetColorEntry(i);
	    // No need to set the index, it is always from 0 no size-1
	    XMLSetAttributeVal(entry, "c1", ent->c1);
	    XMLSetAttributeVal(entry, "c2", ent->c2);
	    XMLSetAttributeVal(entry, "c3", ent->c3);
	    if (ent->c4 != 255)
		XMLSetAttributeVal(entry, "c4", ent->c4);
	}

	// Done with the palette
	delete poColorTable;
    }

    if (is_Endianess_Dependent(dt, comp)) // Need to set the order
	CPLCreateXMLElementAndValue(raster, "NetByteOrder",
	(nbo || NET_ORDER) ? "TRUE" : "FALSE");

    if (quality > 0 && quality != 85)
	CPLCreateXMLElementAndValue(raster, "Quality", CPLString().Printf("%d", quality));

    XMLSetAttributeVal(raster, "PageSize", img.pagesize, "%.0f");
    // Done with raster

    CPLCreateXMLNode(config, CXT_Element, "Rsets");
    if (factor != 0) {
	CPLSetXMLValue(config, "Rsets.#model", "uniform");
	CPLSetXMLValue(config, "Rsets.#scale", CPLString().Printf("%d", factor));
    }
    CPLXMLNode *gtags = CPLCreateXMLNode(config, CXT_Element, "GeoTags");

    // Do we have an affine transform?
    double gt[6];

    if (poSrcDS->GetGeoTransform(gt) == CE_None
	&& (gt[0] != 0 || gt[1] != 1 || gt[2] != 0 ||
	gt[3] != 0 || gt[4] != 0 || gt[5] != 1))
    {
	static const char frmt[] = "%12.8f";
	double minx = gt[0];
	double maxx = gt[1] * poSrcDS->GetRasterXSize() + minx;
	double maxy = gt[3];
	double miny = gt[5] * poSrcDS->GetRasterYSize() + maxy;
	CPLXMLNode *bbox = CPLCreateXMLNode(gtags, CXT_Element, "BoundingBox");
	XMLSetAttributeVal(bbox, "minx", minx, frmt);
	XMLSetAttributeVal(bbox, "miny", miny, frmt);
	XMLSetAttributeVal(bbox, "maxx", maxx, frmt);
	XMLSetAttributeVal(bbox, "maxy", maxy, frmt);
    }

    const char *pszProj = poSrcDS->GetProjectionRef();
    if (pszProj && (!EQUAL(pszProj, "")))
	CPLCreateXMLElementAndValue(gtags, "Projection", pszProj);
    if (options.size() != 0)
	CPLCreateXMLElementAndValue(config, "Options", options);

    // Dump the XML
    CPLSerializeXMLTreeToFile(config, pszFilename);
    CPLDestroyXMLNode(config);

    // Now that these files are created on demand, do we still need to create them here?

    // Create the data and index files, but only if they don't exist, otherwise leave them untouched
    VSILFILE *f_data = VSIFOpenL(fname_data, "r+b");
    if (NULL == f_data)
	f_data = VSIFOpenL(fname_data, "w+b");
    VSILFILE *f_idx = VSIFOpenL(fname_idx, "r+b");
    if (NULL == f_idx)
	f_idx = VSIFOpenL(fname_idx, "w+b");

    if ((NULL == f_data) || (NULL == f_idx)) {
	CPLError(CE_Failure, CPLE_AppDefined, "Can't open data or index files in update mode");
	return NULL;
    }
    // Close them
    VSIFCloseL(f_idx);
    VSIFCloseL(f_data);

    // Check or extend the index file size
    int ret = CheckFileSize(fname_idx, IdxSize(img, factor), GA_Update);

    if (!ret) {
	CPLError(CE_Failure, CPLE_AppDefined, "Can't extend the index file");
	return NULL;
    }

    // Reopen in RW mode and use the standard CopyWholeRaster
    GDALMRFDataset *poDS = (GDALMRFDataset *)GDALOpen(pszFilename, GA_Update);

    // Now that we have a dataset, try to load stuff into PAM
    poDS->CloneInfo(poSrcDS, GCIF_ONLY_IF_MISSING | GCIF_METADATA | GCIF_GCPS);

    //    if (poSrcDS->GetGCPCount() > 0)
    //	poDS->SetGCPs(poSrcDS->GetGCPCount(), poSrcDS->GetGCPs(), poSrcDS->GetGCPProjection());

    // If copy is disabled, we're done, we just created an empty MRF
    if (on(CSLFetchNameValue(papszOptions, "NOCOPY")))
	return poDS;

    // Need to flag the dataset as compressed (COMPRESSED=TRUE) to force block writes
    // This might not be what we want, if the input and out order is truly separate
    char **papszCWROptions = CSLDuplicate(0);
    papszCWROptions = CSLAddNameValue(papszCWROptions, "COMPRESSED", "TRUE");
    CPLErr err = GDALDatasetCopyWholeRaster((GDALDatasetH)poSrcDS,
	(GDALDatasetH)poDS, papszCWROptions, pfnProgress, pProgressData);

    CSLDestroy(papszCWROptions);

    if (CE_Failure == err) {
	delete poDS;
	// Maybe clean up the files that might have been created here?
	return NULL;
    }

    return poDS;
}

// Apply create options to the current dataset, only valid during creation
void GDALMRFDataset::ProcessCreateOptions(char **papszOptions)
{
    assert(!bCrystalized);
    CPLStringList opt(papszOptions, FALSE);
    ILImage &img(full);

    const char *val;

    val = opt.FetchNameValue("COMPRESS");
    if (val && IL_ERR_COMP == (img.comp = CompToken(val)))
	throw CPLString("GDAL MRF: Error setting compression");

    val = opt.FetchNameValue("INTERLEAVE");
    if (val && IL_ERR_ORD == (img.order = OrderToken(val)))
	throw CPLString("GDAL MRF: Error setting interleave");

    val = opt.FetchNameValue("QUALITY");
    if (val) img.quality = atoi(val);

    val = opt.FetchNameValue("BLOCKXSIZE");
    if (val) img.pagesize.x = atoi(val);

    val = opt.FetchNameValue("BLOCKYSIZE");
    if (val) img.pagesize.y = atoi(val);

    val = opt.FetchNameValue("BLOCKSIZE");
    if (val) img.pagesize.x = img.pagesize.y = atoi(val);

    img.nbo = opt.FetchBoolean("NETBYTEORDER", FALSE);

    val = opt.FetchNameValue("CACHEDSOURCE");
    if (val) source = val;

    val = opt.FetchNameValue("UNIFORM_SCALE");
    if (val) scale = atoi(val);

    optlist.Assign(CSLTokenizeString2(opt.FetchNameValue("OPTIONS"),
	" \t\n\r", CSLT_STRIPLEADSPACES | CSLT_STRIPENDSPACES));

    // General Fixups
    if (img.order == IL_Interleaved)
	img.pagesize.c = img.size.c;

    // Compression dependent fixups
#if defined(LERC)
    if (IL_LERC == img.comp) {
	if (img.pagesize.c != 1) {
	    if (img.order == IL_Interleaved && img.size.c > 1)
		img.order = IL_Separate;
	    img.pagesize.c = 1;
	}
    }
#endif

}

/**
 *\Brief Create an MRF dataset, some settings can be changed later
 * papszOptions might be anything that an MRF might take
 * Still missing are the georeference ...
 *
 */

GDALDataset *
GDALMRFDataset::Create(const char * pszName,
int nXSize, int nYSize, int nBands,
GDALDataType eType, char ** papszOptions)

{   // Pending create
    GDALMRFDataset *poDS = new GDALMRFDataset();
    poDS->fname = pszName;
    poDS->nBands = nBands;

    // Use the full, set some initial parameters
    ILImage &img = poDS->full;
    img.size = ILSize(nXSize, nYSize, 1, nBands);
    img.comp = IL_PNG;
    img.order = (nBands < 5) ? IL_Interleaved : IL_Separate;
    img.pagesize = ILSize(512, 512, 1, 1);
    img.quality = 85;
    img.dt = eType;
    img.dataoffset = 0;
    img.idxoffset = 0;
    img.hasNoData = false;
    img.nbo = FALSE;

    //    poDS->full = img;

    // Set the guard that tells us it needs saving before IO can take place
    poDS->bCrystalized = 0;

    // Process the options, anything that an MRF might take

    try {
	// Adjust the dataset and the full image
	poDS->ProcessCreateOptions(papszOptions);

	// Set default file names
	img.datfname = getFname(pszName, ILComp_Ext[img.comp]);
	img.idxfname = getFname(pszName, ".idx");

	poDS->eAccess = GA_Update;
    }

    catch (CPLString e) {
	CPLError(CE_Failure, CPLE_OpenFailed, e.c_str());
	delete poDS;
	poDS = NULL;
    }

    poDS->current = poDS->full;

    poDS->SetDescription(pszName);

    // Build a MRF XML and initialize from it, this creates the bands
    CPLXMLNode *config = poDS->BuildConfig();
    poDS->Initialize(config);
    CPLDestroyXMLNode(config);

    // If not set by the band, get a pageSizeBytes buffer
    if (poDS->GetPBufferSize() == 0)
	poDS->SetPBuffer(poDS->current.pageSizeBytes);

    // Tell PAM what our real file name is, to help it find the aux.xml
    poDS->SetPhysicalFilename(pszName);

    return poDS;
}

void GDALMRFDataset::Crystalize()

{
    if (bCrystalized || eAccess != GA_Update)
	return;

    // No need to write to disk if there is no filename.  This is a 
    // memory only dataset.
    if (strlen(GetDescription()) == 0
	|| EQUALN(GetDescription(), "<MRF_META>", 10))
	return;

    CPLXMLNode *config = BuildConfig();
    WriteConfig(config);
    if (!IdxFP() || !DataFP())
	throw CPLString().Printf("MRF: Can't create file %s", strerror(errno));
    CPLDestroyXMLNode(config);

    bCrystalized = TRUE;
}

// Copy the first index at the end of the file and bump the version count
CPLErr GDALMRFDataset::AddVersion()
{
    // Hides the dataset variables with the same name
    VSILFILE *ifp = IdxFP();

    void *tbuff = CPLMalloc(idxSize);
    VSIFSeekL(ifp, 0, SEEK_SET);
    VSIFReadL(tbuff, 1, idxSize, ifp);
    verCount++; // The one we write
    VSIFSeekL(ifp, idxSize * verCount, SEEK_SET); // At the end, this can mess things up royally
    VSIFWriteL(tbuff, 1, idxSize, ifp);
    CPLFree(tbuff);
    return CE_None;
}

//
// Write a tile at the end of the data file
// If buff and size are zero, it is equivalent to erasing the tile
// If only size is zero, it is a special empty tile, 
// when used for caching, offset should be 1
//
// To make it multi-processor safe, open the file in append mode
// and verify after write
//
CPLErr GDALMRFDataset::WriteTile(void *buff, GUIntBig infooffset, GUIntBig size)
{
    CPLErr ret = CE_None;
    ILIdx tinfo = { 0, 0 };

    // These hide the dataset variables with the same name
    VSILFILE *dfp = DataFP();
    VSILFILE *ifp = IdxFP();

    // Pointer to verfiy buffer, if it doesn't exist everything worked fine
    void *tbuff = 0;

    if (ifp == NULL || dfp == NULL)
	return CE_Failure;

    if (hasVersions) {
	int new_version = false; // Assume no need to build new version
	int new_tile = false;

	// Read the current tile info
	VSIFSeekL(ifp, infooffset, SEEK_SET);
	VSIFReadL(&tinfo, 1, sizeof(ILIdx), ifp);

	if (verCount != 0) { // We have at least two versions before we test buffers
	    ILIdx prevtinfo = { 0, 0 };

	    // Read the previous one
	    VSIFSeekL(ifp, infooffset + verCount * idxSize, SEEK_SET);
	    VSIFReadL(&prevtinfo, 1, sizeof(ILIdx), ifp);

	    // current and previous tiles are different, might create version
	    if (tinfo.size != prevtinfo.size || tinfo.offset != prevtinfo.offset)
		new_version = true;
	}
	else
	    new_version = true; // No previous, might create version

	// tinfo contains the current info or 0,0
	if (tinfo.size == net64(size)) { // Might be the same, read and compare
	    if (size != 0) {
		tbuff = CPLMalloc(size);
		// Use the temporary buffer, we can't have a versioned cache !!
		VSIFSeekL(dfp, infooffset, SEEK_SET);
		VSIFReadL(tbuff, 1, size, dfp);
		// Need to write it if not the same
		new_tile = (0 != memcmp(buff, tbuff, size));
		CPLFree(tbuff);
	    }
	    else {
		// Writing a null tile on top of a null tile, does it count?
		if (tinfo.offset != net64(GUIntBig(buff)))
		    new_tile = true;
	    }
	}
	else {
	    new_tile = true; // Need to write it because it is different
	    if (verCount == 0 && tinfo.size == 0)
		new_version = false; // Don't create a version if current is empty and there is no previous
	}

	if (!new_tile)
	    return CE_None; // No reason to write

	// Do we need to start a new version before writing the tile?
	if (new_version)
	    AddVersion();
    }

    // Convert to net format
    tinfo.size = net64(size);

    if (size) do {
	// Theese statements are the critical MP section
	VSIFSeekL(dfp, 0, SEEK_END);
	GUIntBig offset = VSIFTellL(dfp);
	if (size != VSIFWriteL(buff, 1, size, dfp))
	    ret = CE_Failure;

	tinfo.offset = net64(offset);
	//
	// If caching, check that we can read it back, otherwise we're done
	// This makes the caching MRF MP safe, without using locks
	//
	if (GetSrcDS() != NULL) {
	    // Assume we failed
	    // Allocate the temp buffer if we haven't done so already
	    // This marks the check as failed
	    if (!tbuff)
		tbuff = CPLMalloc(size);
	    VSIFSeekL(dfp, offset, SEEK_SET);
	    VSIFReadL(tbuff, 1, size, dfp);
	    // If memcmp returns zero, verify passed
	    if (!memcmp(buff, tbuff, size)) {
		CPLFree(tbuff);
		tbuff = NULL; // Triggers the loop termination
	    }
	    // Otherwise the tbuf stays allocated and try to write again
	    // This works best if the file is opened in append mode
	}
    } while (tbuff);

    // At this point, the data is in the datafile

    // Special case
    // Any non-zero will do, use 1 to only consume one bit
    if (0 != buff && 0 == size)
	tinfo.offset = net64(GUIntBig(buff));

    VSIFSeekL(ifp, infooffset, SEEK_SET);
    if (sizeof(tinfo) != VSIFWriteL(&tinfo, 1, sizeof(tinfo), ifp))
	ret = CE_Failure;

    // Removed because the data might not be in the file yet, can't flush here
    //
    // Flush index if this is a caching MRF
    //    if (GetSrcDS() != NULL)
    // VSIFFlushL(ifp);

    return ret;
}

CPLErr GDALMRFDataset::SetGeoTransform(double *gt)

{
    if (GetAccess() == GA_Update)
    {
	memcpy(GeoTransform, gt, 6 * sizeof(double));
	bGeoTransformValid = TRUE;
	return CE_None;
    }
    CPLError(CE_Failure, CPLE_NotSupported,
	"SetGeoTransform called on read only file");
    return CE_Failure;
}

/*
*  Should return 0,1,0,0,0,1 even if it was not set
*/
CPLErr GDALMRFDataset::GetGeoTransform(double *gt)
{
    memcpy(gt, GeoTransform, 6 * sizeof(double));
    if (!bGeoTransformValid) return CE_Failure;
    return CE_None;
}

/**
*\brief Read a tile index
*
* It handles the non-existent index case, for no compression
* The bias is non-zero only when the cloned index is read
*/

CPLErr GDALMRFDataset::ReadTileIdx(ILIdx &tinfo, const ILSize &pos, const ILImage &img, const GIntBig bias)

{
    VSILFILE *ifp = IdxFP();
    GIntBig offset = bias + IdxOffset(pos, img);
    if (ifp == NULL && img.comp == IL_NONE) {
	tinfo.size = current.pageSizeBytes;
	tinfo.offset = offset * tinfo.size;
	return CE_None;
    }

    if (ifp == NULL) {
	CPLError(CE_Failure, CPLE_FileIO, "Can't open index file");
	return CE_Failure;
    }

    VSIFSeekL(ifp, offset, SEEK_SET);
    if (1 != VSIFReadL(&tinfo, sizeof(ILIdx), 1, ifp))
	return CE_Failure;
    // Convert them to native form
    tinfo.offset = net64(tinfo.offset);
    tinfo.size = net64(tinfo.size);

    if (0 == bias || 0 != tinfo.size || 0 != tinfo.offset)
	return CE_None;

    // zero size and zero offset in sourced index means that this portion is un-initialized

    // Should be cloned and the offset within the cloned index
    offset -= bias;
    assert(offset < bias);
    assert(clonedSource);


    // Read this block from the remote index, prepare it and store it in the right place
    // The block size in bytes, should be a multiple of 16, to have full index entries
    const int CPYSZ = 32768;
    // Adjust offset to the start of the block
    offset = (offset / CPYSZ) * CPYSZ;
    GIntBig size = MIN(size_t(CPYSZ), size_t(bias - offset));
    size /= sizeof(ILIdx); // In records
    vector<ILIdx> buf(size);
    ILIdx *buffer = &buf[0]; // Buffer to copy the source to the clone index


    // Fetch the data from the cloned index
    GDALMRFDataset *pSrc = static_cast<GDALMRFDataset *>(GetSrcDS());

    VSILFILE *srcidx = pSrc->IdxFP();
    if (!srcidx) {
	CPLError(CE_Failure, CPLE_FileIO, "Can't open cloned source index");
	return CE_Failure; // Source reported the error
    }

    VSIFSeekL(srcidx, offset, SEEK_SET);
    size = VSIFReadL(buffer, sizeof(ILIdx), size, srcidx);
    if (size != buf.size()) {
	CPLError(CE_Failure, CPLE_FileIO, "Can't read cloned source index");
	return CE_Failure; // Source reported the error
    }

    // Mark the empty records as checked, by making the offset non-zero
    for (vector<ILIdx>::iterator it = buf.begin(); it != buf.end(); it++) {
	if (it->offset == 0 && it->size == 0)
	    it->offset = net64(1);
    }

    // Write it in the right place in the local index file
    VSIFSeekL(ifp, bias + offset, SEEK_SET);
    size = VSIFWriteL(&buf[0], sizeof(ILIdx), size, ifp);
    if (size != buf.size()) {
	CPLError(CE_Failure, CPLE_FileIO, "Can't write to cloning MRF index");
	return CE_Failure; // Source reported the error
    }

    // Cloned index updated, restart this function, it will work now
    return ReadTileIdx(tinfo, pos, img, bias);
}
