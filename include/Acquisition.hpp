/**
 *  @example Acquisition.cpp
 *
 *  @brief Acquisition.cpp shows how to acquire images. It relies on 
 *  information provided in the Enumeration example. Also, check out the
 *  ExceptionHandling and NodeMapInfo examples if you haven't already. 
 *  ExceptionHandling shows the handling of standard and Spinnaker exceptions
 *  while NodeMapInfo explores retrieving information from various node types.  
 *
 *  This example touches on the preparation and cleanup of a camera just before
 *  and just after the acquisition of images. Image retrieval and conversion,
 *  grabbing image data, and saving images are all covered as well.
 *
 *  Once comfortable with Acquisition, we suggest checking out 
 *  AcquisitionMultipleCamera, NodeMapCallback, or SaveToAvi. 
 *  AcquisitionMultipleCamera demonstrates simultaneously acquiring images from 
 *  a number of cameras, NodeMapCallback serves as a good introduction to 
 *  programming with callbacks and events, and SaveToAvi exhibits video creation.
 */
 
#include "Spinnaker.h"
#include "SpinGenApi/SpinnakerGenApi.h"
#include <iostream>
#include <sstream>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>

#ifndef _WIN32
#include <pthread.h>
#endif

using namespace Spinnaker;
using namespace Spinnaker::GenApi;
using namespace Spinnaker::GenICam;
using namespace std;

enum triggerType
{
	SOFTWARE,
	HARDWARE
};
const triggerType chosenTrigger = HARDWARE;
cv::Point2i offset[4] = { cv::Point(500, 500), cv::Point(500,300), cv::Point(750,500), cv::Point(800,300) };
const int64_t numBuffers = 3;
const float frameRate = 30.0f;
int64_t height[4] = { 1280, 1280, 1280, 1280 };
int64_t width[4] = { 800, 800, 736, 736 };
bool SetExposureManual = false;
class AcquisitionParameters
{
public:
	AcquisitionParameters()
	{
		pCam = NULL;
		cvImage = & cv::Mat(1280, 800, CV_8UC3);
	}
	CameraPtr pCam = NULL;
	cv::Mat* cvImage;
};
#ifdef _DEBUG
// Disables heartbeat on GEV cameras(GigE Vison) so debugging does not incur timeout errors
int DisableHeartbeat(CameraPtr pCam, INodeMap & nodeMap, INodeMap & nodeMapTLDevice)
{
    cout << "Checking device type to see if we need to disable the camera's heartbeat..." << "\n" << endl;
    //
    // Write to boolean node controlling the camera's heartbeat
    // 
    // *** NOTES ***
    // This applies only to GEV cameras and only applies when in DEBUG mode.
    // GEV cameras have a heartbeat built in, but when debugging applications the
    // camera may time out due to its heartbeat. Disabling the heartbeat prevents 
    // this timeout from occurring, enabling us to continue with any necessary debugging.
    // This procedure does not affect other types of cameras and will prematurely exit
    // if it determines the device in question is not a GEV camera. 
    //
    // *** LATER ***
    // Since we only disable the heartbeat on GEV cameras during debug mode, it is better
    // to power cycle the camera after debugging. A power cycle will reset the camera 
    // to its default settings. 
    // 

    CEnumerationPtr ptrDeviceType = nodeMapTLDevice.GetNode("DeviceType");
    if (!IsAvailable(ptrDeviceType) && !IsReadable(ptrDeviceType))
    {
        cout << "Error with reading the device's type. Aborting..." << "\n" << endl;
        return -1;
    }
    else
    {
        if (ptrDeviceType->GetIntValue() == DeviceType_GEV)
        {
            cout << "Working with a GigE camera. Attempting to disable heartbeat before continuing..." << "\n" << endl;
            CBooleanPtr ptrDeviceHeartbeat = nodeMap.GetNode("GevGVCPHeartbeatDisable");
            if ( !IsAvailable(ptrDeviceHeartbeat) || !IsWritable(ptrDeviceHeartbeat) )
            {
                cout << "Unable to disable heartbeat on camera. Aborting..." << "\n" << endl;
                return -1;
            }
            ptrDeviceHeartbeat->SetValue(true);
            cout << "WARNING: Heartbeat on GigE camera disabled for the rest of Debug Mode." << endl;
            cout << "         Power cycle camera when done debugging to re-enable the heartbeat..." << "\n" << endl;
        }
        else
        {
            cout << "Camera does not use GigE interface. Resuming normal execution..." << "\n" << endl;
        }
    }
    return 0;
}
#endif
cv::Mat ConvertToCVmat(ImagePtr spinImage);
bool ConfigCamera(CameraPtr pCam);
int ResetExposure(CameraPtr pCam);
int PrintDeviceInfo(INodeMap & nodeMap);
/*
 * This function shows how to convert between Spinnaker ImagePtr container to CVmat container used in OpenCV.
*/
cv::Mat ConvertToCVmat(ImagePtr spinImage)
{
    unsigned int XPadding = spinImage->GetXPadding();
    unsigned int YPadding = spinImage->GetYPadding();
    unsigned int rowsize = spinImage->GetWidth();
    unsigned int colsize = spinImage->GetHeight();
    //image data contains padding. When allocating Mat container size, you need to account for the X,Y image data padding.
    return cv::Mat(colsize + YPadding, rowsize + XPadding, CV_MAKETYPE(CV_8U, spinImage->GetNumChannels()), spinImage->GetData(), spinImage->GetStride());
}
// This function acquires images from a device.
#if defined (_WIN32)
DWORD WINAPI AcquireImages(LPVOID lpParam)
{
	AcquisitionParameters acqPara = *((AcquisitionParameters*) lpParam);
	CameraPtr pCam = acqPara.pCam;
	cv::Mat* cvImage = acqPara.cvImage;
#else
void* AcquireImages(void* arg)
{
	AcquisitionParameters acqPara = *((AcquisitionParameters*)lpParam);
	CameraPtr pCam = acqPara.pCam;
	cv::Mat* cvImage = acqPara.cvImage;
#endif

    try
    {
		// Use trigger to capture image
		//
		// *** NOTES ***
		
		if (chosenTrigger == SOFTWARE)
		{
			// Execute software trigger
			if (pCam->TriggerSoftware == NULL || pCam->TriggerSoftware.GetAccessMode() != WO)
			{
				cout << "Unable to execute trigger..." << endl;
			}

			pCam->TriggerSoftware.Execute();
		}
		else
		{
			cout << "Use the hardware to trigger image acquisition." << endl;
		}
        // Retrieve next received image
        //
        // *** NOTES ***`
        // Capturing an image houses images on the camera buffer. Trying
        // to capture an image that does not exist will hang the camera.
        //
        // *** LATER ***
        // Once an image from the buffer is saved and/or no longer
        // needed, the image must be released in order to keep the
        // buffer from filling up.
        //
        ImagePtr pResultImage = pCam->GetNextImage();

        // Ensure image completion
        //
        if (pResultImage->IsIncomplete())
        {
            // Retreive and print the image status description
            std::cout << "Image incomplete: "
                    << Image::GetImageStatusDescription(pResultImage->GetImageStatus())
                    << "..." << "\n" << endl;
        }
        else
        {
            *cvImage = ConvertToCVmat(pResultImage);
            pResultImage->Release();
            //needs to be converted into BGR(OpenCV uses RGB)
            //cv::cvtColor(cvImage, cvImage, CV_BayerGB2BGR);
			// 使用指针参数，直接对形参赋值
			//cv::cvtColor(*cvImage, *cvImage, CV_RGB2BGR);
			//cout << "Acquiring image finished" << endl;
			return true;
        }
    }
    catch (Spinnaker::Exception &e)
    {
        std::cout << "Error during acquiring images: " << e.what() << endl;
    }
}
// This function config the camera settings
bool ConfigCamera(CameraPtr pCam, int cameraIndex)
{
	bool status = true;
    
    std::cout << "\n" << "\n" << "*** IMAGE ACQUISITION ***" << "\n" << endl;
    
    try
    {
        INodeMap & nodeMapTLDevice = pCam->GetTLDeviceNodeMap();
		INodeMap& sNodeMap = pCam->GetTLStreamNodeMap();
        // Retrieve GenICam nodemap
        INodeMap & nodeMap = pCam->GetNodeMap();
		// The device serial number is retrieved in order to keep cameras from 
		// overwriting one another. Grabbing image IDs could also accomplish
		// this.
		//

		gcstring deviceSerialNumber("");
		CStringPtr ptrStringSerial = nodeMapTLDevice.GetNode("DeviceSerialNumber");
		if (IsAvailable(ptrStringSerial) && IsReadable(ptrStringSerial))
		{
			deviceSerialNumber = ptrStringSerial->GetValue();

			std::cout << "Device serial number retrieved as " << deviceSerialNumber << "..." << endl;
		}
		// Set acquisition mode to continuous
		if (!IsReadable(pCam->AcquisitionMode) || !IsWritable(pCam->AcquisitionMode))
		{
			cout << "Unable to set acquisition mode to continuous. Aborting..." << "\n" << endl;
			status = false;
		}

		pCam->AcquisitionMode.SetValue(AcquisitionMode_Continuous);
		cout << "Acquisition mode set to continuous..." << endl;
		
		if (pCam->PixelFormat != NULL && pCam->PixelFormat.GetAccessMode() == RW)
		{
			pCam->PixelFormat.SetValue(PixelFormat_RGB8);

			cout << "Pixel format set to " << pCam->PixelFormat.GetCurrentEntry()->GetSymbolic() << "..." << endl;
		}
		else
		{
			cout << "Pixel format not available..." << endl;
			status = false;
		}
		// Retrieve Buffer Handling Mode Information
		try
		{
			CEnumerationPtr ptrHandlingMode = sNodeMap.GetNode("StreamBufferHandlingMode");
			if (!IsAvailable(ptrHandlingMode) || !IsWritable(ptrHandlingMode))
			{
				cout << "Unable to set Buffer Handling mode (node retrieval). Aborting..." << endl << endl;
				return false;
			}

			CEnumEntryPtr ptrHandlingModeEntry = ptrHandlingMode->GetCurrentEntry();
			if (!IsAvailable(ptrHandlingModeEntry) || !IsReadable(ptrHandlingModeEntry))
			{
				cout << "Unable to set Buffer Handling mode (Entry retrieval). Aborting..." << endl << endl;
				return false;
			}

			// Set stream buffer Count Mode to manual
			CEnumerationPtr ptrStreamBufferCountMode = sNodeMap.GetNode("StreamBufferCountMode");
			if (!IsAvailable(ptrStreamBufferCountMode) || !IsWritable(ptrStreamBufferCountMode))
			{
				cout << "Unable to set Buffer Count Mode (node retrieval). Aborting..." << endl << endl;
				return false;
			}

			CEnumEntryPtr ptrStreamBufferCountModeManual = ptrStreamBufferCountMode->GetEntryByName("Manual");
			if (!IsAvailable(ptrStreamBufferCountModeManual) || !IsReadable(ptrStreamBufferCountModeManual))
			{
				cout << "Unable to set Buffer Count Mode entry (Entry retrieval). Aborting..." << endl << endl;
				return false;
			}

			ptrStreamBufferCountMode->SetIntValue(ptrStreamBufferCountModeManual->GetValue());

			cout << "Stream Buffer Count Mode set to manual..." << endl;

			// Retrieve and modify Stream Buffer Count
			CIntegerPtr ptrBufferCount = sNodeMap.GetNode("StreamBufferCountManual");
			if (!IsAvailable(ptrBufferCount) || !IsWritable(ptrBufferCount))
			{
				cout << "Unable to set Buffer Count (Integer node retrieval). Aborting..." << endl << endl;
				return false;
			}

			// Display Buffer Info
			cout << endl << "Default Buffer Handling Mode: " << ptrHandlingModeEntry->GetDisplayName() << endl;
			cout << "Default Buffer Count: " << ptrBufferCount->GetValue() << endl;
			cout << "Maximum Buffer Count: " << ptrBufferCount->GetMax() << endl;

			ptrBufferCount->SetValue(numBuffers);

			cout << "Buffer count now set to: " << ptrBufferCount->GetValue() << endl;
			ptrHandlingModeEntry = ptrHandlingMode->GetEntryByName("NewestOnly");
			ptrHandlingMode->SetIntValue(ptrHandlingModeEntry->GetValue());
			cout << endl << endl << "Buffer Handling Mode has been set to " << ptrHandlingModeEntry->GetDisplayName() << endl;
		}
		catch (Spinnaker::Exception& e)
		{
			cout << "Error during setting Buffers: " << e.what() << endl;
		}
		//Set FrameRate
		//	Frame rate setting
		//	Turn on frame rate control 
		try
		{
			
			// Trigger Mode needs to be turned off to set frame rate
			if (pCam->TriggerMode == NULL || pCam->TriggerMode.GetAccessMode() != RW)
			{
				cout << "Unable to disable trigger mode. Aborting..." << endl;
				return -1;
			}

			pCam->TriggerMode.SetValue(TriggerMode_Off);
			// enable frame rate setting
			cout << "Trigger mode disabled..." << endl;
			if (!IsReadable(pCam->AcquisitionFrameRateEnable) || !IsWritable(pCam->AcquisitionFrameRateEnable))
			{
				cout << " Unable to enable Acquisition Frame Rate (node retrieval)" << endl;
				cout << "IsReadble: " << IsReadable(pCam->AcquisitionFrameRateEnable) <<
						", IsWritable: " << IsWritable(pCam->AcquisitionFrameRateEnable) << endl;
			}
			pCam->AcquisitionFrameRateEnable.SetValue(true);
			cout << "AcquisitionFrameRate Enabled..." << endl;
			// read current frame rate
			if (!IsReadable(pCam->AcquisitionFrameRate))
			{
				cout << " Unable to read Acquisition Frame Rate (node retrieval)" << endl;
			}
			cout << "Current Frame Rate: " << pCam->AcquisitionFrameRate.GetValue() << endl;
			// set frame rate
			if (!IsReadable(pCam->AcquisitionFrameRate) || !IsWritable(pCam->AcquisitionFrameRate))
			{
				cout << " Unable to set Acquisition Frame Rate (node retrieval)" << endl;
				cout << "IsReadble: " << IsReadable(pCam->AcquisitionFrameRate) <<
					", IsWritable: " << IsWritable(pCam->AcquisitionFrameRate) << endl;
			}
			pCam->AcquisitionFrameRate.SetValue(frameRate);
			cout << "Camera Frame rate is set to " << frameRate << endl;
		}
		catch (Spinnaker::Exception& e)
		{
			cout << "Error during setting Acquisition rate: " << e.what() << endl;
		}
		
		// Set maximum width
		//
		// *** NOTES ***
		// Other nodes, such as those corresponding to image width and height, 
		// might have an increment other than 1. In these cases, it can be
		// important to check that the desired value is a multiple of the
		// increment. 
		//
		// This is often the case for width and height nodes. However, because
		// these nodes are being set to their maximums, there is no real reason
		// to check against the increment.
		// The offsetX and offsetY are influenced by height and width
		// So make sure you config height and width, then config offset
		// 偏移量offset受到height 和 width影响，所以先设置宽和高
		if (IsReadable(pCam->Width) && IsWritable(pCam->Width) && pCam->Width.GetInc() != 0 && pCam->Width.GetMax() != 0)
		{
			pCam->Width.SetValue(width[cameraIndex]); // X 方向宽度的递增量为32， 确保Width是32的整倍数

			cout << "Width set to " << pCam->Width.GetValue() << "..." << endl;
		}
		else
		{
			cout << "Width not available..." << endl;
			status = false;
		}
		if (IsReadable(pCam->Height) && IsWritable(pCam->Height) && pCam->Height.GetInc() != 0 && pCam->Height.GetMax() != 0)
		{
			pCam->Height.SetValue(height[cameraIndex]);

			cout << "Height set to " << pCam->Height.GetValue() << "..." << endl;
		}
		else
		{
			cout << "Height not available..." << endl;
			status = false;
		}
		if (IsReadable(pCam->OffsetX) && IsWritable(pCam->OffsetX))
		{
			pCam->OffsetX.SetValue(offset[cameraIndex].x);

			cout << "Offset X set to " << pCam->OffsetX.GetValue() << "..." << endl;
		}
		else
		{
			cout << "Offset X not available..." << endl;
			status = false;
		}
		//
		if (IsReadable(pCam->OffsetY) && IsWritable(pCam->OffsetY))
		{
			pCam->OffsetY.SetValue(offset[cameraIndex].y);

			cout << "Offset Y set to " << pCam->OffsetY.GetValue() << "..." << endl;
		}
		else
		{
			cout << "Offset Y not available..." << endl;
			status = false;
		}
        //process_status = process_status && PrintDeviceInfo(nodeMapTLDevice);
#ifdef _DEBUG
        cout << "\n" << endl << "*** DEBUG ***" << "\n" << endl;
        // If using a GEV camera and debugging, should disable heartbeat first to prevent further issues
        if (DisableHeartbeat(pCam, nodeMap, nodeMapTLDevice) != 0)
        {
            return false;
        }
        cout << "\n" << endl << "*** END OF DEBUG ***" << "\n" << endl;
#endif
		
		try
		{
			if (chosenTrigger == SOFTWARE)
			{
				cout << "Software trigger chosen..." << endl;
			}
			else
			{
				cout << "Hardware trigger chosen..." << endl;
			}

			//
			// Ensure trigger mode off
			//
			// *** NOTES ***
			// The trigger must be disabled in order to configure whether the source
			// is software or hardware.
			//
			if (pCam->TriggerMode == NULL || pCam->TriggerMode.GetAccessMode() != RW)
			{
				cout << "Unable to disable trigger mode. Aborting..." << endl;
				return -1;
			}

			pCam->TriggerMode.SetValue(TriggerMode_Off);

			cout << "Trigger mode disabled..." << endl;

			//
			// Select trigger source
			//
			// *** NOTES ***
			// The trigger source must be set to hardware or software while trigger 
			// mode is off.
			//
			if (chosenTrigger == SOFTWARE)
			{
				// Set the trigger source to software
				if (pCam->TriggerSource == NULL || pCam->TriggerSource.GetAccessMode() != RW)
				{
					cout << "Unable to set trigger mode (node retrieval). Aborting..." << endl;
					return -1;
				}

				pCam->TriggerSource.SetValue(TriggerSource_Software);

				cout << "Trigger source set to software..." << endl;
			}
			else
			{
				// Set the trigger source to hardware (using 'Line0')
				if (pCam->TriggerSource == NULL || pCam->TriggerSource.GetAccessMode() != RW)
				{
					cout << "Unable to set trigger mode (node retrieval). Aborting..." << endl;
					return -1;
				}

				pCam->TriggerSource.SetValue(TriggerSource_Line0);

				cout << "Trigger source set to hardware..." << endl;
			}

			//
			// Turn trigger mode on
			//
			// *** LATER ***
			// Once the appropriate trigger source has been set, turn trigger mode 
			// back on in order to retrieve images using the trigger.
			//
			if (pCam->TriggerMode == NULL || pCam->TriggerMode.GetAccessMode() != RW)
			{
				cout << "Unable to disable trigger mode. Aborting..." << endl;
				return -1;
			}

			pCam->TriggerMode.SetValue(TriggerMode_On);

			cout << "Trigger mode turned back on..." << endl << endl;
		}
		catch (Spinnaker::Exception& e)
		{
			cout << "Error during config trigger: " << e.what() << endl;
			status = false;
		}
		
		cout << "\n" << "\n" << "*** CONFIGURING EXPOSURE ***" << "\n" << endl;

		// Turn off automatic exposure mode
		//
		// *** NOTES ***
		// Automatic exposure prevents the manual configuration of exposure 
		// times and needs to be turned off for this example. 
		// *** LATER ***
		// Exposure time can be set automatically or manually as needed. This
		// example turns automatic exposure off to set it manually and back
		// on to return the camera to its default state.
		//
		try
		{
			
			if (SetExposureManual)
			{
				if (!IsReadable(pCam->ExposureAuto) || !IsWritable(pCam->ExposureAuto))
				{
					cout << "Unable to disable automatic exposure. Aborting..." << "\n" << endl;
					return false;
				}

				pCam->ExposureAuto.SetValue(ExposureAuto_Off);

				cout << "Automatic exposure disabled..." << endl;
				//
				// Set exposure time manually; exposure time recorded in microseconds
				//
				// *** NOTES ***
				// Notice that the node is checked for availability and writability
				// prior to the setting of the node. In QuickSpin, availability is
				// ensured by checking for null while writability is ensured by checking
				// the access mode. 
				//
				// Further, it is ensured that the desired exposure time does not exceed 
				// the maximum. Exposure time is counted in microseconds - this can be 
				// found out either by retrieving the unit with the GetUnit() method or 
				// by checking SpinView.
				// 
				if (!IsReadable(pCam->ExposureTime) || !IsWritable(pCam->ExposureTime))
				{
					cout << "Unable to set exposure time. Aborting..." << "\n" << endl;
					return false;
				}

				// Ensure desired exposure time does not exceed the maximum
				const float exposureTimeMax = pCam->ExposureTime.GetMax();
				const float exposureTimeMin = pCam->ExposureTime.GetMin();
				cout << "Max exposure time: " << exposureTimeMax << ", min exposure time: " << exposureTimeMin << endl;
				float exposureTimeToSet = 17000.0f;

				if (exposureTimeToSet > exposureTimeMax || exposureTimeToSet < exposureTimeMin)
				{
					exposureTimeToSet = exposureTimeMin;
				}

				pCam->ExposureTime.SetValue(exposureTimeToSet);

				cout << std::fixed << "Shutter time set to " << exposureTimeToSet << " us..." << "\n" << endl;
			}
			else
			{
				if (!IsReadable(pCam->ExposureAuto) || !IsWritable(pCam->ExposureAuto))
				{
					cout << "Unable to enable automatic exposure. Aborting..." << "\n" << endl;
					return false;
				}

				pCam->ExposureAuto.SetValue(ExposureAuto_Continuous);

				cout << "Automatic exposure disabled..." << endl;
			}
			
		}
		catch (Spinnaker::Exception& e)
		{
			cout << "Error during config Exposure: " << e.what() << endl;
			status = false;
		}
		
    }
    catch (Spinnaker::Exception &e)
    {
        std::cout << "Spinnaker Error during config Camera: " << e.what() << endl;
        status = false;
    }
    catch (cv::Exception &e)
    {
        std::cout << "CV Error during config Camera: " << e.what() << endl;
        status = false;
    }
    return status;
}
// This function prints the device information of the camera from the transport
// layer; please see NodeMapInfo example for more in-depth comments on printing
// device information from the nodemap.
int PrintDeviceInfo(INodeMap & nodeMap)
{
    bool process_status = true;
    
    std::cout << "\n" << "*** DEVICE INFORMATION ***" << "\n" << endl;

    try
    {
        FeatureList_t features;
        CCategoryPtr category = nodeMap.GetNode("DeviceInformation");
        if (IsAvailable(category) && IsReadable(category))
        {
            category->GetFeatures(features);

            FeatureList_t::const_iterator it;
            for (it = features.begin(); it != features.end(); ++it)
            {
                CNodePtr pfeatureNode = *it;
                std::cout << pfeatureNode->GetName() << " : ";
                CValuePtr pValue = (CValuePtr)pfeatureNode;
                std::cout << (IsReadable(pValue) ? pValue->ToString() : "Node not readable");
                std::cout << endl;
            }
        }
        else
        {
            std::cout << "Device control information not available." << endl;
        }
    }
    catch (Spinnaker::Exception &e)
    {
        std::cout << "Error: during print Device information" << e.what() << endl;
        process_status = false;
    }
    
    return process_status;
}

// This function returns the camera to a normal state by re-enabling automatic
// exposure.
int ResetExposure(CameraPtr pCam)
{
	int result = 0;

	try
	{
		// 
		// Turn automatic exposure back on
		//
		// *** NOTES ***
		// Automatic exposure is turned on in order to return the camera to its 
		// default state.
		//
		if (!IsReadable(pCam->ExposureAuto) || !IsWritable(pCam->ExposureAuto))
		{
			cout << "Unable to enable automatic exposure (node retrieval). Non-fatal error..." << endl << endl;
			return -1;
		}

		pCam->ExposureAuto.SetValue(ExposureAuto_Continuous);

		cout << "Automatic exposure enabled..." << endl << endl;
	}
	catch (Spinnaker::Exception& e)
	{
		cout << "Error: " << e.what() << endl;
		result = -1;
	}

	return result;
}

bool ResetTrigger(CameraPtr pCam)
{
	bool status = true;
	try
	{
		if (pCam->TriggerMode == NULL || pCam->TriggerMode.GetAccessMode() != RW)
		{
			cout << "Unable to disable trigger mode. Aborting..." << endl;
			return false;
		}
		pCam->TriggerMode.SetValue(TriggerMode_Off);

		cout << "Trigger mode disabled..." << endl;
	}
	catch (Spinnaker::Exception& e)
	{
		cout << "Error: " << e.what() << endl;
		status = false;
	}
}