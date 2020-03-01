#include <iostream>
#include<fstream>
#include<string>
#include <cstring>
#include "FJFX.h"
#include "FRFXLL.h"

using namespace std;

#define CBEFF (0x00330502)
#define Check(x, err) { if ((x) < FRFXLL_OK) return err; }
#define CheckFx(x)    Check(x, FJFX_FAIL_EXTRACTION_UNSPEC);

struct DpHandle {
    FRFXLL_HANDLE h;

    explicit DpHandle(FRFXLL_HANDLE _h = NULL) : h(_h) {}

    ~DpHandle() {
        if (h)
            Close();
    }

    FRFXLL_RESULT Close() {
        FRFXLL_RESULT rc = FRFXLL_OK;
        if (h) {
            rc = FRFXLLCloseHandle(&h);
        }
        h = NULL;
        return rc;
    }

    operator FRFXLL_HANDLE() const  { return h; }
    FRFXLL_HANDLE* operator &()     { return &h; }
};



// Minutiae Extraction interface
int fjfx_create_fmd_from_raw(
        const unsigned char* raw_image,
        const unsigned short dpi,
        const unsigned short height,
        const unsigned short width,
        const unsigned int output_format,
        unsigned char* fmd,
        unsigned int* size_of_fmd_ptr
) {
    if (fmd == NULL)       return FJFX_FAIL_EXTRACTION_UNSPEC;
    if (raw_image == NULL) return FJFX_FAIL_EXTRACTION_BAD_IMP;
    if (width > 2000 || height > 2000)                         return FJFX_FAIL_IMAGE_SIZE_NOT_SUP;
    if (dpi < 300 || dpi > 1024)                               return FJFX_FAIL_IMAGE_SIZE_NOT_SUP;
//   if (width * 500 < 150 * dpi  || width * 500 > 812 * dpi)   return FJFX_FAIL_IMAGE_SIZE_NOT_SUP; // in range 0.3..1.62 in
//   if (height * 500 < 150 * dpi || height * 500 > 1000 * dpi) return FJFX_FAIL_IMAGE_SIZE_NOT_SUP; // in range 0.3..2.0 in
    size_t size = size_of_fmd_ptr ? *size_of_fmd_ptr : FJFX_FMD_BUFFER_SIZE;
//   if (size < FJFX_FMD_BUFFER_SIZE)                           return FJFX_FAIL_OUTPUT_BUFFER_IS_TOO_SMALL;

    FRFXLL_DATA_TYPE dt = 0;
    switch (output_format) {
        case FJFX_FMD_ANSI_378_2004:    dt = FRFXLL_DT_ANSI_FEATURE_SET; break;
        case FJFX_FMD_ISO_19794_2_2005: dt = FRFXLL_DT_ISO_FEATURE_SET; break;
        default:
            return FJFX_FAIL_INVALID_OUTPUT_FORMAT;
    }

    DpHandle hContext, hFtrSet;
    CheckFx( FRFXLLCreateLibraryContext(&hContext) );
    switch ( FRFXLLCreateFeatureSetFromRaw(hContext, raw_image, width * height, width, height, dpi, FRFXLL_FEX_ENABLE_ENHANCEMENT, &hFtrSet ) ) {
        case FRFXLL_OK:
            break;
        case FRFXLL_ERR_FB_TOO_SMALL_AREA:
            return FJFX_FAIL_EXTRACTION_BAD_IMP;
        default:
            return FJFX_FAIL_EXTRACTION_UNSPEC;
    }
    const unsigned short dpcm = (dpi * 100 + 50) / 254;
    const unsigned char finger_quality  = 60;  // Equivalent to NFIQ value 3
    const unsigned char finger_position = 0;   // Unknown finger
    const unsigned char impression_type = 0;   // Live-scan plain
    FRFXLL_OUTPUT_PARAM_ISO_ANSI param = {sizeof(FRFXLL_OUTPUT_PARAM_ISO_ANSI), CBEFF, finger_position, 0, dpcm, dpcm, width, height, 0, finger_quality, impression_type};
    unsigned char * tmpl = reinterpret_cast<unsigned char *>(fmd);
    CheckFx( FRFXLLExport(hFtrSet, dt, &param, tmpl, &size) );
    if (size_of_fmd_ptr) *size_of_fmd_ptr = (unsigned int)size;
    return FJFX_SUCCESS;
}

const unsigned char* convertToRaw(const char* fileName)
{
    /* declaration */
    unsigned char* memblock = NULL;
    streampos size;

    /* read bytes from bmp file */
    ifstream bmpFile(fileName, ios::in | ios::binary | ios::ate);
    if (bmpFile.good())
    {
        size = bmpFile.tellg();
        memblock = new unsigned char[size];
        bmpFile.seekg(0, ios::beg);
        bmpFile.read((char*)memblock, size);
        bmpFile.close();
        cout << "size is: " << size << " bytes.\n";
    }


    /* create minex from bmp */
    const unsigned char* imageRaw = &memblock[0];
    // unsigned int imageSize = (unsigned int)size;
    return imageRaw;
}


int main()
{
//	/* declaration */
//    unsigned char* memblock = NULL;
//	streampos size;
//
//	/* read bytes from bmp file */
//	ifstream bmpFile("/home/mehmet/c_projects/fp_demo/engine_demo/bin/b.bmp", ios::in | ios::binary | ios::ate);
//	if (bmpFile.good())
//	{
//		size = bmpFile.tellg();
//		memblock = new unsigned char[size];
//		bmpFile.seekg(0, ios::beg);
//		bmpFile.read((char*)memblock, size);
//		bmpFile.close();
//		cout << "size is: " << size << " bytes.\n";
//	}
//
//
//	/* create minex from bmp */
//	const unsigned char* imageRaw = &memblock[0];
//	// unsigned int imageSize = (unsigned int)size;

    const char* inFile = "/home/mehmet/c_projects/fp_demo/engine_demo/bin/b.bmp";
    const unsigned char* imageRaw = convertToRaw(inFile);


    /* get max size for the feature template */
    unsigned int nFeaturesSize = FJFX_FMD_BUFFER_SIZE;
    unsigned char* pFeatures = (unsigned char*)malloc(nFeaturesSize);
    int ret = fjfx_create_fmd_from_raw(imageRaw, 500, 480, 320, FJFX_FMD_ANSI_378_2004, pFeatures, &nFeaturesSize);
    cout << ret << endl;
    if (ret == FRFXLL_OK)
    {
        cout << "Success" << endl;
    }







    int sc = memcmp(pFeatures, pFeatures, nFeaturesSize);
    cout << "match score: ";
    cout << sc << endl;

//	delete[] memblock;

//	try {
//		/* write minex to file */
//		// ofstream wf("/home/mehmet/c_projects/fp_demo/engine_demo/bin/fp.minex", ios::out | ios::binary);
//		ofstream wf("fp.minex", ios::out | ios::binary);
//		if (wf.good())
//		{
//			wf.write((char*)pFeatures, nFeaturesSize);
//		}
//		wf.close();
//	}
//	catch (exception x)
//	{
//		cout << "Exception" << endl;
//	}

    cout << "Done..." << endl;
    return 0;
}

int compare(unsigned char *a, unsigned char *b, int size) {
    while(size-- > 0) {
        if ( *a != *b ) { return (*a < *b ) ? -1 : 1; }
        a++; b++;
    }
    return 0;
}
