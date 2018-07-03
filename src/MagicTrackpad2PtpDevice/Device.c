// Device.c: Device handling events for driver.

#include "driver.h"
#include "device.tmh"

_IRQL_requires_(PASSIVE_LEVEL)
static const struct BCM5974_CONFIG*
AmtPtpGetDeviceConfig(
	_In_ USB_DEVICE_DESCRIPTOR deviceInfo
)
{
	USHORT id = deviceInfo.idProduct;
	const struct BCM5974_CONFIG *cfg;

	for (cfg = Bcm5974ConfigTable; cfg->ansi; ++cfg) {
		if (cfg->ansi == id || cfg->iso == id || cfg->jis == id) {
			return cfg;
		}
	}

	return NULL;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpCreateDevice(
	_In_    WDFDRIVER       Driver,
	_Inout_ PWDFDEVICE_INIT DeviceInit
)
{
	WDF_PNPPOWER_EVENT_CALLBACKS		pnpPowerCallbacks;
	WDF_DEVICE_PNP_CAPABILITIES         pnpCaps;
	WDF_OBJECT_ATTRIBUTES				deviceAttributes;
	PDEVICE_CONTEXT						deviceContext;
	WDFDEVICE							device;
	NTSTATUS							status;

	UNREFERENCED_PARAMETER(Driver);
	PAGED_CODE();

	TraceEvents(
		TRACE_LEVEL_INFORMATION, 
		TRACE_DRIVER, 
		"%!FUNC! Entry"
	);

	// Initialize Power Callback
	WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);

	// Initialize PNP power event callbacks
	pnpPowerCallbacks.EvtDevicePrepareHardware = AmtPtpEvtDevicePrepareHardware;
	pnpPowerCallbacks.EvtDeviceD0Entry = AmtPtpEvtDeviceD0Entry;
	pnpPowerCallbacks.EvtDeviceD0Exit = AmtPtpEvtDeviceD0Exit;
	WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

	// Create WDF device object
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);

	status = WdfDeviceCreate(
		&DeviceInit, 
		&deviceAttributes, 
		&device
	);

	if (!NT_SUCCESS(status)) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER,
			"%!FUNC! WdfDeviceCreate failed with Status code %!STATUS!", status);
		return status;
	}

	//
	// Get a pointer to the device context structure that we just associated
	// with the device object. We define this structure in the device.h
	// header file. DeviceGetContext is an inline function generated by
	// using the WDF_DECLARE_CONTEXT_TYPE_WITH_NAME macro in device.h.
	// This function will do the type checking and return the device context.
	// If you pass a wrong object handle it will return NULL and assert if
	// run under framework verifier mode.
	//
	deviceContext = DeviceGetContext(device);

	//
	// Tell the framework to set the SurpriseRemovalOK in the DeviceCaps so
	// that you don't get the popup in usermode 
	// when you surprise remove the device.
	//
	WDF_DEVICE_PNP_CAPABILITIES_INIT(&pnpCaps);
	pnpCaps.SurpriseRemovalOK = WdfTrue;
	WdfDeviceSetPnpCapabilities(
		device, 
		&pnpCaps
	);

	//
	// Create a device interface so that applications can find and talk
	// to us.
	//
	status = WdfDeviceCreateDeviceInterface(
		device,
		&GUID_DEVINTERFACE_AmtPtpDevice,
		NULL // ReferenceString
	);

	if (NT_SUCCESS(status)) {
		//
		// Initialize the I/O Package and any Queues
		//
		status = AmtPtpDeviceQueueInitialize(device);
	}

	TraceEvents(
		TRACE_LEVEL_INFORMATION, 
		TRACE_DRIVER, 
		"%!FUNC! Exit"
	);

	return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpEvtDevicePrepareHardware(
	_In_ WDFDEVICE Device,
	_In_ WDFCMRESLIST ResourceList,
	_In_ WDFCMRESLIST ResourceListTranslated
)
{

	NTSTATUS								status;
	PDEVICE_CONTEXT							pDeviceContext;
	ULONG									waitWakeEnable;
	
	waitWakeEnable = FALSE;

	UNREFERENCED_PARAMETER(ResourceList);
	UNREFERENCED_PARAMETER(ResourceListTranslated);
	PAGED_CODE();

	TraceEvents(
		TRACE_LEVEL_INFORMATION, 
		TRACE_DRIVER, 
		"%!FUNC! Entry"
	);

	status = STATUS_SUCCESS;
	pDeviceContext = DeviceGetContext(Device);

#if USB_TRACKPAD
	WDF_USB_DEVICE_INFORMATION				deviceInfo;

	if (pDeviceContext->UsbDevice == NULL) {
		status = WdfUsbTargetDeviceCreate(Device,
			WDF_NO_OBJECT_ATTRIBUTES,
			&pDeviceContext->UsbDevice
		);

		if (!NT_SUCCESS(status)) {
			TraceEvents(
				TRACE_LEVEL_ERROR, 
				TRACE_DRIVER,
				"%!FUNC! WdfUsbTargetDeviceCreate failed with Status code %!STATUS!", 
				status
			);
			return status;
		}
	}

	// Retrieve device information
	WdfUsbTargetDeviceGetDeviceDescriptor(
		pDeviceContext->UsbDevice, 
		&pDeviceContext->DeviceDescriptor
	);

	if (NT_SUCCESS(status)) {
		// Get correct configuration from conf store
		pDeviceContext->DeviceInfo = AmtPtpGetDeviceConfig(pDeviceContext->DeviceDescriptor);
		if (pDeviceContext->DeviceInfo == NULL) {
			status = STATUS_INVALID_DEVICE_STATE;
			TraceEvents(
				TRACE_LEVEL_ERROR, 
				TRACE_DEVICE, 
				"%!FUNC! failed because device is not found in registry."
			);
			TraceLoggingWrite(
				g_hAmtPtpDeviceTraceProvider,
				EVENT_DEVICE_IDENTIFICATION,
				TraceLoggingString("AmtPtpEvtDevicePrepareHardware", "Routine"),
				TraceLoggingUInt16(pDeviceContext->DeviceDescriptor.idProduct, "idProduct"),
				TraceLoggingUInt16(pDeviceContext->DeviceDescriptor.idVendor, "idVendor"),
				TraceLoggingString(EVENT_DEVICE_ID_SUBTYPE_NOTFOUND, EVENT_DRIVER_FUNC_SUBTYPE)
			);
			return status;
		}

		// Set fuzz information
		pDeviceContext->HorizonalFuzz = pDeviceContext->DeviceInfo->x.snratio ?
			(pDeviceContext->DeviceInfo->x.max - pDeviceContext->DeviceInfo->x.min) / pDeviceContext->DeviceInfo->x.snratio :
			0.0;

		pDeviceContext->VerticalFuzz = pDeviceContext->DeviceInfo->y.snratio ?
			(pDeviceContext->DeviceInfo->y.max - pDeviceContext->DeviceInfo->y.min) / pDeviceContext->DeviceInfo->y.snratio :
			0.0;

		pDeviceContext->PressureFuzz = pDeviceContext->DeviceInfo->p.snratio ?
			(pDeviceContext->DeviceInfo->p.max - pDeviceContext->DeviceInfo->p.min) / pDeviceContext->DeviceInfo->p.snratio :
			0.0;

		pDeviceContext->WidthFuzz = pDeviceContext->DeviceInfo->w.snratio ?
			(pDeviceContext->DeviceInfo->w.max - pDeviceContext->DeviceInfo->w.min) / pDeviceContext->DeviceInfo->w.snratio :
			0.0;

		pDeviceContext->OrientationFuzz = pDeviceContext->DeviceInfo->o.snratio ?
			(pDeviceContext->DeviceInfo->o.max - pDeviceContext->DeviceInfo->o.min) / pDeviceContext->DeviceInfo->o.snratio :
			0.0;

		pDeviceContext->SgContactSizeQualLevel = SIZE_QUALIFICATION_THRESHOLD;
		pDeviceContext->MuContactSizeQualLevel = SIZE_MU_LOWER_THRESHOLD;
		pDeviceContext->PressureQualLevel = PRESSURE_QUALIFICATION_THRESHOLD;

		pDeviceContext->TouchStateMachineInfo.HorizonalFuzz		= pDeviceContext->HorizonalFuzz;
		pDeviceContext->TouchStateMachineInfo.VerticalFuzz		= pDeviceContext->VerticalFuzz;
		pDeviceContext->TouchStateMachineInfo.WidthFuzz			= pDeviceContext->WidthFuzz;
		pDeviceContext->TouchStateMachineInfo.OrientationFuzz	= pDeviceContext->OrientationFuzz;
		pDeviceContext->TouchStateMachineInfo.PressureFuzz		= pDeviceContext->PressureFuzz;

		status = SmResetState(&pDeviceContext->TouchStateMachineInfo);
		if (!NT_SUCCESS(status)) {

			TraceEvents(
				TRACE_LEVEL_ERROR,
				TRACE_DEVICE,
				"%!FUNC! SmResetState failed with %!STATUS!",
				status
			);
			return status;

		}

		TraceEvents(
			TRACE_LEVEL_INFORMATION, 
			TRACE_DEVICE, 
			"%!FUNC! fuzz information: h = %f, v = %f, p = %f, w = %f, o = %f", 
			pDeviceContext->HorizonalFuzz,
			pDeviceContext->VerticalFuzz,
			pDeviceContext->PressureFuzz,
			pDeviceContext->WidthFuzz,
			pDeviceContext->OrientationFuzz
		);
	}

	//
	// Retrieve USBD version information, port driver capabilites and device
	// capabilites such as speed, power, etc.
	//
	WDF_USB_DEVICE_INFORMATION_INIT(&deviceInfo);
	status = WdfUsbTargetDeviceRetrieveInformation(
		pDeviceContext->UsbDevice, 
		&deviceInfo
	);

	if (NT_SUCCESS(status)) {
		TraceEvents(
			TRACE_LEVEL_INFORMATION, 
			TRACE_DEVICE, 
			"%!FUNC! IsDeviceHighSpeed: %s",
			(deviceInfo.Traits & WDF_USB_DEVICE_TRAIT_AT_HIGH_SPEED) ? "TRUE" : "FALSE");

		TraceEvents(
			TRACE_LEVEL_INFORMATION, 
			TRACE_DEVICE,
			"%!FUNC! IsDeviceSelfPowered: %s",
			(deviceInfo.Traits & WDF_USB_DEVICE_TRAIT_SELF_POWERED) ? "TRUE" : "FALSE");

		waitWakeEnable = deviceInfo.Traits &
			WDF_USB_DEVICE_TRAIT_REMOTE_WAKE_CAPABLE;

		TraceEvents(
			TRACE_LEVEL_INFORMATION,
			TRACE_DEVICE,
			"%!FUNC! IsDeviceRemoteWakeable: %s",
			waitWakeEnable ? "TRUE" : "FALSE");

		//
		// Save these for use later.
		//
		pDeviceContext->UsbDeviceTraits = deviceInfo.Traits;
	} else {
		pDeviceContext->UsbDeviceTraits = 0;
	}

	// Select interface to use
	status = SelectInterruptInterface(Device);
	if (!NT_SUCCESS(status)) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "%!FUNC! SelectInterruptInterface failed with %!STATUS!", status);
		return status;
	}

	// Set up interrupt
	status = AmtPtpConfigContReaderForInterruptEndPoint(pDeviceContext);
	if (!NT_SUCCESS(status)) {
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DEVICE, "%!FUNC! AmtPtpConfigContReaderForInterruptEndPoint failed with %!STATUS!", status);
		return status;
	}
#endif
#if SPI_TRACKPAD

	// Get a IO target
	pDeviceContext->SpiTrackpadIoTarget = WdfDeviceGetIoTarget(Device);

	if (pDeviceContext->SpiTrackpadIoTarget == NULL)
	{
		TraceEvents(TRACE_LEVEL_ERROR, TRACE_DRIVER, "WdfDeviceGetIoTarget failed");
		return STATUS_INVALID_DEVICE_STATE;
	}

	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "Open IO target for SPI trackpad. Device is ready to be configured.");

#endif

	// Set default settings
	pDeviceContext->IsButtonReportOn = TRUE;
	pDeviceContext->IsSurfaceReportOn = TRUE;

	TraceEvents(TRACE_LEVEL_INFORMATION, TRACE_DRIVER, "%!FUNC! Exit");
	return status;
}

#if USB_TRACKPAD
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpGetWellspringMode(
	_In_  PDEVICE_CONTEXT DeviceContext,
	_Out_ BOOL* IsWellspringModeOn
)
{

	NTSTATUS						status;
	WDF_USB_CONTROL_SETUP_PACKET	setupPacket;
	WDF_MEMORY_DESCRIPTOR			memoryDescriptor;
	ULONG							cbTransferred;
	WDFMEMORY						bufHandle = NULL;
	unsigned char*					buffer;

	status = STATUS_SUCCESS;

	TraceEvents(
		TRACE_LEVEL_INFORMATION,
		TRACE_DRIVER,
		"%!FUNC! Entry"
	);

	// Type 3 does not need a mode switch.
	if (DeviceContext->DeviceInfo->tp_type == TYPE3) {
		*IsWellspringModeOn = TRUE;
		return STATUS_SUCCESS;
	}

	status = WdfMemoryCreate(
		WDF_NO_OBJECT_ATTRIBUTES,
		PagedPool,
		0,
		DeviceContext->DeviceInfo->um_size,
		&bufHandle,
		&buffer
	);

	if (!NT_SUCCESS(status)) {
		goto cleanup;
	}

	RtlZeroMemory(
		buffer,
		DeviceContext->DeviceInfo->um_size
	);

	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(
		&memoryDescriptor,
		buffer,
		sizeof(DeviceContext->DeviceInfo->um_size)
	);

	WDF_USB_CONTROL_SETUP_PACKET_INIT(
		&setupPacket,
		BmRequestDeviceToHost,
		BmRequestToInterface,
		BCM5974_WELLSPRING_MODE_READ_REQUEST_ID,
		(USHORT)DeviceContext->DeviceInfo->um_req_val,
		(USHORT)DeviceContext->DeviceInfo->um_req_idx
	);

	// Set stuffs right
	setupPacket.Packet.bm.Request.Type = BmRequestClass;

	status = WdfUsbTargetDeviceSendControlTransferSynchronously(
		DeviceContext->UsbDevice,
		WDF_NO_HANDLE,
		NULL,
		&setupPacket,
		&memoryDescriptor,
		&cbTransferred
	);

	// Behavior mismatch: Actual device does not transfer bytes as expected (in length)
	// So we do not check um_size as a temporary workaround.
	if (!NT_SUCCESS(status)) {
		TraceEvents(
			TRACE_LEVEL_ERROR,
			TRACE_DEVICE,
			"%!FUNC! WdfUsbTargetDeviceSendControlTransferSynchronously (Read) failed with %!STATUS!, cbTransferred = %llu, um_size = %d",
			status,
			cbTransferred,
			DeviceContext->DeviceInfo->um_size
		);
		goto cleanup;
	}

	// Check mode switch
	unsigned char wellspringBit = buffer[DeviceContext->DeviceInfo->um_switch_idx];
	*IsWellspringModeOn = wellspringBit == DeviceContext->DeviceInfo->um_switch_on ? TRUE : FALSE;

cleanup:
	TraceEvents(
		TRACE_LEVEL_INFORMATION,
		TRACE_DRIVER,
		"%!FUNC! Exit"
	);

	bufHandle = NULL;
	return status;

}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpSetWellspringMode(
	_In_ PDEVICE_CONTEXT DeviceContext,
	_In_ BOOL IsWellspringModeOn
)
{

	NTSTATUS						status;
	WDF_USB_CONTROL_SETUP_PACKET	setupPacket;
	WDF_MEMORY_DESCRIPTOR			memoryDescriptor;
	ULONG							cbTransferred;
	WDFMEMORY						bufHandle = NULL;
	unsigned char*					buffer;

	TraceEvents(
		TRACE_LEVEL_INFORMATION, 
		TRACE_DRIVER, 
		"%!FUNC! Entry"
	);

	// Type 3 does not need a mode switch.
	// However, turn mode on or off as requested.
	if (DeviceContext->DeviceInfo->tp_type == TYPE3) {
		DeviceContext->IsWellspringModeOn = IsWellspringModeOn;
		return STATUS_SUCCESS;
	}

	status = WdfMemoryCreate(
		WDF_NO_OBJECT_ATTRIBUTES, 
		PagedPool, 
		0, 
		DeviceContext->DeviceInfo->um_size, 
		&bufHandle, 
		&buffer
	);

	if (!NT_SUCCESS(status)) {
		goto cleanup;
	}

	RtlZeroMemory(
		buffer, 
		DeviceContext->DeviceInfo->um_size
	);

	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(
		&memoryDescriptor, 
		buffer, 
		sizeof(DeviceContext->DeviceInfo->um_size)
	);

	WDF_USB_CONTROL_SETUP_PACKET_INIT(
		&setupPacket, 
		BmRequestDeviceToHost, 
		BmRequestToInterface,
		BCM5974_WELLSPRING_MODE_READ_REQUEST_ID,
		(USHORT) DeviceContext->DeviceInfo->um_req_val, 
		(USHORT) DeviceContext->DeviceInfo->um_req_idx
	);

	// Set stuffs right
	setupPacket.Packet.bm.Request.Type = BmRequestClass;

	status = WdfUsbTargetDeviceSendControlTransferSynchronously(
		DeviceContext->UsbDevice, 
		WDF_NO_HANDLE, 
		NULL, 
		&setupPacket, 
		&memoryDescriptor, 
		&cbTransferred
	);

	// Behavior mismatch: Actual device does not transfer bytes as expected (in length)
	// So we do not check um_size as a temporary workaround.
	if (!NT_SUCCESS(status)) {
		TraceEvents(
			TRACE_LEVEL_ERROR, 
			TRACE_DEVICE, 
			"%!FUNC! WdfUsbTargetDeviceSendControlTransferSynchronously (Read) failed with %!STATUS!, cbTransferred = %llu, um_size = %d", 
			status,
			cbTransferred,
			DeviceContext->DeviceInfo->um_size
		);
		goto cleanup;
	}

	// Apply the mode switch
	buffer[DeviceContext->DeviceInfo->um_switch_idx] = IsWellspringModeOn ?
		(unsigned char) DeviceContext->DeviceInfo->um_switch_on : 
		(unsigned char) DeviceContext->DeviceInfo->um_switch_off;

	// Write configuration
	WDF_USB_CONTROL_SETUP_PACKET_INIT(
		&setupPacket, 
		BmRequestHostToDevice, 
		BmRequestToInterface,
		BCM5974_WELLSPRING_MODE_WRITE_REQUEST_ID,
		(USHORT) DeviceContext->DeviceInfo->um_req_val, 
		(USHORT) DeviceContext->DeviceInfo->um_req_idx
	);

	// Set stuffs right
	setupPacket.Packet.bm.Request.Type = BmRequestClass;

	status = WdfUsbTargetDeviceSendControlTransferSynchronously(
		DeviceContext->UsbDevice, 
		WDF_NO_HANDLE, 
		NULL, 
		&setupPacket, 
		&memoryDescriptor, 
		&cbTransferred
	);

	if (!NT_SUCCESS(status)) {
		TraceEvents(
			TRACE_LEVEL_ERROR, 
			TRACE_DEVICE, 
			"%!FUNC! WdfUsbTargetDeviceSendControlTransferSynchronously (Write) failed with %!STATUS!", 
			status
		);
		goto cleanup;
	}

	// Set status
	DeviceContext->IsWellspringModeOn = IsWellspringModeOn;

cleanup:
	TraceEvents(
		TRACE_LEVEL_INFORMATION,
		TRACE_DRIVER,
		"%!FUNC! Exit"
	);

	bufHandle = NULL;
	return status;

}
#endif

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpSpiTrackpadSetStatus(
	_In_ WDFDEVICE Device,
	_In_ BOOLEAN Enabled
)
{
	NTSTATUS Status;
	UCHAR HidPacketBuffer[HID_XFER_PACKET_SIZE];
	WDF_MEMORY_DESCRIPTOR SetStatusMemoryDescriptor;

	PDEVICE_CONTEXT  pDeviceContext;
	PHID_XFER_PACKET pHidPacket;
	PSPI_SET_FEATURE pSpiSetStatus;

	TraceEvents(
		TRACE_LEVEL_INFORMATION,
		TRACE_DRIVER,
		"%!FUNC! Entry"
	);
	
	pDeviceContext = DeviceGetContext(Device);
	pHidPacket = (PHID_XFER_PACKET) &HidPacketBuffer;

	WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(
		&SetStatusMemoryDescriptor,
		(PVOID) &HidPacketBuffer,
		HID_XFER_PACKET_SIZE
	);

	pHidPacket->reportId = HID_REPORTID_MOUSE;
	pHidPacket->reportBufferLen = sizeof(SPI_SET_FEATURE);
	pHidPacket->reportBuffer = (PUCHAR) pHidPacket + sizeof(HID_XFER_PACKET);
	pSpiSetStatus = (PSPI_SET_FEATURE) pHidPacket->reportBuffer;

	pSpiSetStatus->BusLocation = 2;
	pSpiSetStatus->Status = Enabled;

	Status = WdfIoTargetSendIoctlSynchronously(
		pDeviceContext->SpiTrackpadIoTarget,
		NULL,
		IOCTL_HID_SET_FEATURE,
		NULL,
		&SetStatusMemoryDescriptor,
		NULL,
		NULL
	);

	if (!NT_SUCCESS(Status))
	{
		TraceEvents(
			TRACE_LEVEL_ERROR,
			TRACE_DRIVER,
			"%!FUNC! WdfIoTargetSendInternalIoctlSynchronously failed with %!STATUS!",
			Status
		);
	}
	else
	{
		TraceEvents(
			TRACE_LEVEL_INFORMATION,
			TRACE_DRIVER,
			"%!FUNC! Changed trackpad status to %d",
			Enabled
		);
	}

	TraceEvents(
		TRACE_LEVEL_INFORMATION,
		TRACE_DRIVER,
		"%!FUNC! Exit"
	);

	return Status;
}

// D0 Entry & Exit
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpEvtDeviceD0Entry(
	_In_ WDFDEVICE Device,
	_In_ WDF_POWER_DEVICE_STATE PreviousState
)
{
	PDEVICE_CONTEXT         pDeviceContext;
	NTSTATUS                status;
	BOOLEAN                 isTargetStarted;

	pDeviceContext = DeviceGetContext(Device);
	isTargetStarted = FALSE;

	TraceEvents(
		TRACE_LEVEL_INFORMATION, 
		TRACE_DRIVER, 
		"%!FUNC! -->AmtPtpDeviceEvtDeviceD0Entry - coming from %s",
		DbgDevicePowerString(PreviousState)
	);

#if USB_TRACKPAD
	// Check wellspring mode
	if (pDeviceContext->IsButtonReportOn || pDeviceContext->IsWellspringModeOn) {
		TraceEvents(
			TRACE_LEVEL_INFORMATION,
			TRACE_DRIVER,
			"%!FUNC! <--AmtPtpDeviceEvtDeviceD0Entry - Start Wellspring Mode"
		);

		status = AmtPtpSetWellspringMode(
			pDeviceContext,
			TRUE
		);

		if (!NT_SUCCESS(status)) {
			TraceEvents(
				TRACE_LEVEL_WARNING,
				TRACE_DRIVER,
				"%!FUNC! <--AmtPtpDeviceEvtDeviceD0Entry - Start Wellspring Mode failed with %!STATUS!",
				status
			);
		}
	}

	//
	// Since continuous reader is configured for this interrupt-pipe, we must explicitly start
	// the I/O target to get the framework to post read requests.
	//
	status = WdfIoTargetStart(WdfUsbTargetPipeGetIoTarget(pDeviceContext->InterruptPipe));
	if (!NT_SUCCESS(status)) {
		TraceEvents(
			TRACE_LEVEL_ERROR, 
			TRACE_DRIVER, 
			"%!FUNC! <--AmtPtpDeviceEvtDeviceD0Entry - Failed to start interrupt pipe %!STATUS!", 
			status
		);
		goto End;
	}

	isTargetStarted = TRUE;

End:

	if (!NT_SUCCESS(status)) {
		//
		// Failure in D0Entry will lead to device being removed. So let us stop the continuous
		// reader in preparation for the ensuing remove.
		//
		if (isTargetStarted) {
			WdfIoTargetStop(
				WdfUsbTargetPipeGetIoTarget(pDeviceContext->InterruptPipe), 
				WdfIoTargetCancelSentIo
			);
		}
	}
#endif
#if SPI_TRACKPAD
	// Enable trackpad
	TraceEvents(
		TRACE_LEVEL_INFORMATION,
		TRACE_DRIVER,
		"%!FUNC! Configure Trackpad device to enable mode."
	);

	status = AmtPtpSpiTrackpadSetStatus(
		Device,
		TRUE
	);
#endif

	TraceEvents(
		TRACE_LEVEL_INFORMATION, 
		TRACE_DRIVER, 
		"%!FUNC! <--AmtPtpDeviceEvtDeviceD0Entry"
	);

	return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpEvtDeviceD0Exit(
	_In_ WDFDEVICE Device,
	_In_ WDF_POWER_DEVICE_STATE TargetState
)
{
	PDEVICE_CONTEXT         pDeviceContext;
	NTSTATUS				status;

	PAGED_CODE();
	status = STATUS_SUCCESS;

	TraceEvents(
		TRACE_LEVEL_INFORMATION, 
		TRACE_DRIVER,
		"%!FUNC! -->AmtPtpDeviceEvtDeviceD0Exit - moving to %s", 
		DbgDevicePowerString(TargetState)
	);

	pDeviceContext = DeviceGetContext(Device);

#if USB_TRACKPAD
	
	// Stop IO Pipe.
	WdfIoTargetStop(WdfUsbTargetPipeGetIoTarget(
		pDeviceContext->InterruptPipe),
		WdfIoTargetCancelSentIo
	);

	// Cancel Wellspring mode.
	TraceEvents(
		TRACE_LEVEL_INFORMATION,
		TRACE_DRIVER,
		"%!FUNC! -->AmtPtpDeviceEvtDeviceD0Exit - Cancel Wellspring Mode"
	);

	status = AmtPtpSetWellspringMode(
		pDeviceContext,
		FALSE
	);

	if (!NT_SUCCESS(status)) {
		TraceEvents(
			TRACE_LEVEL_WARNING,
			TRACE_DRIVER,
			"%!FUNC! -->AmtPtpDeviceEvtDeviceD0Exit - Cancel Wellspring Mode failed with %!STATUS!",
			status
		);
	}
#endif
#if SPI_TRACKPAD
	// Enable trackpad
	TraceEvents(
		TRACE_LEVEL_INFORMATION,
		TRACE_DRIVER,
		"%!FUNC! Configure Trackpad device to disabled mode."
	);

	status = AmtPtpSpiTrackpadSetStatus(
		Device,
		FALSE
	);
#endif

	TraceEvents(
		TRACE_LEVEL_INFORMATION, 
		TRACE_DRIVER, 
		"%!FUNC! <--AmtPtpDeviceEvtDeviceD0Exit"
	);

	return status;
}

#if USB_TRACKPAD
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
SelectInterruptInterface(
	_In_ WDFDEVICE Device
)
{
	WDF_USB_DEVICE_SELECT_CONFIG_PARAMS configParams;
	NTSTATUS                            status = STATUS_SUCCESS;
	PDEVICE_CONTEXT                     pDeviceContext;
	WDFUSBPIPE                          pipe;
	WDF_USB_PIPE_INFORMATION            pipeInfo;
	UCHAR                               index;
	UCHAR                               numberConfiguredPipes;
	WDFUSBINTERFACE                     usbInterface;

	PAGED_CODE();

	pDeviceContext = DeviceGetContext(Device);
	WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_SINGLE_INTERFACE(&configParams);
	usbInterface = WdfUsbTargetDeviceGetInterface(
		pDeviceContext->UsbDevice,
		0
	);

	if (NULL == usbInterface) {
		status = STATUS_UNSUCCESSFUL;
		TraceEvents(
			TRACE_LEVEL_ERROR, 
			TRACE_DEVICE,
			"%!FUNC! WdfUsbTargetDeviceGetInterface 0 failed %!STATUS!",
			status
		);
		return status;
	}

	configParams.Types.SingleInterface.ConfiguredUsbInterface = usbInterface;
	configParams.Types.SingleInterface.NumberConfiguredPipes = WdfUsbInterfaceGetNumConfiguredPipes(usbInterface);

	pDeviceContext->UsbInterface = configParams.Types.SingleInterface.ConfiguredUsbInterface;
	numberConfiguredPipes = configParams.Types.SingleInterface.NumberConfiguredPipes;

	//
	// Get pipe handles
	//
	for (index = 0; index < numberConfiguredPipes; index++) {

		WDF_USB_PIPE_INFORMATION_INIT(&pipeInfo);

		pipe = WdfUsbInterfaceGetConfiguredPipe(
			pDeviceContext->UsbInterface,
			index, //PipeIndex,
			&pipeInfo
		);

		//
		// Tell the framework that it's okay to read less than
		// MaximumPacketSize
		//
		WdfUsbTargetPipeSetNoMaximumPacketSizeCheck(pipe);

		if (WdfUsbPipeTypeInterrupt == pipeInfo.PipeType) {
			pDeviceContext->InterruptPipe = pipe;
			break;
		}

	}

	//
	// If we didn't find interrupt pipe, fail the start.
	//
	if (!pDeviceContext->InterruptPipe) {
		status = STATUS_INVALID_DEVICE_STATE;
		TraceEvents(
			TRACE_LEVEL_ERROR, 
			TRACE_DEVICE, 
			"%!FUNC! Device is not configured properly %!STATUS!", 
			status
		);

		return status;
	}

	return status;
}
#endif

_IRQL_requires_(PASSIVE_LEVEL)
PCHAR
DbgDevicePowerString(
	_In_ WDF_POWER_DEVICE_STATE Type
)
{
	switch (Type)
	{
	case WdfPowerDeviceInvalid:
		return "WdfPowerDeviceInvalid";
	case WdfPowerDeviceD0:
		return "WdfPowerDeviceD0";
	case WdfPowerDeviceD1:
		return "WdfPowerDeviceD1";
	case WdfPowerDeviceD2:
		return "WdfPowerDeviceD2";
	case WdfPowerDeviceD3:
		return "WdfPowerDeviceD3";
	case WdfPowerDeviceD3Final:
		return "WdfPowerDeviceD3Final";
	case WdfPowerDevicePrepareForHibernation:
		return "WdfPowerDevicePrepareForHibernation";
	case WdfPowerDeviceMaximum:
		return "WdfPowerDeviceMaximum";
	default:
		return "UnKnown Device Power State";
	}
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
AmtPtpEmergResetDevice(
	_In_ PDEVICE_CONTEXT DeviceContext
)
{

	NTSTATUS status = STATUS_SUCCESS;
	TraceEvents(
		TRACE_LEVEL_INFORMATION,
		TRACE_DRIVER,
		"%!FUNC! Entry");

	UNREFERENCED_PARAMETER(DeviceContext);

#if USB_TRACKPAD

	status = AmtPtpSetWellspringMode(
		DeviceContext, 
		FALSE);

	if (!NT_SUCCESS(status)) {
		TraceEvents(
			TRACE_LEVEL_ERROR, 
			TRACE_DEVICE, 
			"%!FUNC! AmtPtpSetWellspringMode failed with %!STATUS!", 
			status);
	}

	status = AmtPtpSetWellspringMode(
		DeviceContext,
		TRUE);

	if (!NT_SUCCESS(status)) {
		TraceEvents(
			TRACE_LEVEL_ERROR,
			TRACE_DEVICE,
			"%!FUNC! AmtPtpSetWellspringMode failed with %!STATUS!",
			status);
	}

#endif

	TraceEvents(
		TRACE_LEVEL_INFORMATION, 
		TRACE_DRIVER, 
		"%!FUNC! Exit");

	return status;

}
