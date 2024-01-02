// Driver.c: Common entry point and WPP trace filter handler

#include "MouseLikeTouchPad_HidFilter.h"

#if DBG 

#define KdPrintData(_x_) KdPrintDataFun _x_

#else 

#define KdPrintData(_x_)

#endif // DBG wudfwdm


//extern "C" int _fltused = 0;
#ifdef __cplusplus
extern "C" int _fltused = 0;
#endif

#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, DriverEntry)
#pragma alloc_text (PAGE, PtpFilterEvtDeviceAdd)
#pragma alloc_text (PAGE, PtpFilterEvtDriverContextCleanup)
#pragma alloc_text (PAGE, PtpFilterCreateDevice)
#pragma alloc_text (PAGE, PtpFilterIoQueueInitialize)
#endif



NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    NTSTATUS status;
    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;

    //PAGED_CODE();

    KdPrint(("DriverEntry start , %x\n", 0));

    // Register a cleanup callback
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.EvtCleanupCallback = PtpFilterEvtDriverContextCleanup;

    // Register WDF driver
    WDF_DRIVER_CONFIG_INIT(&config, PtpFilterEvtDeviceAdd);
    status = WdfDriverCreate(DriverObject, RegistryPath, &attributes, &config, WDF_NO_HANDLE);

    if (!NT_SUCCESS(status)) {
        KdPrint(("WdfDriverCreate failed , %x\n", status));
        return status;
    }

    KdPrint(("DriverEntry end , %x\n", status));
    return STATUS_SUCCESS;
}

NTSTATUS
PtpFilterEvtDeviceAdd(
    _In_    WDFDRIVER       Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
)
{
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);
    //PAGED_CODE();
    
    // We do not own power control.
    // In addition we do not own every I/O request.
    WdfFdoInitSetFilter(DeviceInit);

    // Create the device.
    status = PtpFilterCreateDevice(DeviceInit);
    return status;
}

VOID
PtpFilterEvtDriverContextCleanup(
    _In_ WDFOBJECT DriverObject
)
{
    UNREFERENCED_PARAMETER(DriverObject);
    //PAGED_CODE();
}



NTSTATUS
PtpFilterCreateDevice(
    _Inout_ PWDFDEVICE_INIT DeviceInit
)
{
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpPowerCallbacks;
    WDF_TIMER_CONFIG timerConfig;
    WDF_WORKITEM_CONFIG workitemConfig;
    WDFDEVICE device;
    PDEVICE_CONTEXT deviceContext;
    NTSTATUS status;

    //PAGED_CODE();
    
    KdPrint(("PtpFilterCreateDevice start , %x\n", 0));

    // Initialize Power Callback
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);
    pnpPowerCallbacks.EvtDevicePrepareHardware = PtpFilterPrepareHardware;
    pnpPowerCallbacks.EvtDeviceD0Entry = PtpFilterDeviceD0Entry;
    pnpPowerCallbacks.EvtDeviceD0Exit = PtpFilterDeviceD0Exit;
    pnpPowerCallbacks.EvtDeviceSelfManagedIoInit = PtpFilterSelfManagedIoInit;
    pnpPowerCallbacks.EvtDeviceSelfManagedIoRestart = PtpFilterSelfManagedIoRestart;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

    // Create WDF device object
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);
    if (!NT_SUCCESS(status)) {
        KdPrint(("PtpFilterCreateDevice WdfDeviceCreate failed, %x\n", status));    
        goto exit;
    }

    // Initialize context and interface
    deviceContext = PtpFilterGetContext(device);
    deviceContext->Device = device;
    deviceContext->WdmDeviceObject = WdfDeviceWdmGetDeviceObject(device);
    if (deviceContext->WdmDeviceObject == NULL) {
        KdPrint(("PtpFilterCreateDevice WdfDeviceWdmGetDeviceObject failed, %x\n", status));    
        goto exit;
    }

    status = WdfDeviceCreateDeviceInterface(device, &GUID_DEVICEINTERFACE_MouseLikeTouchPad_HidFilter, NULL);
    if (!NT_SUCCESS(status)) {
        KdPrint(("PtpFilterCreateDevice WdfDeviceCreateDeviceInterface failed, %x\n", status));
        goto exit;
    }

    // Initialize read buffer
    status = WdfLookasideListCreate(WDF_NO_OBJECT_ATTRIBUTES, REPORT_BUFFER_SIZE,
                                    NonPagedPoolNx, WDF_NO_OBJECT_ATTRIBUTES, PTP_LIST_POOL_TAG,
                                    &deviceContext->HidReadBufferLookaside);
    if (!NT_SUCCESS(status)) {
        KdPrint(("PtpFilterCreateDevice WdfLookasideListCreate failed, %x\n", status)); 
    }

    // Initialize HID recovery timer
    WDF_TIMER_CONFIG_INIT(&timerConfig, PtpFilterRecoveryTimerCallback);
    timerConfig.AutomaticSerialization = TRUE;
    WDF_OBJECT_ATTRIBUTES_INIT(&deviceAttributes);
    deviceAttributes.ParentObject = device;
    deviceAttributes.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfTimerCreate(&timerConfig, &deviceAttributes, &deviceContext->HidTransportRecoveryTimer);
    if (!NT_SUCCESS(status)) {
        KdPrint(("PtpFilterCreateDevice WdfTimerCreate failed, %x\n", status));
    }

    // Initialize HID recovery workitem
    WDF_WORKITEM_CONFIG_INIT(&workitemConfig, PtpFilterWorkItemCallback);
    WDF_OBJECT_ATTRIBUTES_INIT(&deviceAttributes);
    deviceAttributes.ParentObject = device;
    status = WdfWorkItemCreate(&workitemConfig, &deviceAttributes, &deviceContext->HidTransportRecoveryWorkItem);
    if (!NT_SUCCESS(status)) {
        KdPrint(("PtpFilterCreateDevice HidTransportRecoveryWorkItem failed, %x\n", status));  
    }

    // Set initial state
    deviceContext->VendorID = 0;
    deviceContext->ProductID = 0;
    deviceContext->VersionNumber = 0;
    deviceContext->DeviceConfigured = FALSE;

    // Initialize IO queue
    status = PtpFilterIoQueueInitialize(device);
    if (!NT_SUCCESS(status)) {
        KdPrint(("PtpFilterCreateDevice PtpFilterIoQueueInitialize failed, %x\n", status));
    }

exit:
    KdPrint(("PtpFilterCreateDevice end, %x\n", status));
    return status;
}

NTSTATUS
PtpFilterPrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourceList,
    _In_ WDFCMRESLIST ResourceListTranslated
)
{
    PDEVICE_CONTEXT deviceContext;
    NTSTATUS status = STATUS_SUCCESS;

    // We don't need to retrieve resources since this works as a filter now
    UNREFERENCED_PARAMETER(ResourceList);
    UNREFERENCED_PARAMETER(ResourceListTranslated);

    //PAGED_CODE();
    
    deviceContext = PtpFilterGetContext(Device);

    // Initialize IDs, set to zero
    deviceContext->VendorID = 0;
    deviceContext->ProductID = 0;
    deviceContext->VersionNumber = 0;
    deviceContext->DeviceConfigured = FALSE;

    RtlZeroMemory(&deviceContext->tp_settings, sizeof(PTP_PARSER));

    deviceContext->PtpInputModeOn = FALSE;

    deviceContext->bMouseLikeTouchPad_Mode = TRUE;//Ĭ�ϳ�ʼֵΪ����괥���������ʽ

    deviceContext->DeviceConfigured = FALSE;


    KdPrint(("PtpFilterPrepareHardware end, %x\n", status)); 
    return status;
}

NTSTATUS
PtpFilterDeviceD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState
)
{
    NTSTATUS status = STATUS_SUCCESS;

    //PAGED_CODE();
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(PreviousState);
    
    PDEVICE_CONTEXT pDevContext = PtpFilterGetContext(Device);

    //
    runtimes_IOCTL = 0;
    runtimes_IOREAD = 0;
    runtimes_SelfManagedIoInit = 0;

    if (pDevContext->DeviceConfigured) {
        init(pDevContext);//
    }

    KdPrint(("PtpFilterDeviceD0Entry end, %x\n", status));
    return status;
}

NTSTATUS
PtpFilterDeviceD0Exit(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState
)
{
    PDEVICE_CONTEXT deviceContext;
    NTSTATUS status = STATUS_SUCCESS;
    WDFREQUEST outstandingRequest;

    UNREFERENCED_PARAMETER(TargetState);

    //PAGED_CODE();
    
    deviceContext = PtpFilterGetContext(Device);

    // Reset device state
    deviceContext->DeviceConfigured = FALSE;

    // Cancelling all outstanding requests
    while (NT_SUCCESS(status)) {
        status = WdfIoQueueRetrieveNextRequest(
            deviceContext->HidReadQueue,
            &outstandingRequest
        );

        if (NT_SUCCESS(status)) {
            WdfRequestComplete(outstandingRequest, STATUS_CANCELLED);
        }
    }

    //����ע���������������ֵ
    status = SetRegisterMouseSensitivity(deviceContext, deviceContext->MouseSensitivity_Index);//MouseSensitivityTable�洢������ֵ
    if (!NT_SUCCESS(status))
    {
        KdPrint(("PtpFilterDeviceD0Exit SetRegisterMouseSensitivity err,%x\n", status));
    }

        //����ע���������������ֵ
    status = SetRegisterThumbScale(deviceContext, deviceContext->ThumbScale_Index);//ThumbScaleTable�洢������ֵ
    if (!NT_SUCCESS(status))
    {
        KdPrint(("PtpFilterDeviceD0Exit SetRegisterThumbScale err,%x\n", status));
    }


    KdPrint(("PtpFilterDeviceD0Exit end, %x\n", status));
    return STATUS_SUCCESS;
}

NTSTATUS
PtpFilterSelfManagedIoInit(
    _In_ WDFDEVICE Device
)
{
    NTSTATUS status;
    PDEVICE_CONTEXT deviceContext;
    WDF_MEMORY_DESCRIPTOR hidAttributeMemoryDescriptor;
    HID_DEVICE_ATTRIBUTES deviceAttributes;

    //PAGED_CODE();   

    runtimes_SelfManagedIoInit++;

    deviceContext = PtpFilterGetContext(Device);
    status = PtpFilterDetourWindowsHIDStack(Device);
    if (!NT_SUCCESS(status)) {
        KdPrint(("PtpFilterSelfManagedIoInit PtpFilterDetourWindowsHIDStack failed, %x\n", status));  
        goto exit;
    }

    // Request device attribute descriptor for self-identification.
    RtlZeroMemory(&deviceAttributes, sizeof(deviceAttributes));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(
        &hidAttributeMemoryDescriptor,
        (PVOID)&deviceAttributes,
        sizeof(deviceAttributes)
    );

    status = WdfIoTargetSendInternalIoctlSynchronously(
        deviceContext->HidIoTarget, NULL,
        IOCTL_HID_GET_DEVICE_ATTRIBUTES,
        NULL, &hidAttributeMemoryDescriptor, NULL, NULL);
    if (!NT_SUCCESS(status)) {
        KdPrint(("PtpFilterSelfManagedIoInit WdfIoTargetSendInternalIoctlSynchronously IOCTL_HID_GET_DEVICE_ATTRIBUTES failed, %x\n", status));
        goto exit;
    }

    deviceContext->VendorID = deviceAttributes.VendorID;
    deviceContext->ProductID = deviceAttributes.ProductID;
    deviceContext->VersionNumber = deviceAttributes.VersionNumber;

    KdPrint(("PtpFilterSelfManagedIoInit deviceAttributes.VendorID = %x, ProductID = %x, VersionNumber = %x, \n", \
            deviceContext->VendorID, deviceContext->ProductID, deviceContext->VersionNumber));


    WDF_MEMORY_DESCRIPTOR hidDescMemoryDescriptor;
    PHID_DESCRIPTOR pPtpHidDesc = &deviceContext->PtpHidDesc;
    RtlZeroMemory(pPtpHidDesc, sizeof(HID_DESCRIPTOR));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(
        &hidDescMemoryDescriptor,
        (PVOID)pPtpHidDesc,
        sizeof(HID_DESCRIPTOR)
    );

    status = WdfIoTargetSendInternalIoctlSynchronously(
        deviceContext->HidIoTarget, NULL,
        IOCTL_HID_GET_DEVICE_DESCRIPTOR,
        NULL, &hidDescMemoryDescriptor, NULL, NULL);
    if (!NT_SUCCESS(status)) {
        KdPrint(("PtpFilterSelfManagedIoInit WdfIoTargetSendInternalIoctlSynchronously IOCTL_HID_GET_DEVICE_DESCRIPTOR failed, %x\n", status));
        goto exit;
    }
    KdPrint(("PtpFilterSelfManagedIoInit pPtpHidDesc->DescriptorList[0].wReportLength = %x\n", pPtpHidDesc->DescriptorList[0].wReportLength));
    KdPrint(("PtpFilterSelfManagedIoInit pPtpHidDesc->bLength = %x\n", pPtpHidDesc->bLength));

    ULONG PtpHidReportDescLength = pPtpHidDesc->DescriptorList[0].wReportLength;
    if (PtpHidReportDescLength == 0) {
        KdPrint(("PtpFilterSelfManagedIoInit PtpHidReportDescLength err, %x\n", status));
        goto exit;
    }
    deviceContext->PtpHidReportDescLength = PtpHidReportDescLength;
    deviceContext->pPtpHidReportDesc = ExAllocatePool2(POOL_FLAG_NON_PAGED_EXECUTE, PtpHidReportDescLength, TPFILTER_POOL_TAG);//ExAllocatePoolWithTag(NonPagedPoolNx, PtpHidReportDescLength, TPFILTER_POOL_TAG);
    if (!deviceContext->pPtpHidReportDesc) {
        KdPrint(("PtpFilterSelfManagedIoInit ExAllocatePoolWithTag failed, %x\n", status));
        goto exit;
    }

    WDF_MEMORY_DESCRIPTOR HidReportDescMemoryDescriptor;
    RtlZeroMemory(deviceContext->pPtpHidReportDesc, PtpHidReportDescLength);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(
        &HidReportDescMemoryDescriptor,
        deviceContext->pPtpHidReportDesc,
        PtpHidReportDescLength
    );

    status = WdfIoTargetSendInternalIoctlSynchronously(
        deviceContext->HidIoTarget, NULL,
        IOCTL_HID_GET_REPORT_DESCRIPTOR,
        NULL, &HidReportDescMemoryDescriptor, NULL, NULL);
    if (!NT_SUCCESS(status)) {
        KdPrint(("PtpFilterSelfManagedIoInit WdfIoTargetSendInternalIoctlSynchronously IOCTL_HID_GET_REPORT_DESCRIPTOR failed, %x\n", status));
        goto exit;
    }
    KdPrint(("PtpFilterSelfManagedIoInit PtpHidReportDesc=\n"));
    for (UINT32 i = 0; i < PtpHidReportDescLength; i++) {
        KdPrint(("%x,", deviceContext->pPtpHidReportDesc[i]));
    }
    KdPrint(("\n"));
   
    status = AnalyzeHidReportDescriptor(deviceContext);
    if (!NT_SUCCESS(status)) {
        KdPrint(("AnalyzeHidReportDescriptor err,%x\n", runtimes_SelfManagedIoInit));
        return status;
    }

    init(deviceContext);//


    status = PtpFilterConfigureMultiTouch(Device);
    if (!NT_SUCCESS(status)) {
        // If this failed, we will retry after 2 seconds (and pretend nothing happens)
        KdPrint(("PtpFilterSelfManagedIoInit PtpFilterConfigureMultiTouch failed, %x\n", status));
        
        status = STATUS_SUCCESS;
        WdfTimerStart(deviceContext->HidTransportRecoveryTimer, WDF_REL_TIMEOUT_IN_SEC(2));
        goto exit;
    }

    // Set device state
    deviceContext->DeviceConfigured = TRUE;

exit:
    KdPrint(("PtpFilterSelfManagedIoInit end, %x\n", status)); 
    return status;
}

NTSTATUS
PtpFilterSelfManagedIoRestart(
    _In_ WDFDEVICE Device
)
{
    NTSTATUS status = STATUS_SUCCESS;
    PDEVICE_CONTEXT deviceContext;

    //PAGED_CODE();
    
    KdPrint(("PtpFilterSelfManagedIoRestart start, %x\n", status));

    deviceContext = PtpFilterGetContext(Device);

    // If this is first D0, it will be done in self-managed IO init.
    if (deviceContext->IsHidIoDetourCompleted) {
        status = PtpFilterConfigureMultiTouch(Device);
        if (!NT_SUCCESS(status)) {
            KdPrint(("PtpFilterSelfManagedIoRestart PtpFilterConfigureMultiTouch failed, %x\n", status));
            
            // If this failed, we will retry after 2 seconds (and pretend nothing happens)
            status = STATUS_SUCCESS;
            WdfTimerStart(deviceContext->HidTransportRecoveryTimer, WDF_REL_TIMEOUT_IN_SEC(2));
            goto exit;
        }
    }
    else {
        KdPrint(("PtpFilterSelfManagedIoRestart HID detour should already complete here, %x\n", status));
        
        status = STATUS_INVALID_STATE_TRANSITION;
    }

    // Set device state
    deviceContext->DeviceConfigured = TRUE;

exit:
    KdPrint(("PtpFilterSelfManagedIoRestart end, %x\n", status));
    return status;
}

NTSTATUS
PtpFilterConfigureMultiTouch(
    _In_ WDFDEVICE Device
)
{
    NTSTATUS status = STATUS_SUCCESS;
    PDEVICE_CONTEXT deviceContext;

    UCHAR hidPacketBuffer[HID_XFER_PACKET_SIZE];
    PHID_XFER_PACKET pHidPacket;
    WDFMEMORY hidMemory;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_REQUEST_SEND_OPTIONS configRequestSendOptions;
    WDFREQUEST configRequest;
    PIRP pConfigIrp = NULL;

    //PAGED_CODE();
    
    deviceContext = PtpFilterGetContext(Device);

    if (1) {
        return status;
    }


    RtlZeroMemory(hidPacketBuffer, sizeof(hidPacketBuffer));
    pHidPacket = (PHID_XFER_PACKET)&hidPacketBuffer;

    if (deviceContext->VendorID == HID_VID_APPLE_USB) {
        pHidPacket->reportId = 0x02;
        pHidPacket->reportBufferLen = 0x04;
        pHidPacket->reportBuffer = (PUCHAR)pHidPacket + sizeof(HID_XFER_PACKET);
        pHidPacket->reportBuffer[0] = 0x02;
        pHidPacket->reportBuffer[1] = 0x01;
        pHidPacket->reportBuffer[2] = 0x00;
        pHidPacket->reportBuffer[3] = 0x00;
    }
    else if (deviceContext->VendorID == HID_VID_APPLE_BT) {

        pHidPacket->reportId = 0xF1;
        pHidPacket->reportBufferLen = 0x03;
        pHidPacket->reportBuffer = (PUCHAR)pHidPacket + sizeof(HID_XFER_PACKET);
        pHidPacket->reportBuffer[0] = 0xF1;
        pHidPacket->reportBuffer[1] = 0x02;
        pHidPacket->reportBuffer[2] = 0x01;
    }
    else {
        // Something we don't support yet.
        
        status = STATUS_NOT_SUPPORTED;
        goto exit;
    }

    // Init a request entity.
    // Because we bypassed HIDCLASS driver, there's a few things that we need to manually take care of.
    status = WdfRequestCreate(WDF_NO_OBJECT_ATTRIBUTES, deviceContext->HidIoTarget, &configRequest);
    if (!NT_SUCCESS(status)) { 
        goto exit;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = configRequest;
    status = WdfMemoryCreatePreallocated(&attributes, (PVOID)pHidPacket, HID_XFER_PACKET_SIZE, &hidMemory);
    if (!NT_SUCCESS(status)) { 
        goto cleanup;
    }

    status = WdfIoTargetFormatRequestForInternalIoctl(deviceContext->HidIoTarget,
                                                      configRequest, IOCTL_HID_SET_FEATURE,
                                                      hidMemory, NULL, NULL, NULL);
    if (!NT_SUCCESS(status)) {
        goto cleanup;
    }

    // Manually take care of IRP to meet requirements of mini drivers.
    pConfigIrp = WdfRequestWdmGetIrp(configRequest);
    if (pConfigIrp == NULL) {
        
        status = STATUS_UNSUCCESSFUL;
        goto cleanup;
    }

    // God-damn-it we have to configure it by ourselves :)
    pConfigIrp->UserBuffer = pHidPacket;

    WDF_REQUEST_SEND_OPTIONS_INIT(&configRequestSendOptions, WDF_REQUEST_SEND_OPTION_SYNCHRONOUS);
    if (WdfRequestSend(configRequest, deviceContext->HidIoTarget, &configRequestSendOptions) == FALSE) {
        status = WdfRequestGetStatus(configRequest);
        goto cleanup;
    }
    else {
        status = STATUS_SUCCESS;
    }

cleanup:
    if (configRequest != NULL) {
        WdfObjectDelete(configRequest);
    }
exit:
    
    return status;
}

VOID
PtpFilterRecoveryTimerCallback(
    WDFTIMER Timer
)
{
    WDFDEVICE device;
    PDEVICE_CONTEXT deviceContext;
    NTSTATUS status;

    device = WdfTimerGetParentObject(Timer);
    deviceContext = PtpFilterGetContext(device);

    // We will try to reinitialize the device
    status = PtpFilterSelfManagedIoRestart(device);
    if (NT_SUCCESS(status)) {
        // If succeeded, proceed to reissue the request.
        // Otherwise it will retry the process after a few seconds.
        PtpFilterInputIssueTransportRequest(device);
    }
}



NTSTATUS
PtpFilterDetourWindowsHIDStack(
    _In_ WDFDEVICE Device
)
{
    NTSTATUS status = STATUS_SUCCESS;
    PDEVICE_CONTEXT deviceContext;
    PDEVICE_OBJECT  hidTransportWdmDeviceObject = NULL;
    PDRIVER_OBJECT  hidTransportWdmDriverObject = NULL;
    PIO_CLIENT_EXTENSION hidTransportIoClientExtension = NULL;
    PHIDCLASS_DRIVER_EXTENSION hidTransportClassExtension = NULL;

    //PAGED_CODE();
    
    deviceContext = PtpFilterGetContext(Device);

    if (deviceContext->WdmDeviceObject == NULL || deviceContext->WdmDeviceObject->DriverObject == NULL) {
        status = STATUS_UNSUCCESSFUL;
        goto exit;
    }

    KdPrint(("PtpFilterDetourWindowsHIDStack WdmDeviceObject= , %S\n", deviceContext->WdmDeviceObject->DriverObject->DriverName.Buffer));

    // Access the driver object to find next low-level device (in our case, we expect it to be HID transport driver)
    hidTransportWdmDeviceObject = IoGetLowerDeviceObject(deviceContext->WdmDeviceObject);
    if (hidTransportWdmDeviceObject == NULL) {
        status = STATUS_UNSUCCESSFUL;
        goto exit;
    }

    
    hidTransportWdmDriverObject = hidTransportWdmDeviceObject->DriverObject;
    if (hidTransportWdmDriverObject == NULL) {
        status = STATUS_UNSUCCESSFUL;
        goto cleanup;
    }

    KdPrint(("PtpFilterDetourWindowsHIDStack hidTransportWdmDeviceObject= , %S\n", hidTransportWdmDeviceObject->DriverObject->DriverName.Buffer));


    // Verify if the driver extension is what we expected.
    if (hidTransportWdmDriverObject->DriverExtension == NULL) {
        status = STATUS_UNSUCCESSFUL;
        goto cleanup;
    }

    KdPrint(("PtpFilterDetourWindowsHIDStack hidTransportWdmDeviceObject->DriverExtension ok\n"));

    // Just two more check...
    hidTransportIoClientExtension = ((PDRIVER_EXTENSION_EXT)hidTransportWdmDriverObject->DriverExtension)->IoClientExtension;
    if (hidTransportIoClientExtension == NULL) {
        status = STATUS_UNSUCCESSFUL;
        goto cleanup;
    }
    KdPrint(("PtpFilterDetourWindowsHIDStack hidTransportIoClientExtension ok\n"));

    if (strncmp(HID_CLASS_EXTENSION_LITERAL_ID, hidTransportIoClientExtension->ClientIdentificationAddress, sizeof(HID_CLASS_EXTENSION_LITERAL_ID)) != 0) {
        status = STATUS_UNSUCCESSFUL;
        goto cleanup;
    }

    KdPrint(("PtpFilterDetourWindowsHIDStack strncmp ok\n"));

    hidTransportClassExtension = (PHIDCLASS_DRIVER_EXTENSION)(hidTransportIoClientExtension + 1);
    

    // HIDClass overrides:
    // IRP_MJ_SYSTEM_CONTROL, IRP_MJ_WRITE, IRP_MJ_READ, IRP_MJ_POWER, IRP_MJ_PNP, IRP_MJ_INTERNAL_DEVICE_CONTROL, IRP_MJ_DEVICE_CONTROL
    // IRP_MJ_CREATE, IRP_MJ_CLOSE
    // For us, overriding IRP_MJ_DEVICE_CONTROL and IRP_MJ_INTERNAL_DEVICE_CONTROL might be sufficient.
    // Details: https://ligstd.visualstudio.com/Apple%20PTP%20Trackpad/_wiki/wikis/Apple-PTP-Trackpad.wiki/47/Hijack-HIDCLASS
    hidTransportWdmDriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = hidTransportClassExtension->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL];
    

    // Mark detour as completed.
    deviceContext->IsHidIoDetourCompleted = TRUE;
    deviceContext->HidIoTarget = WdfDeviceGetIoTarget(Device);

cleanup:
    if (hidTransportWdmDeviceObject != NULL) {
        ObDereferenceObject(hidTransportWdmDeviceObject);
    }
exit:
    
    return status;
}



NTSTATUS
PtpFilterIoQueueInitialize(
    _In_ WDFDEVICE Device
)
{
    WDFQUEUE queue;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_OBJECT_ATTRIBUTES queueAttributes;
    PDEVICE_CONTEXT deviceContext;
    PQUEUE_CONTEXT queueContext;
    NTSTATUS status;

    //PAGED_CODE();
    
    deviceContext = PtpFilterGetContext(Device);

    // First queue for system-wide HID controls
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&queueAttributes, QUEUE_CONTEXT);
    queueConfig.EvtIoInternalDeviceControl = FilterEvtIoIntDeviceControl;
    queueConfig.EvtIoStop = FilterEvtIoStop;
    status = WdfIoQueueCreate(Device, &queueConfig, &queueAttributes, &queue);
    if (!NT_SUCCESS(status))
    {
        KdPrint(("PtpFilterIoQueueInitialize WdfIoQueueCreate failed, %x\n", status));
        goto exit;
    }

    queueContext = PtpFilterQueueGetContext(queue);
    queueContext->Device = deviceContext->Device;
    queueContext->DeviceIoTarget = deviceContext->HidIoTarget;

    // Second queue for HID read requests
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    queueConfig.PowerManaged = WdfFalse;
    status = WdfIoQueueCreate(Device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &deviceContext->HidReadQueue);
    if (!NT_SUCCESS(status)) {
        KdPrint(("PtpFilterIoQueueInitialize WdfIoQueueCreate Input failed, %x\n", status));
        
    }

exit:
    KdPrint(("PtpFilterIoQueueInitialize end,%x\n", status));
    return status;
}

VOID
FilterEvtIoIntDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
)
{
    PQUEUE_CONTEXT queueContext;
    PDEVICE_CONTEXT deviceContext;
    BOOLEAN requestPending = FALSE;
    NTSTATUS status = STATUS_UNSUCCESSFUL;

    UNREFERENCED_PARAMETER(InputBufferLength);
    UNREFERENCED_PARAMETER(OutputBufferLength);

    queueContext = PtpFilterQueueGetContext(Queue);
    deviceContext = PtpFilterGetContext(queueContext->Device);
    //WDFIOTARGET ioTarget = WdfDeviceGetIoTarget(queueContext->Device);

    runtimes_IOCTL++;
    KdPrint(("FilterEvtIoIntDeviceControl runtimes_IOCTL,%x\n", runtimes_IOCTL));


    switch (IoControlCode)
    {
        case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
            KdPrint(("FilterEvtIoIntDeviceControl IOCTL_HID_GET_DEVICE_DESCRIPTOR,%x\n", runtimes_IOCTL));
            status = PtpFilterGetHidDescriptor(queueContext->Device, Request);
            break;
        case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
            KdPrint(("FilterEvtIoIntDeviceControl IOCTL_HID_GET_DEVICE_ATTRIBUTES,%x\n", runtimes_IOCTL));
            status = PtpFilterGetDeviceAttribs(queueContext->Device, Request);
            break;
        case IOCTL_HID_GET_REPORT_DESCRIPTOR:
            KdPrint(("FilterEvtIoIntDeviceControl IOCTL_HID_GET_REPORT_DESCRIPTOR,%x\n", runtimes_IOCTL));
            status = PtpFilterGetReportDescriptor(queueContext->Device, Request);
            break;
        case IOCTL_HID_GET_STRING:
            KdPrint(("FilterEvtIoIntDeviceControl IOCTL_HID_GET_STRING,%x\n", runtimes_IOCTL));
            status = PtpFilterGetStrings(queueContext->Device, Request, &requestPending);
            break;
        case IOCTL_HID_READ_REPORT:
            ++runtimes_IOREAD;
            if (runtimes_IOREAD == 1) {
                KdPrint(("FilterEvtIoIntDeviceControl IOCTL_HID_READ_REPORT,%x\n", runtimes_IOREAD));
            }
            PtpFilterInputProcessRequest(queueContext->Device, Request);
            requestPending = TRUE;
            break;
        case IOCTL_HID_GET_FEATURE:
            KdPrint(("FilterEvtIoIntDeviceControl IOCTL_HID_GET_FEATURE,%x\n", runtimes_IOCTL));
            //status = Filter_DispatchPassThrough(queueContext->Device, Request, &requestPending);

            status = PtpFilterGetHidFeatures(queueContext->Device, Request);
            break;
        case IOCTL_HID_SET_FEATURE:
            KdPrint(("FilterEvtIoIntDeviceControl IOCTL_HID_SET_FEATURE,%x\n", runtimes_IOCTL));
            //status = Filter_DispatchPassThrough(queueContext->Device, Request, &requestPending);

            status = PtpFilterSetHidFeatures(queueContext->Device, Request);
            status = Filter_DispatchPassThrough(queueContext->Device, Request, &requestPending);
            break;
        case IOCTL_HID_WRITE_REPORT:
        case IOCTL_UMDF_HID_SET_OUTPUT_REPORT:
        case IOCTL_UMDF_HID_GET_INPUT_REPORT:
        case IOCTL_HID_ACTIVATE_DEVICE:
        case IOCTL_HID_DEACTIVATE_DEVICE:
        case IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST:
        default:
            status = STATUS_NOT_SUPPORTED;
            KdPrint(("FilterEvtIoIntDeviceControl STATUS_NOT_SUPPORTED,%x\n", runtimes_IOCTL));
            break;
    }

    if (!requestPending)
    {
        KdPrint(("FilterEvtIoIntDeviceControl Status,%x\n", status));
        WdfRequestComplete(Request, status);
    }
}

VOID
FilterEvtIoStop(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ ULONG ActionFlags
)
{
    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(ActionFlags);

    KdPrint(("FilterEvtIoStop end,%x\n", 0));
}



NTSTATUS
PtpFilterGetHidDescriptor(
	_In_ WDFDEVICE Device,
	_In_ WDFREQUEST Request
)
{

	NTSTATUS        status = STATUS_SUCCESS;
	PDEVICE_CONTEXT deviceContext;
	size_t			hidDescriptorSize = 0;
	WDFMEMORY       requestMemory;
	
	deviceContext = PtpFilterGetContext(Device);

	status = WdfRequestRetrieveOutputMemory(Request, &requestMemory);
	if (!NT_SUCCESS(status)) {
		goto exit;
	}

	hidDescriptorSize = DefaultHidDescriptor.bLength; //deviceContext->PtpHidDesc.bLength�������ӵ����������ȴ�����Ҫ�ñ�׼��DefaultHidDescriptor.bLength;
    PVOID pHidDesc = (PVOID) &DefaultHidDescriptor;//&DefaultHidDescriptor//&deviceContext->PtpHidDesc;
    
	status = WdfMemoryCopyFromBuffer(requestMemory, 0, pHidDesc, hidDescriptorSize);
	if (!NT_SUCCESS(status)) {
		KdPrint(("PtpFilterGetHidDescriptor WdfMemoryCopyFromBuffer err,%x\n", status));
		goto exit;
	}

	WdfRequestSetInformation(Request, hidDescriptorSize);

exit:
	KdPrint(("PtpFilterGetHidDescriptor end,%x\n", status));
	
	return status;
}

NTSTATUS
PtpFilterGetDeviceAttribs(
	_In_ WDFDEVICE Device,
	_In_ WDFREQUEST Request
)
{

	NTSTATUS               status = STATUS_SUCCESS;
	PDEVICE_CONTEXT        deviceContext;
	PHID_DEVICE_ATTRIBUTES pDeviceAttributes = NULL;
	
	deviceContext = PtpFilterGetContext(Device);

	status = WdfRequestRetrieveOutputBuffer(Request, sizeof(HID_DEVICE_ATTRIBUTES), &pDeviceAttributes, NULL);
	if (!NT_SUCCESS(status)) {
		KdPrint(("PtpFilterGetDeviceAttribs WdfRequestRetrieveOutputBuffer err,%x\n", status));
		goto exit;
	}

	pDeviceAttributes->Size = sizeof(HID_DEVICE_ATTRIBUTES);
	// Okay here's one thing: we cannot report the real ID here, otherwise there's will be some great conflict with the USB/BT driver.
	// Therefore Vendor ID is changed to a hardcoded number
	pDeviceAttributes->ProductID = deviceContext->ProductID;
    pDeviceAttributes->VendorID = 0;//deviceContext->VendorID;��ֵ0ʹ�ñ�׼�������豸��Ӳ��id�������뵽���������ɵı�׼�������豸id�У���ֹ��ΰ�װ����Ƕ������ 
	pDeviceAttributes->VersionNumber = deviceContext->VersionNumber;
	WdfRequestSetInformation(Request, sizeof(HID_DEVICE_ATTRIBUTES));

exit:
	KdPrint(("PtpFilterGetDeviceAttribs end,%x\n", status));
	return status;
}

NTSTATUS
PtpFilterGetReportDescriptor(
	_In_ WDFDEVICE Device,
	_In_ WDFREQUEST Request
)
{

	NTSTATUS               status = STATUS_SUCCESS;
	PDEVICE_CONTEXT        deviceContext;
	size_t			       hidDescriptorSize = 0;
	WDFMEMORY              requestMemory;

	KdPrint(("PtpFilterGetReportDescriptor start,%x\n", 0));
	
	deviceContext = PtpFilterGetContext(Device);

	status = WdfRequestRetrieveOutputMemory(Request, &requestMemory);
	if (!NT_SUCCESS(status)) {
		KdPrint(("PtpFilterGetReportDescriptor WdfRequestRetrieveOutputBuffer err,%x\n", status));
		goto exit;
	}

    hidDescriptorSize = DefaultHidDescriptor.DescriptorList[0].wReportLength; // deviceContext->PtpHidDesc.DescriptorList[0].wReportLength;
    PVOID pHidReportDesc = (PVOID) ParallelMode_PtpReportDescriptor;// deviceContext->pPtpHidReportDesc;//(PVOID) ParallelMode_PtpReportDescriptor;ע�ⲻ��ȡ��ַ

	status = WdfMemoryCopyFromBuffer(requestMemory, 0, pHidReportDesc, hidDescriptorSize);
	if (!NT_SUCCESS(status)) {
        KdPrint(("PtpFilterGetReportDescriptor WdfMemoryCopyFromBuffer failed,%x\n", status));
		goto exit;
	}

	WdfRequestSetInformation(Request, hidDescriptorSize);

exit:
	KdPrint(("PtpFilterGetReportDescriptor end,%x\n", status));
	return status;
}

NTSTATUS
PtpFilterGetStrings(
	_In_ WDFDEVICE Device,
	_In_ WDFREQUEST Request,
	_Out_ BOOLEAN* Pending
)
{
	NTSTATUS status = STATUS_SUCCESS;
	PDEVICE_CONTEXT deviceContext;
	BOOLEAN requestSent;
	WDF_REQUEST_SEND_OPTIONS sendOptions;

    KdPrint(("PtpFilterGetStrings start,%x\n", status));
	
	deviceContext = PtpFilterGetContext(Device);

	// Forward the IRP to our upstream IO target
	// We don't really care about the content
	WdfRequestFormatRequestUsingCurrentType(Request);
	WDF_REQUEST_SEND_OPTIONS_INIT(&sendOptions, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);

	// This IOCTL is METHOD_NEITHER, so we just send it without IRP modification
	requestSent = WdfRequestSend(Request, deviceContext->HidIoTarget, &sendOptions);
	*Pending = TRUE;

	if (!requestSent)
	{
		status = WdfRequestGetStatus(Request);
		*Pending = FALSE;
	}

    KdPrint(("PtpFilterGetStrings end,%x\n", status));
	return status;
}

NTSTATUS
PtpFilterGetHidFeatures(
	_In_ WDFDEVICE Device,
	_In_ WDFREQUEST Request
)
{
	NTSTATUS status = STATUS_SUCCESS;
	PDEVICE_CONTEXT deviceContext;

	WDF_REQUEST_PARAMETERS requestParameters;
	size_t reportSize;
	PHID_XFER_PACKET hidContent;
	
	deviceContext = PtpFilterGetContext(Device);

	WDF_REQUEST_PARAMETERS_INIT(&requestParameters);
	WdfRequestGetParameters(Request, &requestParameters);
	if (requestParameters.Parameters.DeviceIoControl.OutputBufferLength < sizeof(HID_XFER_PACKET))
	{
        KdPrint(("PtpFilterGetHidFeatures STATUS_BUFFER_TOO_SMALL,%x\n", status));
		status = STATUS_BUFFER_TOO_SMALL;
		goto exit;
	}

	hidContent = (PHID_XFER_PACKET)WdfRequestWdmGetIrp(Request)->UserBuffer;
	if (hidContent == NULL)
	{
        KdPrint(("PtpFilterGetHidFeatures STATUS_INVALID_DEVICE_REQUEST,%x\n", status));
		status = STATUS_INVALID_DEVICE_REQUEST;
		goto exit;
	}

	switch (hidContent->reportId)
	{
		case FAKE_REPORTID_DEVICE_CAPS:
		{
			// Size sanity check
			reportSize = sizeof(PTP_DEVICE_CAPS_FEATURE_REPORT);
			if (hidContent->reportBufferLen < reportSize) {
				status = STATUS_INVALID_BUFFER_SIZE;
                KdPrint(("PtpFilterGetHidFeatures STATUS_INVALID_BUFFER_SIZE,%x\n", status));
				goto exit;
			}

			PPTP_DEVICE_CAPS_FEATURE_REPORT capsReport = (PPTP_DEVICE_CAPS_FEATURE_REPORT)hidContent->reportBuffer;
			capsReport->MaximumContactPoints = PTP_MAX_CONTACT_POINTS;
			capsReport->ButtonType = PTP_BUTTON_TYPE_CLICK_PAD;
			capsReport->ReportID = FAKE_REPORTID_DEVICE_CAPS;

            KdPrint(("PtpFilterGetHidFeatures FAKE_REPORTID_DEVICE_CAPS,%x\n", status));
	
			break;
		}
		case FAKE_REPORTID_PTPHQA:
		{
            KdPrint(("PtpFilterGetHidFeatures FAKE_REPORTID_PTPHQA,%x\n", status));
			
			// Size sanity check
			reportSize = sizeof(PTP_DEVICE_HQA_CERTIFICATION_REPORT);
			if (hidContent->reportBufferLen < reportSize)
			{
				status = STATUS_INVALID_BUFFER_SIZE;
                KdPrint(("PtpFilterGetHidFeatures FAKE_REPORTID_PTPHQA STATUS_INVALID_BUFFER_SIZE,%x\n", status));
				goto exit;
			}

			PPTP_DEVICE_HQA_CERTIFICATION_REPORT certReport = (PPTP_DEVICE_HQA_CERTIFICATION_REPORT)hidContent->reportBuffer;
			*certReport->CertificationBlob = DEFAULT_PTP_HQA_BLOB;
			certReport->ReportID = FAKE_REPORTID_PTPHQA;
            deviceContext->PtpInputModeOn = TRUE;

			break;
		}
		default:
		{
            KdPrint(("PtpFilterGetHidFeatures STATUS_NOT_SUPPORTED,%x\n", status));
			status = STATUS_NOT_SUPPORTED;
			goto exit;
		}
	}

exit:
    KdPrint(("PtpFilterGetHidFeatures end,%x\n", status));
	return status;
}

NTSTATUS
PtpFilterSetHidFeatures(
	_In_ WDFDEVICE Device,
	_In_ WDFREQUEST Request
)
{
	NTSTATUS status = STATUS_SUCCESS;
	PDEVICE_CONTEXT deviceContext;

	PHID_XFER_PACKET hidPacket;
	WDF_REQUEST_PARAMETERS requestParameters;

    KdPrint(("PtpFilterSetHidFeatures start,%x\n", status));
	
	deviceContext = PtpFilterGetContext(Device);

	WDF_REQUEST_PARAMETERS_INIT(&requestParameters);
	WdfRequestGetParameters(Request, &requestParameters);
	if (requestParameters.Parameters.DeviceIoControl.InputBufferLength < sizeof(HID_XFER_PACKET))
	{
        KdPrint(("PtpFilterSetHidFeatures STATUS_BUFFER_TOO_SMALL,%x\n", status));
		status = STATUS_BUFFER_TOO_SMALL;
		goto exit;
	}

	hidPacket = (PHID_XFER_PACKET)WdfRequestWdmGetIrp(Request)->UserBuffer;
	if (hidPacket == NULL)
	{
		status = STATUS_INVALID_DEVICE_REQUEST;
        KdPrint(("PtpFilterSetHidFeatures STATUS_INVALID_DEVICE_REQUEST,%x\n", status));
		goto exit;
	}

	switch (hidPacket->reportId)
	{
		case FAKE_REPORTID_INPUTMODE:
		{
            KdPrint(("PtpFilterSetHidFeatures FAKE_REPORTID_INPUTMODE,%x\n", status));
            hidPacket->reportId = deviceContext->REPORTID_INPUT_MODE;//hidPacket��DeviceInputMode��Ҫ���Ʋ���Ч

			PPTP_DEVICE_INPUT_MODE_REPORT DeviceInputMode = (PPTP_DEVICE_INPUT_MODE_REPORT)hidPacket->reportBuffer;
            DeviceInputMode->ReportID = deviceContext->REPORTID_INPUT_MODE;//hidPacket��DeviceInputMode��Ҫ���Ʋ���Ч

			switch (DeviceInputMode->Mode)
			{
				case PTP_COLLECTION_MOUSE:
				{
                    KdPrint(("PtpFilterSetHidFeatures PTP_COLLECTION_MOUSE,%x\n", status));
					
					deviceContext->PtpInputModeOn = FALSE;
					break;
				}
				case PTP_COLLECTION_WINDOWS:
				{
                    KdPrint(("PtpFilterSetHidFeatures PTP_COLLECTION_WINDOWS,%x\n", status));
					
					deviceContext->PtpInputModeOn = TRUE;
					break;
				}
			}
			break;
		}
		case FAKE_REPORTID_FUNCTION_SWITCH:
		{
            KdPrint(("PtpFilterSetHidFeatures FAKE_REPORTID_FUNCTION_SWITCH,%x\n", status));
            hidPacket->reportId = deviceContext->REPORTID_FUNCTION_SWITCH;//hidPacket��InputSelection��Ҫ���Ʋ���Ч

			PPTP_DEVICE_SELECTIVE_REPORT_MODE_REPORT InputSelection = (PPTP_DEVICE_SELECTIVE_REPORT_MODE_REPORT)hidPacket->reportBuffer;
            InputSelection->ReportID = deviceContext->REPORTID_FUNCTION_SWITCH;//hidPacket��InputSelection��Ҫ���Ʋ���Ч
            InputSelection->ButtonReport = 0x1;//
            InputSelection->SurfaceReport = 0x1;//
			deviceContext->PtpReportButton = InputSelection->ButtonReport;
			deviceContext->PtpReportTouch = InputSelection->SurfaceReport;

			break;
		}
        case FAKE_REPORTID_LATENCY_MODE:
        {
            KdPrint(("PtpFilterSetHidFeatures FAKE_REPORTID_LATENCY_MODE,%x\n", status));
            hidPacket->reportId = deviceContext->REPORTID_LATENCY_MODE;//hidPacket��latency_mode��Ҫ���Ʋ���Ч

            PPTP_DEVICE_LATENCY_MODE_REPORT latency_mode = (PPTP_DEVICE_LATENCY_MODE_REPORT)hidPacket->reportBuffer;
            latency_mode->ReportID = deviceContext->REPORTID_LATENCY_MODE;//hidPacket��latency_mode��Ҫ���Ʋ���Ч
            latency_mode->Mode = 0;//

            break;
        }
		default:
		{
            KdPrint(("PtpFilterSetHidFeatures STATUS_NOT_SUPPORTED,%x\n", status));
			
			status = STATUS_NOT_SUPPORTED;
			goto exit;
		}
	}

exit:

    KdPrint(("PtpFilterSetHidFeatures end,%x\n", status));
	return status;
}



VOID
PtpFilterInputProcessRequest(
	_In_ WDFDEVICE Device,
	_In_ WDFREQUEST Request
)
{
	NTSTATUS status;
	PDEVICE_CONTEXT deviceContext;

	deviceContext = PtpFilterGetContext(Device);
	status = WdfRequestForwardToIoQueue(Request, deviceContext->HidReadQueue);
	if (!NT_SUCCESS(status)) {
        KdPrint(("PtpFilterInputProcessRequest WdfRequestForwardToIoQueue failed,%x\n", status));
		WdfRequestComplete(Request, status);
		return;
	}

	// Only issue request when fully configured.
	// Otherwise we will let power recovery process to triage it
	if (deviceContext->DeviceConfigured == TRUE) {
		PtpFilterInputIssueTransportRequest(Device);
        KdPrint(("PtpFilterInputProcessRequest ok,%x\n", runtimes_IOREAD));
	}

    KdPrint(("PtpFilterInputProcessRequest end,%x\n", runtimes_IOREAD));
}


VOID
PtpFilterWorkItemCallback(
	_In_ WDFWORKITEM WorkItem
)
{
	WDFDEVICE Device = WdfWorkItemGetParentObject(WorkItem);
	PtpFilterInputIssueTransportRequest(Device);
}


VOID
PtpFilterInputIssueTransportRequest(
	_In_ WDFDEVICE Device
)
{
	NTSTATUS status;
	PDEVICE_CONTEXT deviceContext;

	WDF_OBJECT_ATTRIBUTES attributes;
	WDFREQUEST hidReadRequest;
	WDFMEMORY hidReadOutputMemory;
	PWORKER_REQUEST_CONTEXT requestContext;
	BOOLEAN requestStatus = FALSE;

	deviceContext = PtpFilterGetContext(Device);

    KdPrint(("PtpFilterInputIssueTransportRequest start,%x\n", runtimes_IOREAD));

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, WORKER_REQUEST_CONTEXT);
	attributes.ParentObject = Device;
	status = WdfRequestCreate(&attributes, deviceContext->HidIoTarget, &hidReadRequest);
	if (!NT_SUCCESS(status)) {
		// This can fail for Bluetooth devices. We will set up a 3 second timer for retry triage.
		// Typically this should not fail for USB transport.
		
        KdPrint(("PtpFilterInputIssueTransportRequest WdfRequestCreate failed,%x\n", status));
		deviceContext->DeviceConfigured = FALSE;
		WdfTimerStart(deviceContext->HidTransportRecoveryTimer, WDF_REL_TIMEOUT_IN_SEC(3));
		return;
	}

	status = WdfMemoryCreateFromLookaside(deviceContext->HidReadBufferLookaside, &hidReadOutputMemory);
	if (!NT_SUCCESS(status)) {
		// tbh if you fail here, something seriously went wrong...request a restart.
        KdPrint(("PtpFilterInputIssueTransportRequest WdfMemoryCreateFromLookaside failed,%x\n", status));
		
		WdfObjectDelete(hidReadRequest);
		WdfDeviceSetFailed(deviceContext->Device, WdfDeviceFailedAttemptRestart);
		return;
	}

	// Assign context information
	// And format HID read request.
	requestContext = WorkerRequestGetContext(hidReadRequest);
	requestContext->DeviceContext = deviceContext;
	requestContext->RequestMemory = hidReadOutputMemory;
	status = WdfIoTargetFormatRequestForInternalIoctl(deviceContext->HidIoTarget, hidReadRequest,
													  IOCTL_HID_READ_REPORT, NULL, 0, hidReadOutputMemory, 0);
	if (!NT_SUCCESS(status)) {
		// tbh if you fail here, something seriously went wrong...request a restart.
        KdPrint(("PtpFilterInputIssueTransportRequest WdfIoTargetFormatRequestForInternalIoctl failed,%x\n", status));

		if (hidReadOutputMemory != NULL) {
			WdfObjectDelete(hidReadOutputMemory);
		}

		if (hidReadRequest != NULL) {
			WdfObjectDelete(hidReadRequest);
		}

		WdfDeviceSetFailed(deviceContext->Device, WdfDeviceFailedAttemptRestart);
		return;
	}

	// Set callback
	WdfRequestSetCompletionRoutine(hidReadRequest, PtpFilterInputRequestCompletionCallback, requestContext);

	requestStatus = WdfRequestSend(hidReadRequest, deviceContext->HidIoTarget, NULL);
	if (!requestStatus) {
		// Retry after 3 seconds, in case this is a transportation issue.
        KdPrint(("PtpFilterInputIssueTransportRequest WdfRequestSend failed,%x\n", status));

		deviceContext->DeviceConfigured = FALSE;
		WdfTimerStart(deviceContext->HidTransportRecoveryTimer, WDF_REL_TIMEOUT_IN_SEC(3));

		if (hidReadOutputMemory != NULL) {
			WdfObjectDelete(hidReadOutputMemory);
		}

		if (hidReadRequest != NULL) {
			WdfObjectDelete(hidReadRequest);
		}
	}

    KdPrint(("PtpFilterInputIssueTransportRequest end,%x\n", runtimes_IOREAD));
}

VOID
PtpFilterInputRequestCompletionCallback(
	_In_ WDFREQUEST Request,
	_In_ WDFIOTARGET Target,
	_In_ PWDF_REQUEST_COMPLETION_PARAMS Params,
	_In_ WDFCONTEXT Context
)
{
	PWORKER_REQUEST_CONTEXT requestContext;
	PDEVICE_CONTEXT deviceContext;
	NTSTATUS status = STATUS_SUCCESS;

    PUCHAR pOutputReport;
    PTP_REPORT OutputReport;

	size_t responseLength;
	PUCHAR TouchDataBuffer;

    size_t InputSize = 0;

	UNREFERENCED_PARAMETER(Target);

	requestContext = (PWORKER_REQUEST_CONTEXT)Context;
	deviceContext = requestContext->DeviceContext;
	responseLength = (size_t)(LONG)WdfRequestGetInformation(Request);
	TouchDataBuffer = WdfMemoryGetBuffer(Params->Parameters.Ioctl.Output.Buffer, NULL);
    pOutputReport = TouchDataBuffer;
    size_t OutputSize = sizeof(PTP_REPORT);

    RtlZeroMemory(&OutputReport, OutputSize);
    KdPrint(("\n PtpFilterInputRequestCompletionCallback start,%x\n", runtimes_IOREAD));

	// Pre-flight check 1: if size is 0, this is not something we need. Ignore the read, and issue next request.
	if (responseLength <= 0) {
		WdfWorkItemEnqueue(requestContext->DeviceContext->HidTransportRecoveryWorkItem);
        KdPrint(("PtpFilterInputRequestCompletionCallback responseLength<0,%x\n",runtimes_IOREAD));
		goto cleanup;
	}

    KdPrint(("PtpFilterInputRequestCompletionCallback responseLength=,%x\n", (ULONG)responseLength));
    for (UINT32 i = 0; i < responseLength; i++) {
        KdPrint(("PtpFilterInputRequestCompletionCallback TouchDataBuffer[%x]=,%x\n", i,TouchDataBuffer[i]));
    }

    if (deviceContext->VendorID == 0x6cb) {//synaptic�������豸vendorID��lenovo yoga 14s 2021 laptops I2C HID
        InputSize = sizeof(PTP_REPORT);
        KdPrint(("PtpFilterInputRequestCompletionCallback VendorID=,%x\n", deviceContext->VendorID));

        if (responseLength == InputSize) {
            PTP_REPORT InputReport = *(PTP_REPORT*)TouchDataBuffer;
            KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.ReportID =,%x\n", InputReport.ReportID));
            KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.ContactCount =,%x\n", InputReport.ContactCount));
            KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.IsButtonClicked =,%x\n", InputReport.IsButtonClicked));
            KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.ScanTime =,%x\n", InputReport.ScanTime));
            KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.Contacts[0].Confidence =,%x\n", InputReport.Contacts[0].Confidence));
            KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.Contacts[0].TipSwitch =,%x\n", InputReport.Contacts[0].TipSwitch));
            KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.Contacts[0].ContactID =,%x\n", InputReport.Contacts[0].ContactID));
            KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.Contacts[0].X =,%x\n", InputReport.Contacts[0].X));
            KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.Contacts[0].Y =,%x\n", InputReport.Contacts[0].Y));

            RtlCopyMemory(&OutputReport, &InputReport, InputSize);
            OutputReport.ReportID = FAKE_REPORTID_MULTITOUCH;
            pOutputReport = (PUCHAR)&OutputReport;

        }
        
    }
    else if (deviceContext->VendorID == 0x17ef) {//lenovo Duet BT Folio
        InputSize = sizeof(PTP_REPORT_DUET);
        KdPrint(("PtpFilterInputRequestCompletionCallback VendorID=,%x\n", deviceContext->VendorID));

        if (responseLength == InputSize) {
            PTP_REPORT_DUET InputReport = *(PTP_REPORT_DUET*)TouchDataBuffer;
            KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.ReportID =,%x\n", InputReport.ReportID));
            KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.ContactCount =,%x\n", InputReport.ContactCount));
            KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.IsButtonClicked =,%x\n", InputReport.IsButtonClicked));
            KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.ScanTime =,%x\n", InputReport.ScanTime));
            KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.Contacts[0].Confidence =,%x\n", InputReport.Contacts[0].Confidence));
            KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.Contacts[0].TipSwitch =,%x\n", InputReport.Contacts[0].TipSwitch));
            KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.Contacts[0].ContactID =,%x\n", InputReport.Contacts[0].ContactID));
            KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.Contacts[0].XL =,%x\n", InputReport.Contacts[0].XL));
            KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.Contacts[0].XH =,%x\n", InputReport.Contacts[0].XH));
            KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.Contacts[0].YL =,%x\n", InputReport.Contacts[0].YL));
            KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.Contacts[0].YH =,%x\n", InputReport.Contacts[0].YH));


            // Report header
            OutputReport.ReportID = FAKE_REPORTID_MULTITOUCH;
            OutputReport.ContactCount = InputReport.ContactCount;
            OutputReport.IsButtonClicked = InputReport.IsButtonClicked;
            OutputReport.ScanTime = InputReport.ScanTime;
            OutputReport.IsButtonClicked = InputReport.IsButtonClicked;
            for (INT i = 0; i < InputReport.ContactCount; i++) {
                OutputReport.Contacts[i].Confidence = InputReport.Contacts[i].Confidence;
                OutputReport.Contacts[i].ContactID = InputReport.Contacts[i].ContactID;
                OutputReport.Contacts[i].TipSwitch = InputReport.Contacts[i].TipSwitch;
                USHORT xh = InputReport.Contacts[i].XH;
                OutputReport.Contacts[i].X = (xh << 8) + InputReport.Contacts[i].XL;
                USHORT yh = InputReport.Contacts[i].YH;
                OutputReport.Contacts[i].Y = (yh << 4) + InputReport.Contacts[i].YL;
            }

            KdPrint(("PtpFilterInputRequestCompletionCallback OutputReport.Contacts[0].X =,%x\n", OutputReport.Contacts[0].X));
            KdPrint(("PtpFilterInputRequestCompletionCallback OutputReport.Contacts[0].Y =,%x\n", OutputReport.Contacts[0].Y));

            pOutputReport = (PUCHAR) &OutputReport;
        }


    }
    else if (deviceContext->VendorID == 0x48D) {//deviceContext->ProductID == 0x8911 
        InputSize = sizeof(PTP_REPORT);
        KdPrint(("PtpFilterInputRequestCompletionCallback VendorID=,%x\n", deviceContext->VendorID));

        if (responseLength == InputSize) {
            PTP_REPORT InputReport = *(PTP_REPORT*)TouchDataBuffer;
            KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.ReportID =,%x\n", InputReport.ReportID));
            KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.ContactCount =,%x\n", InputReport.ContactCount));
            KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.IsButtonClicked =,%x\n", InputReport.IsButtonClicked));
            KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.ScanTime =,%x\n", InputReport.ScanTime));
            KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.Contacts[0].Confidence =,%x\n", InputReport.Contacts[0].Confidence));
            KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.Contacts[0].TipSwitch =,%x\n", InputReport.Contacts[0].TipSwitch));
            KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.Contacts[0].ContactID =,%x\n", InputReport.Contacts[0].ContactID));
            KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.Contacts[0].X =,%x\n", InputReport.Contacts[0].X));
            KdPrint(("PtpFilterInputRequestCompletionCallback InputReport.Contacts[0].Y =,%x\n", InputReport.Contacts[0].Y));


            RtlCopyMemory(&OutputReport, &InputReport, InputSize);
            OutputReport.ReportID = FAKE_REPORTID_MULTITOUCH;
            pOutputReport = (PUCHAR)&OutputReport;
        }

    }


	// Pre-flight check 2: the response size should be sane
	if (responseLength < InputSize) {
		KdPrint(("PtpFilterInputRequestCompletionCallback input received. Length err,%x\n", status));
		WdfTimerStart(deviceContext->HidTransportRecoveryTimer, WDF_REL_TIMEOUT_IN_SEC(3));
		goto cleanup;
	}

    PTP_REPORT ptpReport;

    //Parallel mode
    if (!deviceContext->PtpInputModeOn) {//���뼯���쳣ģʽ��  
        ////����ԭʼ����
        //status = SendOriginalReport(pDevContext, pBuf, Actual_inputReportLength);
        //if (!NT_SUCCESS(status)) {
        //    KdPrint(("OnInterruptIsr SendOriginalReport failed,%x\n", runtimes_ioControl));
        //}
        KdPrint(("PtpFilterInputRequestCompletionCallback PtpInputModeOn not ready,%x\n", runtimes_IOREAD));
        goto cleanup;
    }


    ptpReport = *(PPTP_REPORT)pOutputReport;
    //KdPrint(("PtpFilterInputRequestCompletionCallback PTP_REPORT.ReportID,%x\n", ptpReport.ReportID));
    //KdPrint(("PtpFilterInputRequestCompletionCallback PTP_REPORT.IsButtonClicked,%x\n", ptpReport.IsButtonClicked));
    //KdPrint(("PtpFilterInputRequestCompletionCallback PTP_REPORT.ScanTime,%x\n", ptpReport.ScanTime));
    //KdPrint(("PtpFilterInputRequestCompletionCallback PTP_REPORT.ContactCount,%x\n", ptpReport.ContactCount));

    //KdPrint(("PtpFilterInputRequestCompletionCallback PTP_REPORT..Contacts[0].Confidence ,%x\n", ptpReport.Contacts[0].Confidence));
    //KdPrint(("PtpFilterInputRequestCompletionCallback PTP_REPORT.Contacts[0].ContactID ,%x\n", ptpReport.Contacts[0].ContactID));
    //KdPrint(("PtpFilterInputRequestCompletionCallback PTP_REPORT.Contacts[0].TipSwitch ,%x\n", ptpReport.Contacts[0].TipSwitch));
    //KdPrint(("PtpFilterInputRequestCompletionCallback PTP_REPORT.Contacts[0].Padding ,%x\n", ptpReport.Contacts[0].Padding));
    //KdPrint(("PtpFilterInputRequestCompletionCallback PTP_REPORT.Contacts[0].X ,%x\n", ptpReport.Contacts[0].X));
    //KdPrint(("PtpFilterInputRequestCompletionCallback PTP_REPORT.Contacts[0].Y ,%x\n", ptpReport.Contacts[0].Y));


    if (!deviceContext->bMouseLikeTouchPad_Mode) {//ԭ�津�ذ������ʽֱ�ӷ���ԭʼ����
        PTP_PARSER* tps = &deviceContext->tp_settings;
        if (ptpReport.IsButtonClicked) {
            //��Ҫ�����뿪�ж������򱾴λ��´ν���MouseLikeTouchPad���������ϵbPhysicalButtonUp���ᱻ�ڲ��Ĵ����ִ�����δ֪����
            tps->bPhysicalButtonUp = FALSE;
            KdPrint(("PtpFilterInputRequestCompletionCallback bPhysicalButtonUp FALSE,%x\n", FALSE));
        }
        else {
            if (!tps->bPhysicalButtonUp) {
                tps->bPhysicalButtonUp = TRUE;
                KdPrint(("PtpFilterInputRequestCompletionCallback bPhysicalButtonUp TRUE,%x\n", TRUE));

                if (ptpReport.ContactCount == 5 && !deviceContext->bMouseLikeTouchPad_Mode) {//��ָ��ѹ���ذ�������ʱ���л��ط����ʽ������ģʽ��
                    deviceContext->bMouseLikeTouchPad_Mode = TRUE;
                    KdPrint(("PtpFilterInputRequestCompletionCallback bMouseLikeTouchPad_Mode TRUE,%x\n", status));

                    //�л��ط����ʽ������ģʽ��ͬʱҲ�ָ����ֹ��ܺ�ʵ�ַ�ʽ
                    deviceContext->bWheelDisabled = FALSE;
                    KdPrint(("PtpFilterInputRequestCompletionCallback bWheelDisabled=,%x\n", deviceContext->bWheelDisabled));
                    deviceContext->bWheelScrollMode = FALSE;
                    KdPrint(("PtpFilterInputRequestCompletionCallback bWheelScrollMode=,%x\n", deviceContext->bWheelScrollMode));
                }
            }
        }

        //windowsԭ���PTP��ȷʽ�����������ʽ��ֱ�ӷ���PTP����
        status = SendPtpMultiTouchReport(deviceContext, &ptpReport, sizeof(PTP_REPORT));
        if (!NT_SUCCESS(status)) {
            KdPrint(("PtpFilterInputRequestCompletionCallback SendPtpMultiTouchReport ptpReport failed,%x\n", status));
        }

    }
    else {
        //MouseLikeTouchPad������
        MouseLikeTouchPad_parse(deviceContext, &ptpReport);
    }


    KdPrint(("PtpFilterInputRequestCompletionCallback end1,%x\n", runtimes_IOREAD));

cleanup:
	// Cleanup
	WdfObjectDelete(Request);
	if (requestContext->RequestMemory != NULL) {
		WdfObjectDelete(requestContext->RequestMemory);
	}

	KdPrint(("PtpFilterInputRequestCompletionCallback end,%x\n", runtimes_IOREAD));
	// We don't issue new request here (unless it's a spurious request - which is handled earlier) to
	// keep the request pipe go through one-way.
}


NTSTATUS Filter_DispatchPassThrough(_In_ WDFDEVICE Device, _In_ WDFREQUEST Request, _Out_ BOOLEAN* Pending)
{
    NTSTATUS status = STATUS_SUCCESS;
    PDEVICE_CONTEXT deviceContext;
    BOOLEAN requestSent;
    WDF_REQUEST_SEND_OPTIONS sendOptions;

    KdPrint(("Filter_DispatchPassThrough start,%x\n", status));

    deviceContext = PtpFilterGetContext(Device);

    // Forward the IRP to our upstream IO target
    // We don't really care about the content
    WdfRequestFormatRequestUsingCurrentType(Request);
    WDF_REQUEST_SEND_OPTIONS_INIT(&sendOptions, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);

    // This IOCTL is METHOD_NEITHER, so we just send it without IRP modification
    requestSent = WdfRequestSend(Request, deviceContext->HidIoTarget, &sendOptions);
    *Pending = TRUE;

    if (!requestSent)
    {
        status = WdfRequestGetStatus(Request);
        *Pending = FALSE;
        KdPrint(("Filter_DispatchPassThrough WdfRequestSend failed,%x\n", status));
    }

    KdPrint(("Filter_DispatchPassThrough end,%x\n", status));
    return status;
}




NTSTATUS
AnalyzeHidReportDescriptor(
    PDEVICE_CONTEXT pDevContext
)
{
    NTSTATUS status = STATUS_SUCCESS;
    PBYTE descriptor = pDevContext->pPtpHidReportDesc;
    if (!descriptor) {
        KdPrint(("AnalyzeHidReportDescriptor pPtpHidReportDesc err,%x\n", status));
        return STATUS_UNSUCCESSFUL;
    }

    USHORT descriptorLen = (USHORT)pDevContext->PtpHidReportDescLength;
    PTP_PARSER* tp = &pDevContext->tp_settings;

    int depth = 0;
    BYTE usagePage = 0;
    BYTE reportId = 0;
    BYTE reportSize = 0;
    USHORT reportCount = 0;
    BYTE lastUsage = 0;
    BYTE lastCollection = 0;//�ı����ܹ�����׼ȷ�ж�PTP��MOUSE�������뱨���reportID
    BOOLEAN inConfigTlc = FALSE;
    BOOLEAN inTouchTlc = FALSE;
    BOOLEAN inMouseTlc = FALSE;
    USHORT logicalMax = 0;
    USHORT physicalMax = 0;
    UCHAR unitExp = 0;
    UCHAR unit = 0;

    for (size_t i = 0; i < descriptorLen;) {
        BYTE type = descriptor[i++];
        int size = type & 3;
        if (size == 3) {
            size++;
        }
        BYTE* value = &descriptor[i];
        i += size;

        if (type == HID_TYPE_BEGIN_COLLECTION) {
            depth++;
            if (depth == 1 && usagePage == HID_USAGE_PAGE_DIGITIZER && lastUsage == HID_USAGE_CONFIGURATION) {
                inConfigTlc = TRUE;
                lastCollection = HID_USAGE_CONFIGURATION;
                //KdPrint(("AnalyzeHidReportDescriptor inConfigTlc,%x\n", 0));
            }
            else if (depth == 1 && usagePage == HID_USAGE_PAGE_DIGITIZER && lastUsage == HID_USAGE_DIGITIZER_TOUCH_PAD) {
                inTouchTlc = TRUE;
                lastCollection = HID_USAGE_DIGITIZER_TOUCH_PAD;
                //KdPrint(("AnalyzeHidReportDescriptor inTouchTlc,%x\n", 0));
            }
            else if (depth == 1 && usagePage == HID_USAGE_PAGE_GENERIC && lastUsage == HID_USAGE_GENERIC_MOUSE) {
                inMouseTlc = TRUE;
                lastCollection = HID_USAGE_GENERIC_MOUSE;
                //KdPrint(("AnalyzeHidReportDescriptor inMouseTlc,%x\n", 0));
            }
        }
        else if (type == HID_TYPE_END_COLLECTION) {
            depth--;

            //����3��Tlc״̬�������б�Ҫ�ģ����Է�ֹ������ؼ���Tlc�����ж�����
            if (depth == 0 && inConfigTlc) {
                inConfigTlc = FALSE;
                //KdPrint(("AnalyzeHidReportDescriptor inConfigTlc end,%x\n", 0));
            }
            else if (depth == 0 && inTouchTlc) {
                inTouchTlc = FALSE;
                //KdPrint(("AnalyzeHidReportDescriptor inTouchTlc end,%x\n", 0));
            }
            else if (depth == 0 && inMouseTlc) {
                inMouseTlc = FALSE;
                //KdPrint(("AnalyzeHidReportDescriptor inMouseTlc end,%x\n", 0));
            }

        }
        else if (type == HID_TYPE_USAGE_PAGE) {
            usagePage = *value;
        }
        else if (type == HID_TYPE_USAGE) {
            lastUsage = *value;
        }
        else if (type == HID_TYPE_REPORT_ID) {
            reportId = *value;
        }
        else if (type == HID_TYPE_REPORT_SIZE) {
            reportSize = *value;
        }
        else if (type == HID_TYPE_REPORT_COUNT) {
            reportCount = *value;
        }
        else if (type == HID_TYPE_REPORT_COUNT_2) {
            reportCount = *(PUSHORT)value;
        }
        else if (type == HID_TYPE_LOGICAL_MINIMUM) {
            logicalMax = *value;
        }
        else if (type == HID_TYPE_LOGICAL_MAXIMUM_2) {
            logicalMax = *(PUSHORT)value;
        }
        else if (type == HID_TYPE_PHYSICAL_MAXIMUM) {
            physicalMax = *value;
        }
        else if (type == HID_TYPE_PHYSICAL_MAXIMUM_2) {
            physicalMax = *(PUSHORT)value;
        }
        else if (type == HID_TYPE_UNIT_EXPONENT) {
            unitExp = *value;
        }
        else if (type == HID_TYPE_UNIT) {
            unit = *value;
        }
        else if (type == HID_TYPE_UNIT_2) {
            unit = *value;
        }

        else if (inTouchTlc && depth == 2 && lastCollection == HID_USAGE_DIGITIZER_TOUCH_PAD && lastUsage == HID_USAGE_DIGITIZER_FINGER) {//
            pDevContext->REPORTID_MULTITOUCH_COLLECTION = reportId;
            KdPrint(("AnalyzeHidReportDescriptor REPORTID_MULTITOUCH_COLLECTION=,%x\n", pDevContext->REPORTID_MULTITOUCH_COLLECTION));

            //������㵥���������ݰ�����ָ�������������жϱ���ģʽ��bHybrid_ReportingMode�ĸ�ֵ
            pDevContext->DeviceDescriptorFingerCount++;
            KdPrint(("AnalyzeHidReportDescriptor DeviceDescriptorFingerCount=,%x\n", pDevContext->DeviceDescriptorFingerCount));
        }
        else if (inMouseTlc && depth == 2 && lastCollection == HID_USAGE_GENERIC_MOUSE && lastUsage == HID_USAGE_GENERIC_POINTER) {
            //�²��Mouse����report�������������ȡ��ֻ����Ϊ������ϲ���������Reportʹ��
            pDevContext->REPORTID_MOUSE_COLLECTION = reportId;
            KdPrint(("AnalyzeHidReportDescriptor REPORTID_MOUSE_COLLECTION=,%x\n", pDevContext->REPORTID_MOUSE_COLLECTION));
        }
        else if (inConfigTlc && type == HID_TYPE_FEATURE && lastUsage == HID_USAGE_INPUT_MODE) {
            pDevContext->REPORTSIZE_INPUT_MODE = (reportSize + 7) / 8;//���������ܳ���
            pDevContext->REPORTID_INPUT_MODE = reportId;
            KdPrint(("AnalyzeHidReportDescriptor REPORTID_INPUT_MODE=,%x\n", pDevContext->REPORTID_INPUT_MODE));
            KdPrint(("AnalyzeHidReportDescriptor REPORTSIZE_INPUT_MODE=,%x\n", pDevContext->REPORTSIZE_INPUT_MODE));
            continue;
        }
        else if (inConfigTlc && type == HID_TYPE_FEATURE && lastUsage == HID_USAGE_SURFACE_SWITCH || lastUsage == HID_USAGE_BUTTON_SWITCH) {
            //Ĭ�ϱ�׼�淶ΪHID_USAGE_SURFACE_SWITCH��HID_USAGE_BUTTON_SWITCH��1bit��ϵ�λ��1���ֽ�HID_USAGE_FUNCTION_SWITCH����
            pDevContext->REPORTSIZE_FUNCTION_SWITCH = (reportSize + 7) / 8;//���������ܳ���
            pDevContext->REPORTID_FUNCTION_SWITCH = reportId;
            KdPrint(("AnalyzeHidReportDescriptor REPORTID_FUNCTION_SWITCH=,%x\n", pDevContext->REPORTID_FUNCTION_SWITCH));
            KdPrint(("AnalyzeHidReportDescriptor REPORTSIZE_FUNCTION_SWITCH=,%x\n", pDevContext->REPORTSIZE_FUNCTION_SWITCH));
            continue;
        }
        else if (inTouchTlc && type == HID_TYPE_FEATURE && lastUsage == HID_USAGE_CONTACT_COUNT_MAXIMUM || lastUsage == HID_USAGE_PAD_TYPE) {
            //Ĭ�ϱ�׼�淶ΪHID_USAGE_CONTACT_COUNT_MAXIMUM��HID_USAGE_PAD_TYPE��4bit��ϵ�λ��1���ֽ�HID_USAGE_DEVICE_CAPS����
            pDevContext->REPORTSIZE_DEVICE_CAPS = (reportSize + 7) / 8;//���������ܳ���
            pDevContext->REPORTID_DEVICE_CAPS = reportId;
            KdPrint(("AnalyzeHidReportDescriptor REPORTSIZE_DEVICE_CAPS=,%x\n", pDevContext->REPORTSIZE_DEVICE_CAPS));
            KdPrint(("AnalyzeHidReportDescriptor REPORTID_DEVICE_CAPS=,%x\n", pDevContext->REPORTID_DEVICE_CAPS));
            continue;
        }
        else if (inTouchTlc && type == HID_TYPE_FEATURE && lastUsage == HID_USAGE_LATENCY_MODE) {
            //�ӳ�ģʽ���ܱ���//Ĭ�ϱ�׼�淶ΪHID_USAGE_LATENCY_MODE��λ1bit��ϳ�1���ֽ�HID_USAGE_LATENCY_MODE����
            pDevContext->REPORTSIZE_LATENCY_MODE = (reportSize + 7) / 8;//���������ܳ���
            pDevContext->REPORTID_LATENCY_MODE = reportId;
            KdPrint(("AnalyzeHidReportDescriptor REPORTSIZE_LATENCY_MODE=,%x\n", pDevContext->REPORTSIZE_LATENCY_MODE));
            KdPrint(("AnalyzeHidReportDescriptor REPORTID_LATENCY_MODE=,%x\n", pDevContext->REPORTID_LATENCY_MODE));
            continue;
        }
        else if (inTouchTlc && type == HID_TYPE_FEATURE && lastUsage == HID_USAGE_PAGE_VENDOR_DEFINED_DEVICE_CERTIFICATION) {
            pDevContext->REPORTSIZE_PTPHQA = 256;
            pDevContext->REPORTID_PTPHQA = reportId;
            KdPrint(("AnalyzeHidReportDescriptor REPORTID_PTPHQA=,%x\n", pDevContext->REPORTID_PTPHQA));
            continue;
        }
        else if (inTouchTlc && type == HID_TYPE_INPUT && lastUsage == HID_USAGE_X) {
            tp->physicalMax_X = physicalMax;
            tp->logicalMax_X = logicalMax;
            tp->unitExp = UnitExponent_Table[unitExp];
            tp->unit = unit;
            KdPrint(("AnalyzeHidReportDescriptor physicalMax_X=,%x\n", tp->physicalMax_X));
            KdPrint(("AnalyzeHidReportDescriptor logicalMax_X=,%x\n", tp->logicalMax_X));
            KdPrint(("AnalyzeHidReportDescriptor unitExp=,%x\n", tp->unitExp));
            KdPrint(("AnalyzeHidReportDescriptor unit=,%x\n", tp->unit));
            continue;
        }
        else if (inTouchTlc && type == HID_TYPE_INPUT && lastUsage == HID_USAGE_Y) {
            tp->physicalMax_Y = physicalMax;
            tp->logicalMax_Y = logicalMax;
            tp->unitExp = UnitExponent_Table[unitExp];
            tp->unit = unit;
            KdPrint(("AnalyzeHidReportDescriptor physicalMax_Y=,%x\n", tp->physicalMax_Y));
            KdPrint(("AnalyzeHidReportDescriptor logicalMax_Y=,%x\n", tp->logicalMax_Y));
            KdPrint(("AnalyzeHidReportDescriptor unitExp=,%x\n", tp->unitExp));
            KdPrint(("AnalyzeHidReportDescriptor unit=,%x\n", tp->unit));
            continue;
        }
    }

    //�жϴ����屨��ģʽ
    if (pDevContext->DeviceDescriptorFingerCount < 4) {//4����ָ��������
        //Single finger hybrid reporting mode��ָ��ϱ���ģʽȷ�ϣ���������֧��
        KdPrint(("AnalyzeHidReportDescriptor bHybrid_ReportingMode Confirm,%x\n", pDevContext->DeviceDescriptorFingerCount));
        return STATUS_UNSUCCESSFUL;//���غ���ֹ��������
    }


    //���㱣�津����ߴ�ֱ��ʵȲ���
    //ת��Ϊmm���ȵ�λ
    if (tp->unit == 0x11) {//cm���ȵ�λ
        tp->physical_Width_mm = tp->physicalMax_X * pow(10.0, tp->unitExp) * 10;
        tp->physical_Height_mm = tp->physicalMax_Y * pow(10.0, tp->unitExp) * 10;
    }
    else {//0x13Ϊinch���ȵ�λ
        tp->physical_Width_mm = tp->physicalMax_X * pow(10.0, tp->unitExp) * 25.4;
        tp->physical_Height_mm = tp->physicalMax_Y * pow(10.0, tp->unitExp) * 25.4;
    }

    if (!tp->physical_Width_mm) {
        KdPrint(("AnalyzeHidReportDescriptor physical_Width_mm err,%x\n", 0));
        return STATUS_UNSUCCESSFUL;
    }
    if (!tp->physical_Height_mm) {
        KdPrint(("AnalyzeHidReportDescriptor physical_Height_mm err,%x\n", 0));
        return STATUS_UNSUCCESSFUL;
    }

    tp->TouchPad_DPMM_x = (float)(tp->logicalMax_X / tp->physical_Width_mm);//��λΪdot/mm
    tp->TouchPad_DPMM_y = (float)(tp->logicalMax_Y / tp->physical_Height_mm);//��λΪdot/mm
    KdPrint(("AnalyzeHidReportDescriptor TouchPad_DPMM_x=,%x\n", (ULONG)tp->TouchPad_DPMM_x));
    KdPrint(("AnalyzeHidReportDescriptor TouchPad_DPMM_y=,%x\n", (ULONG)tp->TouchPad_DPMM_y));
 

    KdPrint(("AnalyzeHidReportDescriptor end,%x\n", status));
    return status;
}



NTSTATUS SetRegisterMouseSensitivity(PDEVICE_CONTEXT pDevContext, ULONG ms_idx)//�������õ�ע���
{
    UNREFERENCED_PARAMETER(pDevContext);

    NTSTATUS status = STATUS_SUCCESS;

    status = SetRegConfig(pDevContext, L"MouseSensitivity_Index", ms_idx);
    if (!NT_SUCCESS(status)) {
        KdPrint(("SetRegisterMouseSensitivity err,%x\n", status));
        return status;
    }

    KdPrint(("SetRegisterMouseSensitivity ok,%x\n", status));
    return status;
}


NTSTATUS GetRegisterMouseSensitivity(PDEVICE_CONTEXT pDevContext, ULONG *ms_idx)//��ȡ���ô�ע���
{
    UNREFERENCED_PARAMETER(pDevContext);

    NTSTATUS status = STATUS_SUCCESS;

    status = GetRegConfig(pDevContext, L"MouseSensitivity_Index", ms_idx);
    if (!NT_SUCCESS(status)) {
        KdPrint(("GetRegisterMouseSensitivity err,%x\n", status));
        return status;
    }

    KdPrint(("GetRegisterMouseSensitivity ok,%x\n", status));
    return status;
}



NTSTATUS SetRegConfig(PDEVICE_CONTEXT pDevContext, WCHAR* strConfigName, ULONG nConfigValue)//SetRegConfig(L"_MouseSensitivity_Index",0x1234);
{
    NTSTATUS status = STATUS_SUCCESS;

    //��ʼ��ע�����
    UNICODE_STRING stringKey;
    RtlInitUnicodeString(&stringKey, L"\\Registry\\Machine\\Software\\Microsoft\\Windows\\CurrentVersion\\PrecisionTouchPad");

    //��ʼ��OBJECT_ATTRIBUTES�ṹ
    OBJECT_ATTRIBUTES  ObjectAttributes;
    InitializeObjectAttributes(&ObjectAttributes, &stringKey, OBJ_CASE_INSENSITIVE, NULL, NULL);//OBJ_CASE_INSENSITIVE�Դ�Сд����

    //����ע�����
    HANDLE hKey;
    ULONG Des;
    status = ZwCreateKey(&hKey, KEY_ALL_ACCESS, &ObjectAttributes, 0, NULL, REG_OPTION_NON_VOLATILE, &Des);
    if (NT_SUCCESS(status))
    {
        if (Des == REG_CREATED_NEW_KEY)
        {
            KdPrint(("�½�ע����\n"));
        }
        else
        {
            KdPrint(("Ҫ������ע������Ѿ����ڣ�\n"));
        }
    }
    else {
        return status;
    }


    // ����תΪ�ַ���
    WCHAR strVIDnum[10];
    RtlStringCbPrintfW(strVIDnum, 10, L"%04X", pDevContext->VendorID);

    WCHAR stringConfigName[128];

    WCHAR strVIDheader[] = L"TouchPad_VID_";
    wcscpy(stringConfigName, strVIDheader);
    wcscat(stringConfigName, strVIDnum);
    WCHAR strVIDend[] = L"_";
    wcscat(stringConfigName, strVIDend);

    wcscat(stringConfigName, strConfigName);

    UNICODE_STRING strKeyName;
    RtlInitUnicodeString(&strKeyName, stringConfigName);


    //����REG_DWORD��ֵ
    status = ZwSetValueKey(hKey, &strKeyName, 0, REG_DWORD, &nConfigValue, 4);
    if (!NT_SUCCESS(status))
    {
        KdPrint(("����REG_DWORD��ֵʧ�ܣ�\n"));
    }

    ZwFlushKey(hKey);
    //�ر�ע�����
    ZwClose(hKey);
    return status;
}


NTSTATUS GetRegConfig(PDEVICE_CONTEXT pDevContext, WCHAR* strConfigName, PULONG pConfigValue)//GetRegConfig(L"_MouseSensitivity_Index",&nConfigValue);
{
    NTSTATUS status = STATUS_SUCCESS;

    //��ʼ��ע�����
    UNICODE_STRING stringKey;
    RtlInitUnicodeString(&stringKey, L"\\Registry\\Machine\\Software\\Microsoft\\Windows\\CurrentVersion\\PrecisionTouchPad");

    //��ʼ��OBJECT_ATTRIBUTES�ṹ
    OBJECT_ATTRIBUTES  ObjectAttributes;
    InitializeObjectAttributes(&ObjectAttributes, &stringKey, OBJ_CASE_INSENSITIVE, NULL, NULL);//OBJ_CASE_INSENSITIVE�Դ�Сд����

    //����ע�����
    HANDLE hKey;
    ULONG Des;
    status = ZwCreateKey(&hKey, KEY_ALL_ACCESS, &ObjectAttributes, 0, NULL, REG_OPTION_NON_VOLATILE, &Des);
    if (NT_SUCCESS(status))
    {
        if (Des == REG_CREATED_NEW_KEY)
        {
            KdPrint(("�½�ע����\n"));
        }
        else
        {
            KdPrint(("Ҫ������ע������Ѿ����ڣ�\n"));
        }
    }
    else {
        return STATUS_UNSUCCESSFUL;
    }


    // ����תΪ�ַ���
    WCHAR strVIDnum[10];
    RtlStringCbPrintfW(strVIDnum, 10, L"%04X", pDevContext->VendorID);

    WCHAR stringConfigName[128];

    WCHAR strVIDheader[] = L"TouchPad_VID_";
    wcscpy(stringConfigName, strVIDheader);
    wcscat(stringConfigName, strVIDnum);
    WCHAR strVIDend[] = L"_";
    wcscat(stringConfigName, strVIDend);

    wcscat(stringConfigName, strConfigName);

    UNICODE_STRING strKeyName;
    RtlInitUnicodeString(&strKeyName, stringConfigName);


    //��ʼ��
    ULONG RetSize = 0;

    //��һ�ε���ZwQueryValueKey  ����ȡKeyValuePartialInformation���͵Ĵ�С Ȼ�����һ��buffer
    status = ZwQueryValueKey(hKey, &strKeyName, KeyValuePartialInformation, NULL, 0, &RetSize);
    //������ȷ����������ص���STATUS_BUFFER_TOO_SMALL  ����ʹ��NT_SUCCESS���ж�
    if (status == STATUS_OBJECT_NAME_NOT_FOUND || status == STATUS_INVALID_PARAMETER || RetSize == 0)
    {
        DbgPrint("��ѯ��ֵ����ʧ��");
        ZwClose(hKey);
        return status;
    }

    //����һ���ڴ������շ��ص���Ϣ
    PKEY_VALUE_PARTIAL_INFORMATION pvpi = (PKEY_VALUE_PARTIAL_INFORMATION)ExAllocatePool(PagedPool, RetSize);
    if (!pvpi)
    {
        KdPrint(("�ڴ����ʧ��"));
        ZwClose(hKey);
        return status;
    }
    //��ѯ��Ϣ������ΪPKEY_VALUE_PARTIAL_INFORMATION
    status = ZwQueryValueKey(hKey, &strKeyName, KeyValuePartialInformation, pvpi, RetSize, &RetSize);

    if (!NT_SUCCESS(status))
    {
        KdPrint(("��ѯ��ֵʧ��\n"));

        return status;
    }
    else
    {
        switch (pvpi->Type)
        {
        case REG_DWORD://��ȡREG_DWORD��ֵ
            *pConfigValue = *((PULONG)(&pvpi->Data[0]));
            KdPrint(("��ѯ��ֵΪ%x\n",*pConfigValue));
            break;
        default:
            KdPrint(("δ�����ļ�ֵ\n"));
            status = STATUS_UNSUCCESSFUL;
            break;
        }

    }

    ZwFlushKey(hKey);
    //�ر�ע�����
    ZwClose(hKey);
    return status;

}


VOID init(PDEVICE_CONTEXT pDevContext) {
    NTSTATUS status = STATUS_SUCCESS;


    //��ȡ�豸��������
    pDevContext->DeviceType_Index = 1;

    ULONG dt_idx;
    status = GetRegisterDeviceType(pDevContext, &dt_idx);
    if (!NT_SUCCESS(status))
    {
        KdPrint(("init GetRegisterDeviceType err,%x\n", status));
        status = SetRegisterDeviceType(pDevContext, pDevContext->DeviceType_Index);//��ʼĬ������
        if (!NT_SUCCESS(status)) {
            KdPrint(("init SetRegisterDeviceType err,%x\n", status));
        }
    }
    else {
        if (dt_idx > 2) {//�����ȡ����ֵ����
            dt_idx = pDevContext->DeviceType_Index;//�ָ���ʼĬ��ֵ
        }
        pDevContext->DeviceType_Index = (UCHAR)dt_idx;
        KdPrint(("init GetRegisterDeviceType DeviceType_Index=,%x\n", pDevContext->DeviceType_Index));
    }


    //��ȡ��������Կո������λ�ò�������
    pDevContext->SpaceLayout_Index = 1;

    ULONG sl_idx;
    status = GetRegisterSpaceLayout(pDevContext, &sl_idx);
    if (!NT_SUCCESS(status))
    {
        KdPrint(("init GetRegisterSpaceLayout err,%x\n", status));
        status = SetRegisterSpaceLayout(pDevContext, pDevContext->SpaceLayout_Index);//��ʼĬ������
        if (!NT_SUCCESS(status)) {
            KdPrint(("init SetRegisterSpaceLayout err,%x\n", status));
        }
    }
    else {
        if (sl_idx > 2) {//�����ȡ����ֵ����
            sl_idx = pDevContext->SpaceLayout_Index;//�ָ���ʼĬ��ֵ
        }
        pDevContext->SpaceLayout_Index = (UCHAR)sl_idx;
        KdPrint(("init GetRegisterSpaceLayout DeviceType_Index=,%x\n", pDevContext->DeviceType_Index));
    }


    //��ȡ�������������
    pDevContext->MouseSensitivity_Index = 1;//Ĭ�ϳ�ʼֵΪMouseSensitivityTable�洢������1��
    pDevContext->MouseSensitivity_Value = MouseSensitivityTable[pDevContext->MouseSensitivity_Index];//Ĭ�ϳ�ʼֵΪ1.0

    ULONG ms_idx;
    status = GetRegisterMouseSensitivity(pDevContext, &ms_idx);
    if (!NT_SUCCESS(status))
    {
        KdPrint(("init GetRegisterMouseSensitivity err,%x\n", status));
        status = SetRegisterMouseSensitivity(pDevContext, pDevContext->MouseSensitivity_Index);//��ʼĬ������
        if (!NT_SUCCESS(status)) {
            KdPrint(("init SetRegisterMouseSensitivity err,%x\n", status));
        }
    }
    else {
        if (ms_idx > 2) {//�����ȡ����ֵ����
            ms_idx = pDevContext->MouseSensitivity_Index;//�ָ���ʼĬ��ֵ
        }
        pDevContext->MouseSensitivity_Index = (UCHAR)ms_idx;
        pDevContext->MouseSensitivity_Value = MouseSensitivityTable[pDevContext->MouseSensitivity_Index];
        KdPrint(("init GetRegisterMouseSensitivity MouseSensitivity_Index=,%x\n", pDevContext->MouseSensitivity_Index));
    }



    //��ȡָͷ��С����
    pDevContext->ThumbScale_Index = 1;
    pDevContext->ThumbScale_Value = ThumbScaleTable[pDevContext->ThumbScale_Index];

    ULONG ts_idx;
    status = GetRegisterThumbScale(pDevContext, &ts_idx);
    if (!NT_SUCCESS(status))
    {
        KdPrint(("init GetRegisterThumbScale err,%x\n", status));
        status = SetRegisterThumbScale(pDevContext, pDevContext->ThumbScale_Index);//��ʼĬ������
        if (!NT_SUCCESS(status)) {
            KdPrint(("init SetRegisterThumbScale err,%x\n", status));
        }
    }
    else {
        if (ts_idx > 2) {//�����ȡ����ֵ����
            ts_idx = pDevContext->ThumbScale_Index;//�ָ���ʼĬ��ֵ
        }
        pDevContext->ThumbScale_Index = (UCHAR)ts_idx;
        pDevContext->ThumbScale_Value = ThumbScaleTable[pDevContext->ThumbScale_Index];
        KdPrint(("init GetRegisterThumbScale ThumbScale_Index=,%x\n", pDevContext->ThumbScale_Index));
    }

    //

    PTP_PARSER* tp = &pDevContext->tp_settings;
    //��̬������ָͷ��С����
    tp->thumb_Scale = pDevContext->ThumbScale_Index;//��ָͷ�ߴ����ű�����
    tp->FingerMinDistance = 12 * tp->TouchPad_DPMM_x * tp->thumb_Scale;//������Ч��������ָ��С����
    tp->FingerClosedThresholdDistance = 16 * tp->TouchPad_DPMM_x * tp->thumb_Scale;//����������ָ��£ʱ����С����
    tp->FingerMaxDistance = tp->FingerMinDistance * 4;//������Ч��������ָ������(FingerMinDistance*4) 

    tp->PointerSensitivity_x = tp->TouchPad_DPMM_x / 25;
    tp->PointerSensitivity_y = tp->TouchPad_DPMM_y / 25;

    //
    ULONG Offset = 0;
    ULONG SpaceCenterline;
    ULONG HalfWidthX;
    ULONG CornerWidth;

    //TouchpadSpaceLayout(��������Կո������λ�ò�����ƣ��������󴥵�������㣩
    if (pDevContext->SpaceLayout_Index == 0) {//���в���CenterAlign
        Offset = 0;//�����������߽Ͽո�������߶��룬
    }
    else if (pDevContext->SpaceLayout_Index == 1) {//1-ƫ��5mm���RightAlign5
        Offset = (ULONG)(5 * tp->TouchPad_DPMM_x);//�����������߽Ͽո��������ƫ��5mm��
    }
    else if (pDevContext->SpaceLayout_Index == 2) {//2-ƫ��10mm���RightAlign10
        //�����������߽Ͽո��������ƫ��10mm��Ȼ��ͬ�Ĵ�����ߴ��Դ�������Ϊ�Գ������λ�ò��֣�DisabledX_RIGHT�������Ҳ�������󴥷�Χ����
        Offset = (LONG)(10 * tp->TouchPad_DPMM_x);//�����������߽Ͽո��������ƫ��10mm��
    }

    SpaceCenterline = (ULONG)(40 * tp->TouchPad_DPMM_x);//���������XֵΪ����ո�����������Ҳ�40mm����X����,
    HalfWidthX = tp->logicalMax_X / 2;//������һ������ֵ
    CornerWidth = (ULONG)(10 * tp->TouchPad_DPMM_x);//������߽ǹ��ܼ�������10mm��

    //������߽ǹ��ܼ���������
    if (HalfWidthX > SpaceCenterline) {
        //
        tp->CornerX_LEFT = HalfWidthX - SpaceCenterline;
        if (tp->CornerX_LEFT < CornerWidth) {
            tp->CornerX_LEFT = CornerWidth;
        }
        tp->CornerX_RIGHT = HalfWidthX + SpaceCenterline;
        if (tp->CornerX_RIGHT > (tp->logicalMax_X - CornerWidth)) {
            tp->CornerX_RIGHT = tp->logicalMax_X - CornerWidth;
        }
    }
    else {
        tp->CornerX_LEFT = CornerWidth;
        tp->CornerX_RIGHT = tp->logicalMax_X - CornerWidth;
    }

    KdPrint(("AnalyzeHidReportDescriptor tp->CornerX_LEFT =,%x\n", tp->CornerX_LEFT));
    KdPrint(("AnalyzeHidReportDescriptor tp->CornerX_RIGHT =,%x\n", tp->CornerX_RIGHT));


    if (pDevContext->DeviceType_Index == 1) {//���ö���������������󴥹���
        tp->StartY_TOP = 0;
        tp->StartX_LEFT = 0;
        tp->StartX_RIGHT = tp->logicalMax_X;
    }
    else {
        tp->StartY_TOP = (ULONG)(10 * tp->TouchPad_DPMM_y);////����󴥺���YֵΪ���봥���嶥��10mm����Y����

        LONG DisabledX_LEFT = HalfWidthX - SpaceCenterline - Offset;
        ULONG DisabledX_RIGHT = HalfWidthX + SpaceCenterline - Offset;

        if (DisabledX_LEFT < 0) {
            tp->StartX_LEFT = 0;
        }
        else {
            tp->StartX_LEFT = DisabledX_LEFT;
        }

        if (DisabledX_RIGHT > tp->logicalMax_X) {
            tp->StartX_RIGHT = tp->logicalMax_X;
        }
        else {
            tp->StartX_RIGHT = DisabledX_RIGHT;
        }
    }

    KdPrint(("AnalyzeHidReportDescriptor tp->StartTop_Y =,%x\n", tp->StartY_TOP));
    KdPrint(("AnalyzeHidReportDescriptor tp->StartX_LEFT =,%x\n", tp->StartX_LEFT));
    KdPrint(("AnalyzeHidReportDescriptor tp->StartX_RIGHT =,%x\n", tp->StartX_RIGHT));





    MouseLikeTouchPad_parse_init(pDevContext);

}


VOID KdPrintDataFun(CHAR* pChars, PUCHAR DataBuffer, ULONG DataSize)
{
    DbgPrint(pChars);
    for (UINT i = 0; i < DataSize; i++) {
        DbgPrint("% x,", DataBuffer[i]);
    }
    DbgPrint("\n");
}


NTSTATUS
SendOriginalReport(PDEVICE_CONTEXT pDevContext, PVOID OriginalReport, size_t outputBufferLength)
{
    NTSTATUS status = STATUS_SUCCESS;

    WDFREQUEST PtpRequest;
    WDFMEMORY  ptpRequestMemory;

    // Read report and fulfill PTP request. If no report is found, just exit.
    status = WdfIoQueueRetrieveNextRequest(pDevContext->HidReadQueue, &PtpRequest);
    if (!NT_SUCCESS(status)) {
        KdPrint(("SendOriginalReport WdfIoQueueRetrieveNextRequest failed,%x\n", status));
        goto exit;
    }


    status = WdfRequestRetrieveOutputMemory(PtpRequest, &ptpRequestMemory);
    if (!NT_SUCCESS(status))
    {
        KdPrint(("SendOriginalReport WdfRequestRetrieveOutputMemory failed,%x\n", status));
        WdfDeviceSetFailed(pDevContext->Device, WdfDeviceFailedAttemptRestart);
        goto exit;
    }

    status = WdfMemoryCopyFromBuffer(ptpRequestMemory, 0, OriginalReport, outputBufferLength);
    if (!NT_SUCCESS(status))
    {
        KdPrint(("SendOriginalReport WdfMemoryCopyFromBuffer failed,%x\n", status));
        WdfDeviceSetFailed(pDevContext->Device, WdfDeviceFailedAttemptRestart);
        goto exit;
    }

    WdfRequestSetInformation(PtpRequest, outputBufferLength);
    WdfRequestComplete(PtpRequest, status);


exit:
    KdPrint(("SendOriginalReport end,%x\n", status));
    return status;

}

NTSTATUS
SendPtpMultiTouchReport(PDEVICE_CONTEXT pDevContext, PVOID MultiTouchReport, size_t outputBufferLength)
{
    NTSTATUS status = STATUS_SUCCESS;

    WDFREQUEST PtpRequest;
    WDFMEMORY  ptpRequestMemory;

    // Read report and fulfill PTP request. If no report is found, just exit.
    status = WdfIoQueueRetrieveNextRequest(pDevContext->HidReadQueue, &PtpRequest);
    if (!NT_SUCCESS(status)) {
        KdPrint(("SendPtpMultiTouchReport WdfIoQueueRetrieveNextRequest failed,%x\n", status));
        goto cleanup;
    }

    status = WdfRequestRetrieveOutputMemory(PtpRequest, &ptpRequestMemory);
    if (!NT_SUCCESS(status))
    {
        KdPrint(("SendPtpMultiTouchReport WdfRequestRetrieveOutputMemory failed,%x\n", status));
        WdfDeviceSetFailed(pDevContext->Device, WdfDeviceFailedAttemptRestart);
        goto exit;
    }

    status = WdfMemoryCopyFromBuffer(ptpRequestMemory, 0, MultiTouchReport, outputBufferLength);
    if (!NT_SUCCESS(status))
    {
        KdPrint(("SendPtpMultiTouchReport WdfMemoryCopyFromBuffer failed,%x\n", status));
        WdfDeviceSetFailed(pDevContext->Device, WdfDeviceFailedAttemptRestart);
        goto exit;
    }

    WdfRequestSetInformation(PtpRequest, outputBufferLength);
    KdPrint(("SendPtpMultiTouchReport ok,%x\n", status));

exit:
    WdfRequestComplete(
        PtpRequest,
        status
    );

cleanup:
    KdPrint(("SendPtpMultiTouchReport end,%x\n", status));
    return status;

}

NTSTATUS
SendPtpMouseReport(PDEVICE_CONTEXT pDevContext, struct mouse_report_t* pMouseReport)
{
    NTSTATUS status = STATUS_SUCCESS;

    WDFREQUEST PtpRequest;
    WDFMEMORY  ptpRequestMemory;
    size_t     outputBufferLength = sizeof(struct mouse_report_t);
    //KdPrint(("SendPtpMouseReport pMouseReport=", pMouseReport, (ULONG)outputBufferLength);

    // Read report and fulfill PTP request. If no report is found, just exit.
    status = WdfIoQueueRetrieveNextRequest(pDevContext->HidReadQueue, &PtpRequest);
    if (!NT_SUCCESS(status)) {
        KdPrint(("SendPtpMouseReport WdfIoQueueRetrieveNextRequest failed,%x\n", status));
        goto cleanup;
    }

    status = WdfRequestRetrieveOutputMemory(PtpRequest, &ptpRequestMemory);
    if (!NT_SUCCESS(status))
    {
        KdPrint(("SendPtpMouseReport WdfRequestRetrieveOutputMemory failed,%x\n", status));
        WdfDeviceSetFailed(pDevContext->Device, WdfDeviceFailedAttemptRestart);
        goto exit;
    }

    status = WdfMemoryCopyFromBuffer(ptpRequestMemory, 0, pMouseReport, outputBufferLength);
    if (!NT_SUCCESS(status))
    {
        KdPrint(("SendPtpMouseReport WdfMemoryCopyFromBuffer failed,%x\n", status));
        WdfDeviceSetFailed(pDevContext->Device, WdfDeviceFailedAttemptRestart);
        goto exit;
    }

    WdfRequestSetInformation(PtpRequest, outputBufferLength);
    KdPrint(("SendPtpMouseReport ok,%x\n", runtimes_IOREAD));

exit:
    WdfRequestComplete(
        PtpRequest,
        status
    );

cleanup:
    KdPrint(("SendPtpMouseReport ok2,%x\n", runtimes_IOREAD));
    KdPrint(("SendPtpMouseReport end,%x\n", status));
    return status;
}


void MouseLikeTouchPad_parse(PDEVICE_CONTEXT pDevContext, PTP_REPORT* pPtpReport)
{
    NTSTATUS status = STATUS_SUCCESS;

    PTP_PARSER* tp = &pDevContext->tp_settings;

    //���㱨��Ƶ�ʺ�ʱ����
    KeQueryTickCount(&tp->current_Ticktime);
    tp->ticktime_Interval.QuadPart = (tp->current_Ticktime.QuadPart - tp->last_Ticktime.QuadPart) * tp->tick_Count / 10000;//��λms����
    tp->TouchPad_ReportInterval = (float)tp->ticktime_Interval.LowPart;//�����屨����ʱ��ms
    tp->last_Ticktime = tp->current_Ticktime;


    //���浱ǰ��ָ����
    tp->currentFinger = *pPtpReport;
    UCHAR currentFinger_Count = tp->currentFinger.ContactCount;//��ǰ����������
    UCHAR lastFinger_Count = tp->lastFinger.ContactCount; //�ϴδ���������
    KdPrint(("MouseLikeTouchPad_parse currentFinger_Count=,%x\n", currentFinger_Count));
    KdPrint(("MouseLikeTouchPad_parse lastFinger_Count=,%x\n", lastFinger_Count));

    UCHAR MAX_CONTACT_FINGER = PTP_MAX_CONTACT_POINTS;
    BOOLEAN allFingerDetached = TRUE;
    for (UCHAR i = 0; i < MAX_CONTACT_FINGER; i++) {//����TipSwitchΪ0ʱ�ж�Ϊ��ָȫ���뿪����Ϊ���һ�����뿪ʱContactCount��Confidenceʼ��Ϊ1������0��
        if (tp->currentFinger.Contacts[i].TipSwitch) {
            allFingerDetached = FALSE;
            currentFinger_Count = tp->currentFinger.ContactCount;//���¶��嵱ǰ����������

            KdPrint(("MouseLikeTouchPad_parse allFingerDetached = FALSE,%x\n", runtimes_IOREAD));
            break;
        }
    }
    if (allFingerDetached) {
        currentFinger_Count = 0;
        KdPrint(("MouseLikeTouchPad_parse ��ָȫ���뿪,%x\n", runtimes_IOREAD));
    }


    //��ʼ������¼�
    struct mouse_report_t mReport;
    mReport.report_id = FAKE_REPORTID_MOUSE;//FAKE_REPORTID_MOUSE//pDevContext->REPORTID_MOUSE_COLLECTION

    mReport.button = 0;
    mReport.dx = 0;
    mReport.dy = 0;
    mReport.h_wheel = 0;
    mReport.v_wheel = 0;

    BOOLEAN bMouse_LButton_Status = 0; //������ʱ������״̬��0Ϊ�ͷţ�1Ϊ���£�ÿ�ζ���Ҫ����ȷ�������߼�
    BOOLEAN bMouse_MButton_Status = 0; //������ʱ����м�״̬��0Ϊ�ͷţ�1Ϊ���£�ÿ�ζ���Ҫ����ȷ�������߼�
    BOOLEAN bMouse_RButton_Status = 0; //������ʱ����Ҽ�״̬��0Ϊ�ͷţ�1Ϊ���£�ÿ�ζ���Ҫ����ȷ�������߼�
    BOOLEAN bMouse_BButton_Status = 0; //������ʱ���Back���˼�״̬��0Ϊ�ͷţ�1Ϊ���£�ÿ�ζ���Ҫ����ȷ�������߼�
    BOOLEAN bMouse_FButton_Status = 0; //������ʱ���Forwardǰ����״̬��0Ϊ�ͷţ�1Ϊ���£�ÿ�ζ���Ҫ����ȷ�������߼�

    //��ʼ����ǰ�����������ţ����ٺ�δ�ٸ�ֵ�ı�ʾ��������
    tp->nMouse_Pointer_CurrentIndex = -1;
    tp->nMouse_LButton_CurrentIndex = -1;
    tp->nMouse_RButton_CurrentIndex = -1;
    tp->nMouse_MButton_CurrentIndex = -1;
    tp->nMouse_Wheel_CurrentIndex = -1;


    //������ָ������������Ÿ���
    for (char i = 0; i < currentFinger_Count; i++) {
        if (!tp->currentFinger.Contacts[i].Confidence || !tp->currentFinger.Contacts[i].TipSwitch) {//�����ж�Confidence��TipSwitch������Ч����������     
            continue;
        }

        if (tp->nMouse_Pointer_LastIndex != -1) {
            if (tp->lastFinger.Contacts[tp->nMouse_Pointer_LastIndex].ContactID == tp->currentFinger.Contacts[i].ContactID) {
                tp->nMouse_Pointer_CurrentIndex = i;//�ҵ�ָ��

                KdPrint(("MouseLikeTouchPad_parse �ҵ�ָ�� tp->nMouse_Pointer_CurrentIndex=,%x\n", tp->nMouse_Pointer_CurrentIndex));
                continue;//������������
            }
        }

        if (tp->nMouse_Wheel_LastIndex != -1) {
            if (tp->lastFinger.Contacts[tp->nMouse_Wheel_LastIndex].ContactID == tp->currentFinger.Contacts[i].ContactID) {
                tp->nMouse_Wheel_CurrentIndex = i;//�ҵ����ָ�����

                KdPrint(("MouseLikeTouchPad_parse �ҵ����ָ����� tp->nMouse_Wheel_CurrentIndex=,%x\n", tp->nMouse_Wheel_CurrentIndex));
                continue;//������������
            }
        }

        if (tp->nMouse_LButton_LastIndex != -1) {
            if (tp->lastFinger.Contacts[tp->nMouse_LButton_LastIndex].ContactID == tp->currentFinger.Contacts[i].ContactID) {
                bMouse_LButton_Status = 1; //�ҵ������
                tp->nMouse_LButton_CurrentIndex = i;//��ֵ�����������������

                KdPrint(("MouseLikeTouchPad_parse �ҵ���� tp->nMouse_LButton_CurrentIndex=,%x\n", tp->nMouse_LButton_CurrentIndex));
                continue;//������������
            }
        }

        if (tp->nMouse_RButton_LastIndex != -1) {
            if (tp->lastFinger.Contacts[tp->nMouse_RButton_LastIndex].ContactID == tp->currentFinger.Contacts[i].ContactID) {
                bMouse_RButton_Status = 1; //�ҵ��Ҽ���
                tp->nMouse_RButton_CurrentIndex = i;//��ֵ�Ҽ���������������

                KdPrint(("MouseLikeTouchPad_parse �ҵ��Ҽ� tp->nMouse_RButton_CurrentIndex=,%x\n", tp->nMouse_RButton_CurrentIndex));
                continue;//������������
            }
        }

        if (tp->nMouse_MButton_LastIndex != -1) {
            if (tp->lastFinger.Contacts[tp->nMouse_MButton_LastIndex].ContactID == tp->currentFinger.Contacts[i].ContactID) {
                bMouse_MButton_Status = 1; //�ҵ��м���
                tp->nMouse_MButton_CurrentIndex = i;//��ֵ�м���������������

                KdPrint(("MouseLikeTouchPad_parse �ҵ��м� tp->nMouse_MButton_CurrentIndex=,%x\n", tp->nMouse_MButton_CurrentIndex));
                continue;//������������
            }
        }
    }


    KdPrint(("MouseLikeTouchPad_parse tp->nMouse_Pointer_CurrentIndex=,%x\n", tp->nMouse_Pointer_CurrentIndex));
    KdPrint(("MouseLikeTouchPad_parse tp->nMouse_Pointer_LastIndex=,%x\n", tp->nMouse_Pointer_LastIndex));

    if (tp->nMouse_Pointer_CurrentIndex != -1) {
        KdPrint(("MouseLikeTouchPad_parse tp->currentFinger.Contacts[tp->nMouse_Pointer_CurrentIndex].X=,%x\n", tp->currentFinger.Contacts[tp->nMouse_Pointer_CurrentIndex].X));
        KdPrint(("MouseLikeTouchPad_parse tp->currentFinger.Contacts[tp->nMouse_Pointer_CurrentIndex].Y=,%x\n", tp->currentFinger.Contacts[tp->nMouse_Pointer_CurrentIndex].Y));
    }
    if (tp->nMouse_Pointer_LastIndex != -1) {
        KdPrint(("MouseLikeTouchPad_parse tp->lastFinger.Contacts[tp->nMouse_MButton_LastIndex].X=,%x\n", tp->lastFinger.Contacts[tp->nMouse_MButton_LastIndex].X));
        KdPrint(("MouseLikeTouchPad_parse tp->lastFinger.Contacts[tp->nMouse_MButton_LastIndex].Y=,%x\n", tp->lastFinger.Contacts[tp->nMouse_MButton_LastIndex].Y));
    }


    if (tp->currentFinger.IsButtonClicked) {//��������������������,�л����ذ�������/����ģʽ���صȲ�������,��Ҫ�����뿪�ж�����Ϊ���������һֱ����ֱ���ͷ�
        tp->bPhysicalButtonUp = FALSE;//������Ƿ��ͷű�־
        KdPrint(("MouseLikeTouchPad_parse bPhysicalButtonUp FALSE,%x\n", FALSE));
        //׼�����ô�����������������ز���
        if (currentFinger_Count == 1) {//��ָ�ذ����ذ����½������Ϊ���ĺ��˹��ܼ�����ָ�ذ����ذ����½������Ϊ����ǰ�����ܼ�����ָ�ذ����ذ������м������Ϊ������������ȣ���/�е�/��3�������ȣ���
            if (tp->currentFinger.Contacts[0].ContactID == 0 && tp->currentFinger.Contacts[0].Confidence && tp->currentFinger.Contacts[0].TipSwitch\
                && tp->currentFinger.Contacts[0].Y > (tp->logicalMax_Y * 3 / 4) && tp->currentFinger.Contacts[0].X < tp->CornerX_LEFT) {//�׸����������������½�
                bMouse_BButton_Status = 1;//������ĺ��˼�����
            }
            else if (tp->currentFinger.Contacts[0].ContactID == 0 && tp->currentFinger.Contacts[0].Confidence && tp->currentFinger.Contacts[0].TipSwitch\
                && tp->currentFinger.Contacts[0].Y > (tp->logicalMax_Y * 3 / 4) && tp->currentFinger.Contacts[0].X > tp->CornerX_RIGHT) {//�׸����������������½�
                bMouse_FButton_Status = 1;//�������ǰ��������
            }
            else {//�л����DPI�����ȣ�����������ͷ�ʱִ���ж�

            }

        }
    }
    else {
        if (!tp->bPhysicalButtonUp) {
            tp->bPhysicalButtonUp = TRUE;
            KdPrint(("MouseLikeTouchPad_parse bPhysicalButtonUp TRUE,%x\n", TRUE));
            if (currentFinger_Count == 1) {//��ָ�ذ����ذ������м������Ϊ������������ȣ���/�е�/��3�������ȣ������ĺ���/ǰ�����ܼ�����Ҫ�жϻ��Զ��ͷ�)��

                //tp->currentFinger.Contacts[0].ContactID��һ��Ϊ0���Բ�����Ϊ�ж�����
                if (tp->currentFinger.Contacts[0].Confidence && tp->currentFinger.Contacts[0].TipSwitch\
                    && tp->currentFinger.Contacts[0].Y > (tp->logicalMax_Y * 3 / 4) && tp->currentFinger.Contacts[0].X > tp->CornerX_LEFT && tp->currentFinger.Contacts[0].X < tp->CornerX_RIGHT) {//�׸������������ڴ����������м�
                    //�л����DPI������
                    SetNextSensitivity(pDevContext);//ѭ������������
                }
            }
            else if (currentFinger_Count == 2) {//˫ָ�ذ����ذ����������ʱ����Ϊ����/�ر�˫ָ���ֹ���
                //������3ָ���ַ�ʽ��Ϊ�ж�����˫ָ�ȽӴ��Ĳ�������Ӵ�ʱ����ֵʹ���ӳ�̫�߲����ʲ��Ҳ���˫ָ��������,����Ϸ����ʹ�õ����ֹ��ܿ�ѡ��ر��л����Լ��󽵵�����Ϸʱ��������ʣ����Բ�ȡ�����رչ��ַ�������ճ���������Ϸ

                pDevContext->bWheelDisabled = !pDevContext->bWheelDisabled;
                KdPrint(("MouseLikeTouchPad_parse bWheelDisabled=,%x\n", pDevContext->bWheelDisabled));
                if (!pDevContext->bWheelDisabled) {//�������ֹ���ʱͬʱҲ�ָ�����ʵ�ַ�ʽΪ������˫ָ��������
                    pDevContext->bWheelScrollMode = FALSE;//Ĭ�ϳ�ʼֵΪ������˫ָ��������
                    KdPrint(("MouseLikeTouchPad_parse bWheelScrollMode=,%x\n", pDevContext->bWheelScrollMode));
                }

                KdPrint(("MouseLikeTouchPad_parse bPhysicalButtonUp currentFinger_Count=,%x\n", currentFinger_Count));
            }
            else if (currentFinger_Count == 3) {//��ָ�ذ����ذ����������ʱ����Ϊ�л�����ģʽbWheelScrollMode������������ʵ�ַ�ʽ��TRUEΪģ�������֣�FALSEΪ������˫ָ��������
                //��Ϊ�ճ��������ָ����ã����Թرչ��ֹ��ܵ�״̬�����浽ע����������������߻��Ѻ�ָ����ֹ���
                //��Ϊ������˫ָ�������ƵĹ���ģʽ�����ã�����ģ�����Ĺ���ģʽ״̬�����浽ע����������������߻��Ѻ�ָ���˫ָ�������ƵĹ���ģʽ
                pDevContext->bWheelScrollMode = !pDevContext->bWheelScrollMode;
                KdPrint(("MouseLikeTouchPad_parse bWheelScrollMode=,%x\n", pDevContext->bWheelScrollMode));

                //�л�����ʵ�ַ�ʽ��ͬʱҲ�������ֹ��ܷ����û�
                pDevContext->bWheelDisabled = FALSE;
                KdPrint(("MouseLikeTouchPad_parse bWheelDisabled=,%x\n", pDevContext->bWheelDisabled));


                KdPrint(("MouseLikeTouchPad_parse bPhysicalButtonUp currentFinger_Count=,%x\n", currentFinger_Count));
            }
            else if (currentFinger_Count == 4) {//��ָ��ѹ���ذ�������ʱΪ�л�3����ָ��ȴ�С���ò���Ч�������û���������м����ܡ�
                SetNextThumbScale(pDevContext); //��̬������ָͷ��С����

                KdPrint(("MouseLikeTouchPad_parse bPhysicalButtonUp currentFinger_Count=,%x\n", currentFinger_Count));
            }
            else if (currentFinger_Count == 5) {//��ָ��ѹ���ذ�������ʱ�л������ʽ��������windowsԭ���PTP��ȷʽ�����������ʽ
                //��Ϊԭ�津�ذ������ʽֻ����ʱʹ�����Բ����浽ע����������������߻��Ѻ�ָ��������ʽ������ģʽ
                // ԭ���PTP��ȷʽ�����������ʽʱ���ͱ����ڱ������ⲿִ�в���Ҫ�˷���Դ�������л��ط����ʽ������ģʽҲ�ڱ������ⲿ�ж�
                pDevContext->bMouseLikeTouchPad_Mode = FALSE;
                KdPrint(("MouseLikeTouchPad_parse bMouseLikeTouchPad_Mode=,%x\n", pDevContext->bMouseLikeTouchPad_Mode));

                KdPrint(("MouseLikeTouchPad_parse bPhysicalButtonUp currentFinger_Count=,%x\n", currentFinger_Count));
            }

        }
    }

    //��ʼ����¼��߼��ж�
    //ע�����ָ��ͬʱ���ٽӴ�������ʱ�����屨����ܴ���һ֡��ͬʱ��������������������Բ����õ�ǰֻ��һ����������Ϊ����ָ����ж�����
    if (tp->nMouse_Pointer_LastIndex == -1 && currentFinger_Count > 0) {//���ָ�롢������Ҽ����м���δ����,
        //ָ�봥����ѹ�����Ӵ��泤�����ֵ���������ж����ƴ����󴥺���������,ѹ��ԽС�Ӵ��泤�����ֵԽ�󡢳�����ֵԽС
        for (UCHAR i = 0; i < currentFinger_Count; i++) {
            //tp->currentFinger.Contacts[0].ContactID��һ��Ϊ0���Բ�����Ϊ�ж�����
            if (tp->currentFinger.Contacts[i].Confidence && tp->currentFinger.Contacts[i].TipSwitch\
                && tp->currentFinger.Contacts[i].Y > tp->StartY_TOP && tp->currentFinger.Contacts[i].X > tp->StartX_LEFT && tp->currentFinger.Contacts[i].X < tp->StartX_RIGHT) {//����������󴥺���������
                tp->nMouse_Pointer_CurrentIndex = i;  //�׸���������Ϊָ��
                tp->MousePointer_DefineTime = tp->current_Ticktime;//���嵱ǰָ����ʼʱ��

                KdPrint(("MouseLikeTouchPad_parse �׸���������Ϊָ��,%x\n", runtimes_IOREAD));
                break;
            }
        }
    }
    else if (tp->nMouse_Pointer_CurrentIndex == -1 && tp->nMouse_Pointer_LastIndex != -1) {//ָ����ʧ
        tp->bMouse_Wheel_Mode = FALSE;//��������ģʽ
        tp->bMouse_Wheel_Mode_JudgeEnable = TRUE;//���������б�

        tp->bGestureCompleted = TRUE;//����ģʽ����,��tp->bPtpReportCollection��Ҫ���ô���������������

        tp->nMouse_Pointer_CurrentIndex = -1;
        tp->nMouse_LButton_CurrentIndex = -1;
        tp->nMouse_RButton_CurrentIndex = -1;
        tp->nMouse_MButton_CurrentIndex = -1;
        tp->nMouse_Wheel_CurrentIndex = -1;

        KdPrint(("MouseLikeTouchPad_parse ָ����ʧ,%x\n", runtimes_IOREAD));
    }
    else if (tp->nMouse_Pointer_CurrentIndex != -1 && !tp->bMouse_Wheel_Mode) {  //ָ���Ѷ���ķǹ����¼�����
        //����ָ���������Ҳ��Ƿ�����ָ��Ϊ����ģʽ���߰���ģʽ����ָ�����/�Ҳ����ָ����ʱ����ָ����ָ����ʱ����С���趨��ֵʱ�ж�Ϊ�����ַ���Ϊ��갴������һ��������Ч���𰴼�����ֲ���,����갴���͹��ֲ���һ��ʹ��
        //���������������������������м����ܻ���ʳָ�����л���Ҫ̧��ʳָ����иı䣬���/�м�/�Ҽ����µ�����²���ת��Ϊ����ģʽ��
        LARGE_INTEGER MouseButton_Interval;
        MouseButton_Interval.QuadPart = (tp->current_Ticktime.QuadPart - tp->MousePointer_DefineTime.QuadPart) * tp->tick_Count / 10000;//��λms����
        float Mouse_Button_Interval = (float)MouseButton_Interval.LowPart;//ָ�����Ҳ����ָ����ʱ����ָ�붨����ʼʱ��ļ��ms

        if (currentFinger_Count > 1) {//��������������1����Ҫ�жϰ�������
            for (char i = 0; i < currentFinger_Count; i++) {
                if (i == tp->nMouse_Pointer_CurrentIndex || i == tp->nMouse_LButton_CurrentIndex || i == tp->nMouse_RButton_CurrentIndex || i == tp->nMouse_MButton_CurrentIndex || i == tp->nMouse_Wheel_CurrentIndex) {//iΪ��ֵ�����������������Ƿ�Ϊ-1
                    continue;  // �Ѿ����������
                }
                float dx = (float)(tp->currentFinger.Contacts[i].X - tp->currentFinger.Contacts[tp->nMouse_Pointer_CurrentIndex].X);
                float dy = (float)(tp->currentFinger.Contacts[i].Y - tp->currentFinger.Contacts[tp->nMouse_Pointer_CurrentIndex].Y);
                float distance = (float)sqrt(dx * dx + dy * dy);//��������ָ��ľ���

                BOOLEAN isWheel = FALSE;//����ģʽ����������ʼ�����ã�ע��bWheelDisabled��bMouse_Wheel_Mode_JudgeEnable�����ò�ͬ�����ܻ���
                if (!pDevContext->bWheelDisabled) {//���ֹ��ܿ���ʱ
                    // ָ�����Ҳ�����ָ���²�����ָ����ָ��ʼ����ʱ����С����ֵ��ָ�뱻��������ֹ��ֲ���ֻ���ж�һ��ֱ��ָ����ʧ���������������жϲ��ᱻʱ����ֵԼ��ʹ����Ӧ�ٶȲ���Ӱ��
                    isWheel = tp->bMouse_Wheel_Mode_JudgeEnable && fabs(distance) > tp->FingerMinDistance && fabs(distance) < tp->FingerMaxDistance && Mouse_Button_Interval < ButtonPointer_Interval_MSEC;
                }

                if (isWheel) {//����ģʽ��������
                    tp->bMouse_Wheel_Mode = TRUE;  //��������ģʽ
                    tp->bMouse_Wheel_Mode_JudgeEnable = FALSE;//�رչ����б�

                    tp->bGestureCompleted = FALSE; //���Ʋ���������־,��tp->bPtpReportCollection��Ҫ���ô���������������

                    tp->nMouse_Wheel_CurrentIndex = i;//���ָ����ο���ָ����ֵ
                    //��ָ�仯˲��ʱ���ݿ��ܲ��ȶ�ָ������ͻ����Ư����Ҫ����
                    tp->JitterFixStartTime = tp->current_Ticktime;//����������ʼ��ʱ
                    tp->Scroll_TotalDistanceX = 0;//�ۼƹ���λ��������
                    tp->Scroll_TotalDistanceY = 0;//�ۼƹ���λ��������


                    tp->nMouse_LButton_CurrentIndex = -1;
                    tp->nMouse_RButton_CurrentIndex = -1;
                    tp->nMouse_MButton_CurrentIndex = -1;

                    KdPrint(("MouseLikeTouchPad_parse ��������ģʽ,%x\n", runtimes_IOREAD));
                    break;
                }
                else {//ǰ�����ģʽ�����ж��Ѿ��ų������Բ���Ҫ������ָ����ָ��ʼ����ʱ������
                    if (tp->nMouse_MButton_CurrentIndex == -1 && fabs(distance) > tp->FingerMinDistance && fabs(distance) < tp->FingerClosedThresholdDistance && dx < 0) {//ָ������в�£����ָ����
                        bMouse_MButton_Status = 1; //�ҵ��м�
                        tp->nMouse_MButton_CurrentIndex = i;//��ֵ�м���������������

                        KdPrint(("MouseLikeTouchPad_parse �ҵ��м�,%x\n", runtimes_IOREAD));
                        continue;  //����������������ʳָ�Ѿ����м�ռ������ԭ��������Ѿ�������
                    }
                    else if (tp->nMouse_LButton_CurrentIndex == -1 && fabs(distance) > tp->FingerClosedThresholdDistance && fabs(distance) < tp->FingerMaxDistance && dx < 0) {//ָ������зֿ�����ָ����
                        bMouse_LButton_Status = 1; //�ҵ����
                        tp->nMouse_LButton_CurrentIndex = i;//��ֵ�����������������

                        KdPrint(("MouseLikeTouchPad_parse �ҵ����,%x\n", runtimes_IOREAD));
                        continue;  //��������������
                    }
                    else if (tp->nMouse_RButton_CurrentIndex == -1 && fabs(distance) > tp->FingerMinDistance && fabs(distance) < tp->FingerMaxDistance && dx > 0) {//ָ���Ҳ�����ָ����
                        bMouse_RButton_Status = 1; //�ҵ��Ҽ�
                        tp->nMouse_RButton_CurrentIndex = i;//��ֵ�Ҽ���������������

                        KdPrint(("MouseLikeTouchPad_parse �ҵ��Ҽ�,%x\n", runtimes_IOREAD));
                        continue;  //��������������
                    }
                }

            }
        }

        //���ָ��λ������
        if (currentFinger_Count != lastFinger_Count) {//��ָ�仯˲��ʱ���ݿ��ܲ��ȶ�ָ������ͻ����Ư����Ҫ����
            tp->JitterFixStartTime = tp->current_Ticktime;//����������ʼ��ʱ
            KdPrint(("MouseLikeTouchPad_parse ����������ʼ��ʱ,%x\n", runtimes_IOREAD));
        }
        else {
            LARGE_INTEGER FixTimer;
            FixTimer.QuadPart = (tp->current_Ticktime.QuadPart - tp->JitterFixStartTime.QuadPart) * tp->tick_Count / 10000;//��λms����
            float JitterFixTimer = (float)FixTimer.LowPart;//��ǰ����ʱ���ʱ

            float STABLE_INTERVAL;
            if (tp->nMouse_MButton_CurrentIndex != -1) {//�м�״̬����ָ��£�Ķ�������ֵ������
                STABLE_INTERVAL = STABLE_INTERVAL_FingerClosed_MSEC;
            }
            else {
                STABLE_INTERVAL = STABLE_INTERVAL_FingerSeparated_MSEC;
            }

            SHORT diffX = tp->currentFinger.Contacts[tp->nMouse_Pointer_CurrentIndex].X - tp->lastFinger.Contacts[tp->nMouse_Pointer_LastIndex].X;
            SHORT diffY = tp->currentFinger.Contacts[tp->nMouse_Pointer_CurrentIndex].Y - tp->lastFinger.Contacts[tp->nMouse_Pointer_LastIndex].Y;

            float px = (float)(diffX / tp->thumb_Scale);
            float py = (float)(diffY / tp->thumb_Scale);

            if (JitterFixTimer < STABLE_INTERVAL) {//�������ȶ�ǰ����
                if (tp->nMouse_LButton_CurrentIndex != -1 || tp->nMouse_RButton_CurrentIndex != -1 || tp->nMouse_MButton_CurrentIndex != -1) {//�а���ʱ��������ָ��ʱ����Ҫʹ��ָ�����ȷ
                    if (fabs(px) <= Jitter_Offset) {//ָ����΢��������
                        px = 0;
                    }
                    if (fabs(py) <= Jitter_Offset) {//ָ����΢��������
                        py = 0;
                    }
                }
            }

            double xx = pDevContext->MouseSensitivity_Value * px / tp->PointerSensitivity_x;
            double yy = pDevContext->MouseSensitivity_Value * py / tp->PointerSensitivity_y;
            mReport.dx = (CHAR)xx;
            mReport.dy = (CHAR)yy;

            if (fabs(xx) > 0.5 && fabs(xx) < 1) {//���پ�ϸ�ƶ�ָ������
                if (xx > 0) {
                    mReport.dx = 1;
                }
                else {
                    mReport.dx = -1;
                }

            }
            if (fabs(yy) > 0.5 && fabs(yy) < 1) {//���پ�ϸ�ƶ�ָ������
                if (xx > 0) {
                    mReport.dy = 1;
                }
                else {
                    mReport.dy = -1;
                }
            }

        }
    }
    else if (tp->nMouse_Pointer_CurrentIndex != -1 && tp->bMouse_Wheel_Mode) {//���ֲ���ģʽ��������˫ָ��������ָ��ָ����Ҳ��Ϊ��ģʽ�µ���������һ������״̬���ع������ж�ʹ��
        if (!pDevContext->bWheelScrollMode || currentFinger_Count > 2) {//������˫ָ��������ģʽ����ָ��ָ����Ҳ��Ϊ��ģʽ
            tp->bPtpReportCollection = TRUE;//����PTP�����弯�ϱ��棬����������һ���ж�
            KdPrint(("MouseLikeTouchPad_parse ����PTP�����弯�ϱ��棬����������һ���ж�,%x\n", runtimes_IOREAD));
        }
        else {
            //���ָ��λ������
            LARGE_INTEGER FixTimer;
            FixTimer.QuadPart = (tp->current_Ticktime.QuadPart - tp->JitterFixStartTime.QuadPart) * tp->tick_Count / 10000;//��λms����
            float JitterFixTimer = (float)FixTimer.LowPart;//��ǰ����ʱ���ʱ

            float px = (float)(tp->currentFinger.Contacts[tp->nMouse_Pointer_CurrentIndex].X - tp->lastFinger.Contacts[tp->nMouse_Pointer_LastIndex].X) / tp->thumb_Scale;
            float py = (float)(tp->currentFinger.Contacts[tp->nMouse_Pointer_CurrentIndex].Y - tp->lastFinger.Contacts[tp->nMouse_Pointer_LastIndex].Y) / tp->thumb_Scale;

            if (JitterFixTimer < STABLE_INTERVAL_FingerClosed_MSEC) {//ֻ���ڴ������ȶ�ǰ����
                if (fabs(px) <= Jitter_Offset) {//ָ����΢��������
                    px = 0;
                }
                if (fabs(py) <= Jitter_Offset) {//ָ����΢��������
                    py = 0;
                }
            }

            int direction_hscale = 1;//�����������ű���
            int direction_vscale = 1;//�����������ű���

            if (fabs(px) > fabs(py) / 4) {//���������ȶ�������
                direction_hscale = 1;
                direction_vscale = 8;
            }
            if (fabs(py) > fabs(px) / 4) {//���������ȶ�������
                direction_hscale = 8;
                direction_vscale = 1;
            }

            px = px / direction_hscale;
            py = py / direction_vscale;

            px = (float)(pDevContext->MouseSensitivity_Value * px / tp->PointerSensitivity_x);
            py = (float)(pDevContext->MouseSensitivity_Value * py / tp->PointerSensitivity_y);

            tp->Scroll_TotalDistanceX += px;//�ۼƹ���λ����
            tp->Scroll_TotalDistanceY += py;//�ۼƹ���λ����

            //�жϹ�����
            if (fabs(tp->Scroll_TotalDistanceX) > SCROLL_OFFSET_THRESHOLD_X) {//λ����������ֵ
                int h = (int)(fabs(tp->Scroll_TotalDistanceX) / SCROLL_OFFSET_THRESHOLD_X);
                mReport.h_wheel = (char)(tp->Scroll_TotalDistanceX > 0 ? h : -h);//��������

                float r = (float)(fabs(tp->Scroll_TotalDistanceX) - SCROLL_OFFSET_THRESHOLD_X * h);// ����λ������������ֵ
                tp->Scroll_TotalDistanceX = tp->Scroll_TotalDistanceX > 0 ? r : -r;//����λ��������
            }
            if (fabs(tp->Scroll_TotalDistanceY) > SCROLL_OFFSET_THRESHOLD_Y) {//λ����������ֵ
                int v = (int)(fabs(tp->Scroll_TotalDistanceY) / SCROLL_OFFSET_THRESHOLD_Y);
                mReport.v_wheel = (char)(tp->Scroll_TotalDistanceY > 0 ? v : -v);//��������

                float r = (float)(fabs(tp->Scroll_TotalDistanceY) - SCROLL_OFFSET_THRESHOLD_Y * v);// ����λ������������ֵ
                tp->Scroll_TotalDistanceY = tp->Scroll_TotalDistanceY > 0 ? r : -r;//����λ��������
            }
        }

    }
    else {
        //���������Ч
        KdPrint(("MouseLikeTouchPad_parse ���������Ч,%x\n", runtimes_IOREAD));
    }


    if (tp->bPtpReportCollection) {//�����弯�ϣ�����ģʽ�ж�
        if (!tp->bMouse_Wheel_Mode) {//��ָ����ָ�ͷ�Ϊ����ģʽ������־����һ֡bPtpReportCollection������FALSE����ֻ�ᷢ��һ�ι�������ƽ�������
            tp->bPtpReportCollection = FALSE;//PTP�����弯�ϱ���ģʽ����
            tp->bGestureCompleted = TRUE;//�������Ʋ����������ݺ�bMouse_Wheel_Mode���ֿ��ˣ���ΪbGestureCompleted���ܻ��bMouse_Wheel_Mode��ǰ����
            KdPrint(("MouseLikeTouchPad_parse bPtpReportCollection bGestureCompleted0,%x\n", status));

            //����ȫ����ָ�ͷŵ���ʱ���ݰ�,TipSwitch����㣬windows���Ʋ�������ʱ��Ҫ��ָ�뿪�ĵ�xy��������
            PTP_REPORT CompletedGestureReport;
            RtlCopyMemory(&CompletedGestureReport, &tp->currentFinger, sizeof(PTP_REPORT));
            for (int i = 0; i < currentFinger_Count; i++) {
                CompletedGestureReport.Contacts[i].TipSwitch = 0;
            }

            //����ptp����
            status = SendPtpMultiTouchReport(pDevContext, &CompletedGestureReport, sizeof(PTP_REPORT));
            if (!NT_SUCCESS(status)) {
                KdPrint(("MouseLikeTouchPad_parse SendPtpMultiTouchReport CompletedGestureReport failed,%x\n", status));
            }

        }
        else if (tp->bMouse_Wheel_Mode && currentFinger_Count == 1 && !tp->bGestureCompleted) {//����ģʽδ��������ʣ��ָ����ָ���ڴ�������,��Ҫ���bGestureCompleted��־�ж�ʹ�ù�������ƽ�������ֻ����һ��
            tp->bPtpReportCollection = FALSE;//PTP�����弯�ϱ���ģʽ����
            tp->bGestureCompleted = TRUE;//��ǰ�������Ʋ����������ݺ�bMouse_Wheel_Mode���ֿ��ˣ���ΪbGestureCompleted���ܻ��bMouse_Wheel_Mode��ǰ����
            KdPrint(("MouseLikeTouchPad_parse bPtpReportCollection bGestureCompleted1,%x\n", status));

            //����ָ����ָ�ͷŵ���ʱ���ݰ�,TipSwitch����㣬windows���Ʋ�������ʱ��Ҫ��ָ�뿪�ĵ�xy��������
            PTP_REPORT CompletedGestureReport2;
            RtlCopyMemory(&CompletedGestureReport2, &tp->currentFinger, sizeof(PTP_REPORT));
            CompletedGestureReport2.Contacts[0].TipSwitch = 0;

            //����ptp����
            status = SendPtpMultiTouchReport(pDevContext, &CompletedGestureReport2, sizeof(PTP_REPORT));
            if (!NT_SUCCESS(status)) {
                KdPrint(("MouseLikeTouchPad_parse SendPtpMultiTouchReport CompletedGestureReport2 failed,%x\n", status));
            }
        }

        if (!tp->bGestureCompleted) {//����δ�������������ͱ���
            KdPrint(("MouseLikeTouchPad_parse bPtpReportCollection bGestureCompleted2,%x\n", status));
            //����ptp����
            status = SendPtpMultiTouchReport(pDevContext, pPtpReport, sizeof(PTP_REPORT));
            if (!NT_SUCCESS(status)) {
                KdPrint(("MouseLikeTouchPad_parse SendPtpMultiTouchReport failed,%x\n", status));
            }
        }
    }
    else {//����MouseCollection
        mReport.button = bMouse_LButton_Status + (bMouse_RButton_Status << 1) + (bMouse_MButton_Status << 2) + (bMouse_BButton_Status << 3) + (bMouse_FButton_Status << 4);  //�����Һ���ǰ����״̬�ϳ�
        KdPrint(("MouseLikeTouchPad_parse SendPtpMouseReport mReport.report_id=,%x\n", mReport.report_id));
        KdPrint(("MouseLikeTouchPad_parse SendPtpMouseReport mReport.button=,%x\n", mReport.button));
        KdPrint(("MouseLikeTouchPad_parse SendPtpMouseReport mReport.dx=,%x\n", mReport.dx));
        KdPrint(("MouseLikeTouchPad_parse SendPtpMouseReport mReport.dy=,%x\n", mReport.dy));
        KdPrint(("MouseLikeTouchPad_parse SendPtpMouseReport mReport.v_wheel=,%x\n", mReport.v_wheel));
        KdPrint(("MouseLikeTouchPad_parse SendPtpMouseReport mReport.h_wheel=,%x\n", mReport.h_wheel));


        //������걨��
        status = SendPtpMouseReport(pDevContext, &mReport);
        if (!NT_SUCCESS(status)) {
            KdPrint(("MouseLikeTouchPad_parse SendPtpMouseReport failed,%x\n", status));
        }
    }


    //������һ�����д�����ĳ�ʼ���꼰���ܶ���������
    tp->lastFinger = tp->currentFinger;

    lastFinger_Count = currentFinger_Count;
    tp->nMouse_Pointer_LastIndex = tp->nMouse_Pointer_CurrentIndex;
    tp->nMouse_LButton_LastIndex = tp->nMouse_LButton_CurrentIndex;
    tp->nMouse_RButton_LastIndex = tp->nMouse_RButton_CurrentIndex;
    tp->nMouse_MButton_LastIndex = tp->nMouse_MButton_CurrentIndex;
    tp->nMouse_Wheel_LastIndex = tp->nMouse_Wheel_CurrentIndex;

    KdPrint(("MouseLikeTouchPad_parse end,%x\n", runtimes_IOREAD));

}


void MouseLikeTouchPad_parse_init(PDEVICE_CONTEXT pDevContext)
{
    PTP_PARSER* tp = &pDevContext->tp_settings;

    tp->nMouse_Pointer_CurrentIndex = -1; //���嵱ǰ���ָ�봥������������������ţ�-1Ϊδ����
    tp->nMouse_LButton_CurrentIndex = -1; //���嵱ǰ��������������������������ţ�-1Ϊδ����
    tp->nMouse_RButton_CurrentIndex = -1; //���嵱ǰ����Ҽ���������������������ţ�-1Ϊδ����
    tp->nMouse_MButton_CurrentIndex = -1; //���嵱ǰ����м���������������������ţ�-1Ϊδ����
    tp->nMouse_Wheel_CurrentIndex = -1; //���嵱ǰ�����ָ����ο���ָ��������������������ţ�-1Ϊδ����

    tp->nMouse_Pointer_LastIndex = -1; //�����ϴ����ָ�봥������������������ţ�-1Ϊδ����
    tp->nMouse_LButton_LastIndex = -1; //�����ϴ���������������������������ţ�-1Ϊδ����
    tp->nMouse_RButton_LastIndex = -1; //�����ϴ�����Ҽ���������������������ţ�-1Ϊδ����
    tp->nMouse_MButton_LastIndex = -1; //�����ϴ�����м���������������������ţ�-1Ϊδ����
    tp->nMouse_Wheel_LastIndex = -1; //�����ϴ������ָ����ο���ָ��������������������ţ�-1Ϊδ����

    pDevContext->bWheelDisabled = FALSE;//Ĭ�ϳ�ʼֵΪ�������ֲ�������
    pDevContext->bWheelScrollMode = FALSE;//Ĭ�ϳ�ʼֵΪ������˫ָ��������


    tp->bMouse_Wheel_Mode = FALSE;
    tp->bMouse_Wheel_Mode_JudgeEnable = TRUE;//���������б�

    tp->bGestureCompleted = FALSE; //���Ʋ���������־
    tp->bPtpReportCollection = FALSE;//Ĭ����꼯��

    RtlZeroMemory(&tp->lastFinger, sizeof(PTP_REPORT));
    RtlZeroMemory(&tp->currentFinger, sizeof(PTP_REPORT));

    tp->Scroll_TotalDistanceX = 0;
    tp->Scroll_TotalDistanceY = 0;

    tp->tick_Count = KeQueryTimeIncrement();
    KeQueryTickCount(&tp->last_Ticktime);

    tp->bPhysicalButtonUp = TRUE;
}


void SetNextThumbScale(PDEVICE_CONTEXT pDevContext)
{
    UCHAR ts_idx = pDevContext->ThumbScale_Index;// thumb_Scale_Normal;//thumb_Scale_Little//thumb_Scale_Big

    ts_idx++;
    if (ts_idx == 3) {//������ѭ������
        ts_idx = 0;
    }

    pDevContext->ThumbScale_Index = ts_idx;
    pDevContext->ThumbScale_Value = ThumbScaleTable[ts_idx];

    PTP_PARSER* tp = &pDevContext->tp_settings;
    //��̬������ָͷ��С����
    tp->thumb_Scale = (float)(pDevContext->ThumbScale_Value);//��ָͷ�ߴ����ű�����
    tp->FingerMinDistance = 12 * tp->TouchPad_DPMM_x * tp->thumb_Scale;//������Ч��������ָ��С����
    tp->FingerClosedThresholdDistance = 16 * tp->TouchPad_DPMM_x * tp->thumb_Scale;//����������ָ��£ʱ����С����
    tp->FingerMaxDistance = tp->FingerMinDistance * 4;//������Ч��������ָ������(FingerMinDistance*4) 

    KdPrint(("SetNextThumbScale pDevContext->ThumbScale_Index,%x\n", pDevContext->ThumbScale_Index));

}


NTSTATUS SetRegisterThumbScale(PDEVICE_CONTEXT pDevContext, ULONG ts_idx)//�������õ�ע���
{
    NTSTATUS status = STATUS_SUCCESS;

    status = SetRegConfig(pDevContext, L"ThumbScale_Index", ts_idx);
    if (!NT_SUCCESS(status)) {
        KdPrint(("SetRegisterThumbScale err,%x\n", status));
        return status;
    }

    KdPrint(("SetRegisterThumbScale ok,%x\n", status));
    return status;
}


NTSTATUS GetRegisterThumbScale(PDEVICE_CONTEXT pDevContext, ULONG* ts_idx)//��ע����ȡ����
{
    NTSTATUS status = STATUS_SUCCESS;
    *ts_idx = 0;

    status = GetRegConfig(pDevContext, L"ThumbScale_Index", ts_idx);
    if (!NT_SUCCESS(status)) {
        KdPrint(("GetRegisterThumbScale err,%x\n", status));
        return status;
    }

    KdPrint(("GetRegisterThumbScale ok,%x\n", status));
    return status;
}


void SetNextSensitivity(PDEVICE_CONTEXT pDevContext)
{
    UCHAR ms_idx = pDevContext->MouseSensitivity_Index;// MouseSensitivity_Normal;//MouseSensitivity_Slow//MouseSensitivity_FAST

    ms_idx++;
    if (ms_idx == 3) {//������ѭ������
        ms_idx = 0;
    }

    pDevContext->MouseSensitivity_Index = ms_idx;
    pDevContext->MouseSensitivity_Value = MouseSensitivityTable[ms_idx];
    KdPrint(("SetNextSensitivity pDevContext->MouseSensitivity_Index,%x\n", pDevContext->MouseSensitivity_Index));

}


NTSTATUS SetRegisterDeviceType(PDEVICE_CONTEXT pDevContext, ULONG dt_idx)//�������õ�ע���
{
    ////TouchpadDeviceType�����������ͣ������������󴥹��ܣ�
    //0 - TP�ʼǱ��������ô�����Built in���з��󴥹��ܣ�
    //    1 - ���ö���������External TouchPad���޷��󴥹��ܣ�
    //    2 - ���ô��������External TouchPad Keyboard���з��󴥹��ܣ�

    NTSTATUS status = STATUS_SUCCESS;

    status = SetRegConfig(pDevContext, L"DeviceType_Index", dt_idx);
    if (!NT_SUCCESS(status)) {
        KdPrint(("SetRegisterDeviceType err,%x\n", status));
        return status;
    }

    KdPrint(("SetRegisterDeviceType ok,%x\n", status));
    return status;
}


NTSTATUS GetRegisterDeviceType(PDEVICE_CONTEXT pDevContext, ULONG* dt_idx)//��ע����ȡ����
{
    NTSTATUS status = STATUS_SUCCESS;
    *dt_idx = 0;

    status = GetRegConfig(pDevContext, L"DeviceType_Index", dt_idx);
    if (!NT_SUCCESS(status)) {
        KdPrint(("GetRegisterDeviceType err,%x\n", status));
        return status;
    }

    KdPrint(("GetRegisterDeviceType ok,%x\n", status));
    return status;
}


NTSTATUS SetRegisterSpaceLayout(PDEVICE_CONTEXT pDevContext, ULONG sl_idx)//�������õ�ע���
{
    //TouchpadSpaceLayout(��������Կո������λ�ò�����ƣ��������󴥵�������㣩
    //    0 - ���в���CenterAlign
    //    1 - ƫ��5mm���RightAlign5
    //    2 - ƫ��10mm���RightAlign10

    NTSTATUS status = STATUS_SUCCESS;

    status = SetRegConfig(pDevContext, L"SpaceLayout_Index", sl_idx);
    if (!NT_SUCCESS(status)) {
        KdPrint(("SetRegisterSpaceLayout err,%x\n", status));
        return status;
    }

    KdPrint(("SetRegisterSpaceLayout ok,%x\n", status));
    return status;
}


NTSTATUS GetRegisterSpaceLayout(PDEVICE_CONTEXT pDevContext, ULONG* sl_idx)//��ע����ȡ����
{
    NTSTATUS status = STATUS_SUCCESS;
    *sl_idx = 0;

    status = GetRegConfig(pDevContext, L"SpaceLayout_Index", sl_idx);
    if (!NT_SUCCESS(status)) {
        KdPrint(("GetRegisterSpaceLayout err,%x\n", status));
        return status;
    }

    KdPrint(("GetRegisterSpaceLayout ok,%x\n", status));
    return status;
}
