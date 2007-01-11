// -*- mode:C++; tab-width:4; c-basic-offset:4; indent-tabs-mode:nil -*-

/*
 * Copyright (C) 2006 Paul Fitzpatrick, Giorgio Metta, Lorenzo Natale
 * CopyPolicy: Released under the terms of the GNU GPL v2.0.
 *
 */

///
/// $Id: DragonflyDeviceDriver.cpp,v 1.24 2007-01-11 09:30:56 alex_bernardino Exp $
///
///

#include <yarp/DragonflyDeviceDriver.h>
#include <yarp/dev/FrameGrabberInterfaces.h>
#include <yarp/os/Thread.h>
#include <yarp/os/Time.h>
#include <ace/Log_msg.h>

using namespace yarp::dev;
using namespace yarp::os;

//=============================================================================
// FlyCam Includes
//=============================================================================
#include "../dd_orig/include/pgrflycapture.h"

const int _sizeX=640;
const int _sizeY=480;
const int _halfX=_sizeX/2;
const int _halfY=_sizeY/2;

//forward function declaration
bool Bayer2RGB( const unsigned char* bayer0, int bayer_step, unsigned char *dst0, int dst_step, int width, int height, int blue, int start_with_green, int rgb );


//
class DragonflyResources
{
public:
	DragonflyResources (void) 
	{
		// Variables initialization
		sizeX = _sizeX;
		sizeY = _sizeY;
		maxCams = 0;
		bufIndex = 0;
		_canPost = false;
		imageConverted.pData = NULL;
		_acqStarted = false;
		_validContext = false;
		imageSubSampled = NULL;
        imageFullSize = NULL;
	}

	~DragonflyResources () 
	{ 
		_uninitialize (); // To be sure - must protected against double calling
	}
	
	int sizeX;
	int sizeY;
	int maxCams;
	int bufIndex;
	bool _canPost;
	bool _acqStarted;
	bool _validContext;
	bool fleaCR;

	FlyCaptureContext context;
	FlyCaptureImage imageConverted;
	FlyCaptureImage lastBuffer;
	unsigned char *imageSubSampled;
	unsigned char *imageFullSize; //used by the reconstructGenearal method

    //color reconstruction methods
	bool reconstructColor(const unsigned char *src, unsigned char *dst);
	bool recColorFSNN(const unsigned char *src, unsigned char *dst);
    bool recColorFSBilinear(const unsigned char *src, unsigned char *dst);
    bool recColorHSBilinear(const unsigned char *src, unsigned char *dst);
	bool recColorGeneral(const unsigned char *src, unsigned char *dst);
    void subSampling(const unsigned char *src, unsigned char *dst);
    //
    
    bool _initialize (const DragonflyOpenParameters& params);
	bool _uninitialize (void);
	
	bool _setBrightness (int value, bool bDefault=false);
	bool _setExposure (int value, bool bDefault=false);
	bool _setWhiteBalance (int redValue, int blueValue, bool bDefault=true);
	bool _setShutter (int value, bool bDefault=false);
	bool _setGain (int value, bool bDefault=false);

	double _getShutter() const;
	double _getExposure() const;
	double _getBrightness() const;
	double _getGain() const;
	bool _getWhiteBalance (int &redValue, int &blueValue) const;

private:
	void _prepareBuffers (void);
	void _destroyBuffers (void);
};




bool DragonflyResources::reconstructColor(const unsigned char *src, unsigned char *dst)
{

	//for flea cameras - not optimized for subsampling
	if(fleaCR)
	{
		if ((sizeX == _sizeX) && (sizeY == _sizeY) )
        {
            // full size reconstruction
            Bayer2RGB( src, sizeX*3, dst, sizeX*3, sizeX, sizeY, -1, 1, 1 );
            return true;
        }
		else
        {
            Bayer2RGB( src, _sizeX*3, imageFullSize, _sizeX*3, _sizeX, _sizeY, -1, 1, 1 );
			subSampling(imageFullSize, dst);
            return true;
        }
	}

	//for dragonfly cameras
	if ((sizeX == _sizeX) && (sizeY == _sizeY) )
        {
            // full size reconstruction
            recColorFSBilinear(src, dst);
            return true;
        }
    if ((sizeX == _halfX) && (sizeY == _halfY) )
        {
            recColorHSBilinear(src,dst);
        }
	else
        {
            recColorGeneral(src,dst);
            return true;
        }
    return true;
}

void reportCameraInfo( const FlyCaptureInfoEx* pinfo )
{
    fprintf(stderr, "Serial number: %d\n", pinfo->SerialNumber );
    fprintf(stderr, "Camera model: %s\n", pinfo->pszModelName );
    fprintf(stderr, "Camera vendor: %s\n", pinfo->pszVendorName );
    fprintf(stderr, "Sensor: %s\n", pinfo->pszSensorInfo );
    fprintf(stderr, "DCAM compliance: %1.2f\n", (float)pinfo->iDCAMVer / 100.0 );
    fprintf(stderr, "Bus position: (%d,%d).\n", pinfo->iBusNum, pinfo->iNodeNum );
}

///
///
/// full initialize and startup of the grabber.
inline bool DragonflyResources::_initialize (const DragonflyOpenParameters& params)
{
	FlyCaptureError   error = FLYCAPTURE_OK;

	// LATER: add a camera_init function

	sizeX = params._size_x;
	sizeY = params._size_y;
	fleaCR = params._fleacr;

	// Create the context.
	if (!_validContext)
        {
            error = flycaptureCreateContext( &context );
            if (error != FLYCAPTURE_OK)
                return false;
            _validContext = true;
            // Initialize the camera.
            error = flycaptureInitialize( context, params._unit_number );
            if (error != FLYCAPTURE_OK)
                return false;
        }
	
	FlyCaptureInfoEx info;
	flycaptureGetCameraInfo(context, &info);
	reportCameraInfo( &info );

	if(params._brightness >= 0) {
		_setBrightness(params._brightness);
		printf("Brightness %d\n", params._brightness); }

	if(params._exposure >= 0) {
		_setExposure(params._exposure);
		printf("Exposure %d\n", params._exposure); }

	if( (params._whiteR >= 0) && (params._whiteB >= 0)) {
		_setWhiteBalance(params._whiteR, params._whiteB); 
		printf("White balance %d %d\n", params._whiteR, params._whiteB); }

	if(params._shutter >= 0) {
		_setShutter(params._shutter);	// x * 0.0625 = 20 mSec = 50 Hz
		printf("Shutter %d\n", params._shutter); }

	if(params._gain >= 0) {
		_setGain(params._gain);	 	// x * -0.0224 = -11.2dB
		printf("Gain %d\n", params._gain); }
	
	// Set color reconstruction method
	error = flycaptureSetColorProcessingMethod(context, FLYCAPTURE_NEAREST_NEIGHBOR_FAST); // Should be an Option
	if (error != FLYCAPTURE_OK)
		return false;

	// Set Acquisition Timeout
	error = flycaptureSetGrabTimeoutEx(context, 200);
	if (error != FLYCAPTURE_OK)
		return false;
	// Buffers
	_prepareBuffers ();

	// Start Acquisition
	if (!_acqStarted)
        {
            error = flycaptureStart(	context, 
                                        FLYCAPTURE_VIDEOMODE_640x480Y8,
                                        FLYCAPTURE_FRAMERATE_30);  
            if (error != FLYCAPTURE_OK)
                return false;
            _acqStarted = true;
        }
	
	return true;
}

inline bool DragonflyResources::_uninitialize (void)
{
	FlyCaptureError   error = FLYCAPTURE_OK;

	// Stop Acquisition
	if (_acqStarted) {
        error = flycaptureStop(context);
        if (error != FLYCAPTURE_OK)
            return false;
        _acqStarted = false;

		// Destroy the context only if properly started
		// otherwise this might disturb another instance already running
		if (_validContext) {
            error = flycaptureDestroyContext( context );
            if (error != FLYCAPTURE_OK)
                return false;
            _validContext = false;
        }
	}

	// Deallocate buffers
	_destroyBuffers();
	
	return true;
}



inline double DragonflyResources::_getShutter() const
{
    long tmpA;

    FlyCaptureError   error = FLYCAPTURE_OK;

	error = flycaptureGetCameraProperty(context, FLYCAPTURE_SHUTTER, &tmpA, 0, false);
	if (error == FLYCAPTURE_OK)
		return (double) tmpA;
	else 
		return -1;
}



inline double DragonflyResources::_getBrightness() const
{
    long tmpA;

    FlyCaptureError   error = FLYCAPTURE_OK;
	error = flycaptureGetCameraProperty(context, FLYCAPTURE_BRIGHTNESS, &tmpA, 0, false);
	if (error == FLYCAPTURE_OK)
		return (double) tmpA;
	else 
		return -1;
}



inline double DragonflyResources::_getGain() const
{
    long tmpA;

    FlyCaptureError   error = FLYCAPTURE_OK;

	error = flycaptureGetCameraProperty(context, FLYCAPTURE_GAIN, &tmpA, 0, false);
	if (error == FLYCAPTURE_OK)
		return (double) tmpA;
	else 
		return -1;
}


inline bool DragonflyResources::_getWhiteBalance (int &redValue, int &blueValue) const
{
	fprintf(stderr, "Not implemented yet, assuming you care...");
	return false;
}

///
///
inline void DragonflyResources::_prepareBuffers(void)
{
	if (imageConverted.pData == NULL)
		imageConverted.pData = new unsigned char[ _sizeX * _sizeY * 3 ];
	imageConverted.pixelFormat = FLYCAPTURE_BGR;

	if (imageSubSampled == NULL)
		imageSubSampled = new unsigned char[ sizeX * sizeY * 3 ];

    if (imageFullSize == NULL)
        imageFullSize = new unsigned char [_sizeX*_sizeY*3];

	memset(imageSubSampled, 0x0, (sizeX * sizeY * 3));
    memset(imageFullSize, 0x0, (_sizeX*_sizeY*3));
}

inline void DragonflyResources::_destroyBuffers(void)
{
	if (imageConverted.pData != NULL)
		delete [] imageConverted.pData;
	imageConverted.pData = NULL;

    if (imageFullSize != NULL)
        delete [] imageFullSize;
    imageFullSize=NULL;

	if (imageSubSampled != NULL)
		delete [] imageSubSampled;
	imageSubSampled = NULL;
}

inline bool DragonflyResources::_setBrightness (int value, bool bAuto)
{
	FlyCaptureError   error = FLYCAPTURE_OK;

	error = flycaptureSetCameraPropertyEx(context, FLYCAPTURE_BRIGHTNESS, false, false, bAuto, value, 0);

	if (error == FLYCAPTURE_OK)
		return true;
	else 
		return false;
}

inline bool DragonflyResources::_setExposure (int value, bool bAuto)
{
	FlyCaptureError   error = FLYCAPTURE_OK;
    //	error = flycaptureSetCameraProperty(context, FLYCAPTURE_AUTO_EXPOSURE, value, 0, bAuto);
	error = flycaptureSetCameraPropertyEx(context, FLYCAPTURE_AUTO_EXPOSURE, false, false, bAuto, value, 0);

	if (error == FLYCAPTURE_OK)
		return true;
	else 
		return false;
}

inline bool DragonflyResources::_setWhiteBalance (int redValue, int blueValue, bool bAuto)
{
	FlyCaptureError   error = FLYCAPTURE_OK;

	fprintf(stderr, "Setting White: %d %d\n", redValue, blueValue);
	// error = flycaptureSetCameraProperty(context, FLYCAPTURE_WHITE_BALANCE, redValue, blueValue, bAuto);
	error = flycaptureSetCameraPropertyEx(context, FLYCAPTURE_WHITE_BALANCE, false, true, false, redValue, blueValue);

	if (error==FLYCAPTURE_OK)
	{
		fprintf(stderr, "OK!!");
	}
	else
	{
		fprintf(stderr, "NOT ok\n");
	}

	bool push;
	bool on;
	bool bb;
	
	error = flycaptureGetCameraPropertyEx(context, FLYCAPTURE_WHITE_BALANCE, &push, &on, &bb, &redValue, &blueValue);

	fprintf(stderr, "Got back: %d %d\n", redValue, blueValue);

	if (error == FLYCAPTURE_OK)
		return true;
	else 
		return false;
}

inline bool DragonflyResources::_setShutter (int value, bool bAuto)
{
	FlyCaptureError   error = FLYCAPTURE_OK;

	error = flycaptureSetCameraPropertyEx(context, FLYCAPTURE_SHUTTER, false, false, bAuto, value, 0);
	if (error == FLYCAPTURE_OK)
		return true;
	else 
		return false;
}

inline bool DragonflyResources::_setGain (int value, bool bAuto)
{
	FlyCaptureError   error = FLYCAPTURE_OK;
    //	error = flycaptureSetCameraProperty(context, FLYCAPTURE_GAIN, value, 0, bAuto);
	error = flycaptureSetCameraPropertyEx(context, FLYCAPTURE_GAIN, false, false, bAuto, value, 0);
	if (error == FLYCAPTURE_OK)
		return true;
	else 
		return false;
}

inline DragonflyResources& RES(void *res) { return *(DragonflyResources *)res; }

///
///
DragonflyDeviceDriver::DragonflyDeviceDriver(void)
{
    system_resources = 0;
	system_resources = (void *) new DragonflyResources;
	ACE_ASSERT (system_resources != 0);
}

DragonflyDeviceDriver::~DragonflyDeviceDriver()
{
	if (system_resources != 0)
		delete (DragonflyResources *)system_resources;
	system_resources = 0;
}

///
///
bool DragonflyDeviceDriver::open (const DragonflyOpenParameters &par)
{
	DragonflyResources& d = RES(system_resources);
	int ret = d._initialize (par);
    return ret;
}

bool DragonflyDeviceDriver::close (void)
{
	bool ret=true;
	DragonflyResources& d = RES(system_resources);
	ret = d._uninitialize ();
	return ret;
}

bool DragonflyDeviceDriver::getRgbBuffer(unsigned char *buffer)
{
    DragonflyResources& d = RES(system_resources);

    FlyCaptureError error = FLYCAPTURE_OK;

	FlyCaptureImage *pSrc;
	
	pSrc = &(d.lastBuffer);
	
	error = flycaptureGrabImage2(d.context, pSrc);
	if (error!=FLYCAPTURE_OK)
		return false;
	
	d.reconstructColor(pSrc->pData, buffer);

	return true;
}

bool DragonflyDeviceDriver::getImage(yarp::sig::ImageOf<yarp::sig::PixelRgb>& image)
{
    DragonflyResources& d = RES(system_resources);

    FlyCaptureError error = FLYCAPTURE_OK;

	FlyCaptureImage *pSrc;
	
	pSrc = &(d.lastBuffer);
	
	error = flycaptureGrabImage2(d.context, pSrc);
	if (error!=FLYCAPTURE_OK)
		return false;
	
	// hmm, we should make sure image has some space in it first, right?
	image.resize(d.sizeX,d.sizeY);

	d.reconstructColor(pSrc->pData, (unsigned char *)image.getRawImage());

	return true;
}

bool DragonflyDeviceDriver::getRawBuffer(unsigned char *buffer)
{
    DragonflyResources& d = RES(system_resources);

    FlyCaptureError error = FLYCAPTURE_OK;

	FlyCaptureImage *pSrc;
	
	pSrc = &(d.lastBuffer);
	
	error = flycaptureGrabImage2(d.context, pSrc);
	if (error!=FLYCAPTURE_OK)
		return false;
	
	memcpy(buffer, pSrc->pData, _sizeX*_sizeY);
	

	return true;
}

int DragonflyDeviceDriver::getRawBufferSize()
{
    DragonflyResources& d = RES(system_resources);

    return _sizeX*_sizeY;
}

int DragonflyDeviceDriver::width () const
{
	return RES(system_resources).sizeX;
}

int DragonflyDeviceDriver::height () const
{
	return RES(system_resources).sizeY;
}

bool DragonflyDeviceDriver::setBrightness(double value)
{
    DragonflyResources& d = RES(system_resources);
	return d._setBrightness((int)(value));

}

bool DragonflyDeviceDriver::setShutter(double value)
{
    DragonflyResources& d = RES(system_resources);


	return d._setBrightness((int)(value));
}

bool DragonflyDeviceDriver::setGain(double value)
{
    DragonflyResources& d = RES(system_resources);


	return d._setGain(value);

}

double DragonflyDeviceDriver::getShutter() const
{
    DragonflyResources& d = RES(system_resources);



	return d._getGain();
}


bool DragonflyDeviceDriver::setWhiteBalance(double red, double blue)

{

    DragonflyResources& d = RES(system_resources);

	return d._setWhiteBalance(red, blue);

}


double DragonflyDeviceDriver::getBrightness() const
{
    DragonflyResources& d = RES(system_resources);

	return d._getBrightness();
}

double DragonflyDeviceDriver::getGain() const
{
    DragonflyResources& d = RES(system_resources);
	return d._getGain();

}

void DragonflyDeviceDriver::recColorFSBilinear(const unsigned char *src, unsigned char *out)
{
    RES(system_resources).recColorFSBilinear(src, out);
}

void DragonflyDeviceDriver::recColorFSNN(const unsigned char *src, unsigned char *out)
{
    RES(system_resources).recColorFSNN(src, out);
}

void DragonflyDeviceDriver::recColorHSBilinear(const unsigned char *src, unsigned char *out)
{
    RES(system_resources).recColorHSBilinear(src, out);
}

////// Reconstruct color methods
// reconstruct color in a full size image, bilinear interpolation
// Assumes pattern: RGRG...RG
//                  GBGB...GB etc..
bool DragonflyResources::recColorFSBilinear(const unsigned char *src, unsigned char *dest)
{
	int tmpB=0;
	int tmpG=0;
	int tmpR=0;

	int rr=0;
	int cc=0;

	unsigned char *tmpSrc=const_cast<unsigned char *>(src);

	///////////// prima riga
	// primo pixel
	tmpG=*(tmpSrc+_sizeX);
	tmpG+=*(tmpSrc+1);

	*dest++=*tmpSrc;
	*dest++=(unsigned char) tmpG/2;
	*dest++=*(tmpSrc+_sizeX+1);
	tmpSrc++;

    // prima riga
	for(cc=1;cc<(_sizeX/2); cc++)
        {
            // first pixel
            tmpR=*(tmpSrc-1);  
            tmpR+=*(tmpSrc+1); 
			
            // x interpolation
            tmpB=*(tmpSrc+_sizeX);
						
            *dest++=(unsigned char)(tmpR/2);
            *dest++=*(tmpSrc);
            *dest++=(unsigned char)(tmpB);

            tmpSrc++;

            // second pixel
            tmpB=*(tmpSrc+_sizeX+1); 
            tmpB+=*(tmpSrc+_sizeX-1); 
			
            tmpG=*(tmpSrc-1);  
            tmpG+=*(tmpSrc+1); 
            tmpG+=*(tmpSrc+_sizeX); 

            *dest++=*(tmpSrc);
            *dest++=(unsigned char)(tmpG/3);
            *dest++=(unsigned char)(tmpB/2);
            tmpSrc++;
        }

	// last columns, ends with g
	*dest++=*(tmpSrc-1);
	*dest++=*(tmpSrc);
	*dest++=*(tmpSrc+_sizeX);

	tmpSrc++;

	for (rr=1; rr<(_sizeY/2); rr++)
        {
            ////////////////// gb row
            // prima colonna
            tmpG=*(tmpSrc);
            tmpB=*(tmpSrc+1);
            tmpR=*(tmpSrc-_sizeX);
            tmpR+=*(tmpSrc+_sizeX);

            *dest++=(unsigned char)(tmpR/2);
            *dest++=tmpG;
            *dest++=tmpB;
		
            tmpSrc++;

            for(cc=1; cc<(_sizeX/2); cc++)
                {
                    // second pixel
                    tmpR= *(tmpSrc-_sizeX-1);  
                    tmpR+= *(tmpSrc-_sizeX+1); 
                    tmpR+= *(tmpSrc+_sizeX-1); 
                    tmpR+= *(tmpSrc+_sizeX+1); 
			
                    // + interpolation
                    tmpG=*(tmpSrc-_sizeX);		
                    tmpG+=*(tmpSrc-1);	
                    tmpG+=*(tmpSrc+1);		
                    tmpG+=*(tmpSrc+_sizeX);		

                    *dest++=(unsigned char)(tmpR/4);
                    *dest++=(unsigned char)(tmpG/4);
                    *dest++=*tmpSrc;

                    tmpSrc++;

                    // first pixel
                    tmpR=*(tmpSrc-_sizeX);	
                    tmpR+=*(tmpSrc+_sizeX);
			
                    // x interpolation
                    tmpB=*(tmpSrc-1);
                    tmpB+=*(tmpSrc+1);
						
                    *dest++=(unsigned char)(tmpR/2);
                    *dest++=*tmpSrc;
                    *dest++=(unsigned char)(tmpB/2);
			
                    tmpSrc++;
                }
            //last col, ends with b
            *dest++=*(tmpSrc+_sizeX-1);
            *dest++=*(tmpSrc+_sizeX);
            *dest++=*tmpSrc;

            tmpSrc++;
			
            ////////////////// gb row
            // prima colonna
            tmpG=*(tmpSrc-_sizeX);	
            tmpG+=*(tmpSrc+_sizeX);	
            tmpG+=*(tmpSrc+1);	

            tmpB=*(tmpSrc-_sizeX+1);
            tmpB+=*(tmpSrc+_sizeX+1);
            tmpR=*tmpSrc;
		
            *dest++=(unsigned char)(tmpR);
            *dest++=(unsigned char)(tmpG/3);
            *dest++=(unsigned char)(tmpB/2);

            tmpSrc++;

            // altre colonne
            for(cc=1; cc<(_sizeX/2); cc++)
                {
                    // second pixel
                    tmpB=*(tmpSrc-_sizeX);	
                    tmpB+=*(tmpSrc+_sizeX);  
			
                    // x interpolation
                    tmpR=*(tmpSrc-1);	
                    tmpR+=*(tmpSrc+1);	
						
                    *dest++=(unsigned char)(tmpR/2);
                    *dest++=*tmpSrc;
                    *dest++=(unsigned char)(tmpB/2);

                    tmpSrc++;

                    // first pixel, x interpolation
                    tmpB=*(tmpSrc-_sizeX-1);		
                    tmpB+=*(tmpSrc-_sizeX+1);	
                    tmpB+=*(tmpSrc+_sizeX-1);
                    tmpB+=*(tmpSrc+_sizeX+1);	
			
                    // + interpolation
                    tmpG=*(tmpSrc+1);
                    tmpG+=*(tmpSrc-1);
                    tmpG+=*(tmpSrc+_sizeX);
                    tmpG+=*(tmpSrc-_sizeX);
			
                    *dest++=*tmpSrc;
                    *dest++=(unsigned char)(tmpG/4);
                    *dest++=(unsigned char)(tmpB/4);
			
                    tmpSrc++;
                }
	
            *dest++=*(tmpSrc-1);
            *dest++=*(tmpSrc);
            *dest++=*(tmpSrc-_sizeX);

            tmpSrc++;
        }

	//////////// ultima riga
	// prima colonna
	*dest++=*(tmpSrc-_sizeX);
	*dest++=*(tmpSrc);
	*dest++=*(tmpSrc+1);

	tmpSrc++;

	for(cc=1;cc<=(sizeX/2-1); cc++)
        {
            *dest++=*(tmpSrc-_sizeX+1);
            *dest++=*(tmpSrc+1);
            *dest++=*(tmpSrc);
            tmpSrc++;

            *dest++=*(tmpSrc-_sizeX);
            *dest++=*(tmpSrc);
            *dest++=*(tmpSrc-_sizeX+1);
            tmpSrc++;
        }

	// ultimo pixel
	*dest++=*(tmpSrc-1-_sizeX);
	*dest++=*(tmpSrc-1);
	*dest++=*tmpSrc;
	tmpSrc++;

	return true;
}

// reconstruct color in a full size image, nearest neighbor interpolation
// Assumes pattern: RGRG...RG
//                  GBGB...GB etc..
bool DragonflyResources::recColorFSNN(const unsigned char *src, unsigned char *dest)
{
	int tmpB=0;
	int tmpG=0;
	int tmpR=0;

	int rr=0;
	int cc=0;

	unsigned char *tmpSrc=const_cast<unsigned char *>(src);

	///////////// prima riga
	// primo pixel
	*dest++=*tmpSrc;
	*dest++=*(tmpSrc+1);
	*dest++=*(tmpSrc+_sizeX+1);
	tmpSrc++;

    // prima riga
	for(cc=1;cc<(_sizeX/2); cc++)
        {
            // first pixel
            *dest++=*(tmpSrc+1);
            *dest++=*(tmpSrc);
            *dest++=*(tmpSrc+_sizeX);

            tmpSrc++;

            // second pixel
            *dest++=*(tmpSrc);
            *dest++=*(tmpSrc+1);
            *dest++=*(tmpSrc+_sizeX-1);

            tmpSrc++;
        }

	// last columns, ends with g
	*dest++=*(tmpSrc-1);
	*dest++=*(tmpSrc);
	*dest++=*(tmpSrc+_sizeX);

	tmpSrc++;

	for (rr=1; rr<=(_sizeY/2-1); rr++)
        {
            ////////////////// gb row
            // prima colonna
            *dest++=*(tmpSrc+_sizeX);
            *dest++=*(tmpSrc+1);
            *dest++=*(tmpSrc);
		
            tmpSrc++;

            for(cc=1; cc<(_sizeX/2); cc++)
                {
                    // second pixel
                    *dest++=*(tmpSrc+_sizeX+1);
                    *dest++=*(tmpSrc+1);
                    *dest++=*tmpSrc;

                    tmpSrc++;

                    // first pixel
                    *dest++=*(tmpSrc+_sizeX);
                    *dest++=*tmpSrc;
                    *dest++=*(tmpSrc+1);

                    tmpSrc++;
                }
            //last col, ends with b
            *dest++=*(tmpSrc+_sizeX-1);
            *dest++=*(tmpSrc+_sizeX);
            *dest++=*tmpSrc;

            tmpSrc++;
			
            ////////////////// gb row
            // prima colonna
            *dest++=*tmpSrc;
            *dest++=*(tmpSrc+1);
            *dest++=*(tmpSrc+_sizeX+1);

            tmpSrc++;

            // altre colonne
            for(cc=1; cc<(_sizeX/2); cc++)
                {
                    // second pixel
                    *dest++=*(tmpSrc+1);
                    *dest++=*tmpSrc;
                    *dest++=*(tmpSrc+_sizeX);

                    tmpSrc++;

                    // first pixel, x interpolation
                    *dest++=*tmpSrc;
                    *dest++=*(tmpSrc+1);
                    *dest++=*(tmpSrc+_sizeX+1);

                    tmpSrc++;
                }
	
            *dest++=*(tmpSrc-1);
            *dest++=*(tmpSrc);
            *dest++=*(tmpSrc-_sizeX);

            tmpSrc++;
        }

	//////////// ultima riga
	// prima colonna
	*dest++=*(tmpSrc-_sizeX);
	*dest++=*(tmpSrc);
	*dest++=*(tmpSrc+1);

	tmpSrc++;

	for(cc=1;cc<=(sizeX/2-1); cc++)
        {
            *dest++=*(tmpSrc-_sizeX+1);
            *dest++=*(tmpSrc+1);
            *dest++=*(tmpSrc);
            tmpSrc++;

            *dest++=*(tmpSrc-_sizeX);
            *dest++=*(tmpSrc);
            *dest++=*(tmpSrc-_sizeX+1);
            tmpSrc++;
        }

	// ultimo pixel
	*dest++=*(tmpSrc-1-_sizeX);
	*dest++=*(tmpSrc-1);
	*dest++=*tmpSrc;
	tmpSrc++;

	return true;
}

////// Reconstruct color methods
// reconstruct color in a full size image, bilinear interpolation
// Assumes pattern: RGRG...RG
//                  GBGB...GB etc..
bool DragonflyResources::recColorHSBilinear(const unsigned char *src, unsigned char *dest)
{
	int tmpB=0;
	int tmpG=0;
	int tmpR=0;

	int rr=0;
	int cc=0;

    int r=0;
    int c=0;

	unsigned char *tmpSrc=const_cast<unsigned char *>(src);

    //////// prima riga
	// primo pixel
	*dest++=*(tmpSrc);
	*dest++=*(tmpSrc+1);
	*dest++=*(tmpSrc+_sizeX+1);
	tmpSrc+=2;

    c++;

    // prima riga
	for(cc=1;cc<(_halfX-1); cc++)
        {
            // first pixel
            tmpB=*(tmpSrc+_sizeX+1);
            tmpB+=*(tmpSrc+_sizeX-1);
        
            tmpG=*(tmpSrc-1);
            tmpG+=*(tmpSrc+1);
            tmpG+=*(tmpSrc+_sizeX);
						
            *dest++=*(tmpSrc);
            *dest++=(unsigned char)(tmpG/3);
            *dest++=(unsigned char)(tmpB/2);

            tmpSrc+=2;

            c++;
        }

  	// last columns, ends with r
	*dest++=*(tmpSrc);
	*dest++=*(tmpSrc-1);
	*dest++=*(tmpSrc+_sizeX-1);
	tmpSrc+=2;
    
    c++;

    // skip a row
    tmpSrc+=_sizeX;

    c=0;
    r++;
	for (rr=1; rr<(_halfY-1); rr++)
        {
            ////////////////// rg row
            // prima colonna
            *dest++=*tmpSrc;
            *dest++=*(tmpSrc+1);
            *dest++=*(tmpSrc+_sizeX+1);
		
            tmpSrc+=2;
            c++;

            for(cc=1; cc<(_halfX-1); cc++)
                {
                    // first pixel
                    tmpB=*(tmpSrc+_sizeX+1);
                    tmpB+=*(tmpSrc+_sizeX-1);
                    tmpB+=*(tmpSrc-_sizeX+1);
                    tmpB+=*(tmpSrc-_sizeX-1);
            
                    tmpG=*(tmpSrc-1);
                    tmpG+=*(tmpSrc+1);
                    tmpG+=*(tmpSrc+_sizeX);
                    tmpG+=*(tmpSrc-_sizeX);
            
                    *dest++=*(tmpSrc);
                    *dest++=(unsigned char)(tmpG/4);
                    *dest++=(unsigned char)(tmpB/4);
            
                    tmpSrc+=2;
                    c++;
                }
            //last col, ends with r
            *dest++=*(tmpSrc);
            *dest++=*(tmpSrc-1);
            *dest++=*(tmpSrc+_sizeX-1);
 
            tmpSrc+=2;
            tmpSrc+=_sizeX; //skip a row

            r++;
            c=0;
        }

    c=0;

    //////// ultima riga
	// primo pixel
	*dest++=*(tmpSrc);
	*dest++=*(tmpSrc+1);
	*dest++=*(tmpSrc-_sizeX+1);
	tmpSrc+=2;

    c++;

    for(cc=1;cc<(_halfX-1); cc++)
        {
            // first pixel
            tmpB=*(tmpSrc-_sizeX+1);
            tmpB+=*(tmpSrc-_sizeX-1);
        
            tmpG=*(tmpSrc-1);
            tmpG+=*(tmpSrc+1);
            tmpG+=*(tmpSrc-_sizeX);
						
            *dest++=*(tmpSrc);
            *dest++=(unsigned char)(tmpG/3);
            *dest++=(unsigned char)(tmpB/2);

            tmpSrc+=2;

            c++;
        }

  	// last columns, ends with r
	*dest++=*(tmpSrc);
	*dest++=*(tmpSrc-1);
	*dest++=*(tmpSrc-_sizeX-1);
	
	return true;
}

////// Reconstruct color and downsampling, general case.
// Assumes pattern: RGRG...RG
//                  GBGB...GB etc..
bool DragonflyResources::recColorGeneral(const unsigned char *src, unsigned char *dest)
{
    bool ret;
    ret=recColorFSNN(src, imageFullSize);
    subSampling(imageFullSize, dest);
    return ret;
}

void DragonflyResources::subSampling(const unsigned char *src, unsigned char *dest)
{
	int srcX, srcY;
	float xRatio, yRatio;
	int srcOffset;

	int srcSizeY = _sizeY, srcSizeX = _sizeX;
	int dstSizeY = sizeY, dstSizeX = sizeX;
	int bytePerPixel = 3;

	xRatio = ((float)srcSizeX)/dstSizeX;
	yRatio = ((float)srcSizeY)/dstSizeY;

    const unsigned char *pSrc = src;
	unsigned char *pDst = dest;

	for (int j=0; j<dstSizeY; j++)
        {
            srcY = (int)(yRatio*j);
            srcOffset = srcY * srcSizeX * bytePerPixel;

            for (int i=0; i<dstSizeX; i++)
                {
                    srcX = (int)(xRatio*i);
                    pSrc = src + srcOffset + (srcX * bytePerPixel);
                    memcpy(pDst,pSrc,bytePerPixel);
                    pDst += bytePerPixel;

                }
        }
}

// converts bayer pattern to BGR
// blue - indicates if the second line contains blue (-1 = YES, 1 = NO)
// starts_with_green - indicates if the line to process has green in the second column (BOOLEAN)
// rgb - indicates if the output color format is RGB ( rgb = 1 ) or BGR (rgb = -1)
// properly setting "blue" and "start_with_green" covers all bayer pattern possibilities
bool Bayer2RGB( const unsigned char* bayer0, int bayer_step, unsigned char *dst0, int dst_step, int width, int height, int blue, int start_with_green, int rgb )
{
	// normalize arguments
    blue = (blue > 0 ? 1 : -1);
    start_with_green = (start_with_green > 0 ? 1 : 0);
	rgb = (rgb > 0 ? 1 : -1);

    memset( dst0, 0, width*3*sizeof(dst0[0]) );
    memset( dst0 + (height - 1)*dst_step, 0, width*3*sizeof(dst0[0]) );
    dst0 += dst_step + 3 + 1;  //jumps to second line, second column, green field
    height -= 2;
    width -= 2;

    for( ; height-- > 0; bayer0 += bayer_step, dst0 += dst_step )
    {
        int t0, t1;
        const unsigned char* bayer = bayer0;
        unsigned char* dst = dst0;
        const unsigned char* bayer_end = bayer + width;

        dst[-4] = dst[-3] = dst[-2] = dst[width*3-1] =
            dst[width*3] = dst[width*3+1] = 0;

        if( width <= 0 )
            continue;

        if( start_with_green )
        {
            t0 = (bayer[1] + bayer[bayer_step*2+1] + 1) >> 1;
            t1 = (bayer[bayer_step] + bayer[bayer_step+2] + 1) >> 1;
            dst[blue*rgb] = (unsigned char)t0;
            dst[0] = bayer[bayer_step+1];
            dst[-blue*rgb] = (unsigned char)t1;
            bayer++;
            dst += 3;
        }

        if( blue > 0 )
        {
            for( ; bayer <= bayer_end - 2; bayer += 2, dst += 6 )
            {
                t0 = (bayer[0] + bayer[2] + bayer[bayer_step*2] +
                      bayer[bayer_step*2+2] + 2) >> 2;
                t1 = (bayer[1] + bayer[bayer_step] +
                      bayer[bayer_step+2] + bayer[bayer_step*2+1]+2) >> 2;
                dst[rgb] = (unsigned char)t0;
                dst[0] = (unsigned char)t1;
                dst[-rgb] = bayer[bayer_step+1];

                t0 = (bayer[2] + bayer[bayer_step*2+2] + 1) >> 1;
                t1 = (bayer[bayer_step+1] + bayer[bayer_step+3] + 1) >> 1;
                dst[3+rgb] = (unsigned char)t0;
                dst[3] = bayer[bayer_step+2];
                dst[3-rgb] = (unsigned char)t1;
            }
        }
        else
        {
            for( ; bayer <= bayer_end - 2; bayer += 2, dst += 6 )
            {
                t0 = (bayer[0] + bayer[2] + bayer[bayer_step*2] +
                      bayer[bayer_step*2+2] + 2) >> 2;
                t1 = (bayer[1] + bayer[bayer_step] +
                      bayer[bayer_step+2] + bayer[bayer_step*2+1]+2) >> 2;
                dst[-rgb] = (unsigned char)t0;
                dst[0] = (unsigned char)t1;
                dst[rgb] = bayer[bayer_step+1];

                t0 = (bayer[2] + bayer[bayer_step*2+2] + 1) >> 1;
                t1 = (bayer[bayer_step+1] + bayer[bayer_step+3] + 1) >> 1;
                dst[3-rgb] = (unsigned char)t0;
                dst[3] = bayer[bayer_step+2];
                dst[3+rgb] = (unsigned char)t1;
            }
        }

        if( bayer < bayer_end )
        {
            t0 = (bayer[0] + bayer[2] + bayer[bayer_step*2] +
                  bayer[bayer_step*2+2] + 2) >> 2;
            t1 = (bayer[1] + bayer[bayer_step] +
                  bayer[bayer_step+2] + bayer[bayer_step*2+1]+2) >> 2;
            dst[blue*rgb] = (unsigned char)t0;
            dst[0] = (unsigned char)t1;
            dst[-blue*rgb] = bayer[bayer_step+1];
            bayer++;
            dst += 3;
        }

        blue = -blue;
        start_with_green = !start_with_green;
    }
    return true;
}


