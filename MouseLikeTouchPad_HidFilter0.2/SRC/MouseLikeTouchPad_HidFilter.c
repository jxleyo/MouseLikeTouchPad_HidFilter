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

    deviceContext->bMouseLikeTouchPad_Mode = TRUE;//默认初始值为仿鼠标触摸板操作方式

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

    //保存注册表灵敏度设置数值
    status = SetRegisterMouseSensitivity(deviceContext, deviceContext->MouseSensitivity_Index);//MouseSensitivityTable存储表的序号值
    if (!NT_SUCCESS(status))
    {
        KdPrint(("PtpFilterDeviceD0Exit SetRegisterMouseSensitivity err,%x\n", status));
    }

        //保存注册表灵敏度设置数值
    status = SetRegisterThumbScale(deviceContext, deviceContext->ThumbScale_Index);//ThumbScaleTable存储表的序号值
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

	hidDescriptorSize = DefaultHidDescriptor.bLength; //deviceContext->PtpHidDesc.bLength蓝牙连接的描述符长度错误，需要用标准的DefaultHidDescriptor.bLength;
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
    pDeviceAttributes->VendorID = 0;//deviceContext->VendorID;赋值0使得标准触摸板设备的硬件id不被引入到驱动新生成的标准触摸板设备id中，防止多次安装驱动嵌套现象。 
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
    PVOID pHidReportDesc = (PVOID) ParallelMode_PtpReportDescriptor;// deviceContext->pPtpHidReportDesc;//(PVOID) ParallelMode_PtpReportDescriptor;注意不用取地址

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
            hidPacket->reportId = deviceContext->REPORTID_INPUT_MODE;//hidPacket、DeviceInputMode都要复制才有效

			PPTP_DEVICE_INPUT_MODE_REPORT DeviceInputMode = (PPTP_DEVICE_INPUT_MODE_REPORT)hidPacket->reportBuffer;
            DeviceInputMode->ReportID = deviceContext->REPORTID_INPUT_MODE;//hidPacket、DeviceInputMode都要复制才有效

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
            hidPacket->reportId = deviceContext->REPORTID_FUNCTION_SWITCH;//hidPacket、InputSelection都要复制才有效

			PPTP_DEVICE_SELECTIVE_REPORT_MODE_REPORT InputSelection = (PPTP_DEVICE_SELECTIVE_REPORT_MODE_REPORT)hidPacket->reportBuffer;
            InputSelection->ReportID = deviceContext->REPORTID_FUNCTION_SWITCH;//hidPacket、InputSelection都要复制才有效
            InputSelection->ButtonReport = 0x1;//
            InputSelection->SurfaceReport = 0x1;//
			deviceContext->PtpReportButton = InputSelection->ButtonReport;
			deviceContext->PtpReportTouch = InputSelection->SurfaceReport;

			break;
		}
        case FAKE_REPORTID_LATENCY_MODE:
        {
            KdPrint(("PtpFilterSetHidFeatures FAKE_REPORTID_LATENCY_MODE,%x\n", status));
            hidPacket->reportId = deviceContext->REPORTID_LATENCY_MODE;//hidPacket、latency_mode都要复制才有效

            PPTP_DEVICE_LATENCY_MODE_REPORT latency_mode = (PPTP_DEVICE_LATENCY_MODE_REPORT)hidPacket->reportBuffer;
            latency_mode->ReportID = deviceContext->REPORTID_LATENCY_MODE;//hidPacket、latency_mode都要复制才有效
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

    if (deviceContext->VendorID == 0x6cb) {//synaptic触摸板设备vendorID，lenovo yoga 14s 2021 laptops I2C HID
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
    if (!deviceContext->PtpInputModeOn) {//输入集合异常模式下  
        ////发送原始报告
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


    if (!deviceContext->bMouseLikeTouchPad_Mode) {//原版触控板操作方式直接发送原始报告
        PTP_PARSER* tps = &deviceContext->tp_settings;
        if (ptpReport.IsButtonClicked) {
            //需要进行离开判定，否则本次或下次进入MouseLikeTouchPad解析器后关系bPhysicalButtonUp还会被内部的代码段执行造成未知问题
            tps->bPhysicalButtonUp = FALSE;
            KdPrint(("PtpFilterInputRequestCompletionCallback bPhysicalButtonUp FALSE,%x\n", FALSE));
        }
        else {
            if (!tps->bPhysicalButtonUp) {
                tps->bPhysicalButtonUp = TRUE;
                KdPrint(("PtpFilterInputRequestCompletionCallback bPhysicalButtonUp TRUE,%x\n", TRUE));

                if (ptpReport.ContactCount == 5 && !deviceContext->bMouseLikeTouchPad_Mode) {//五指按压触控板物理按键时，切换回仿鼠标式触摸板模式，
                    deviceContext->bMouseLikeTouchPad_Mode = TRUE;
                    KdPrint(("PtpFilterInputRequestCompletionCallback bMouseLikeTouchPad_Mode TRUE,%x\n", status));

                    //切换回仿鼠标式触摸板模式的同时也恢复滚轮功能和实现方式
                    deviceContext->bWheelDisabled = FALSE;
                    KdPrint(("PtpFilterInputRequestCompletionCallback bWheelDisabled=,%x\n", deviceContext->bWheelDisabled));
                    deviceContext->bWheelScrollMode = FALSE;
                    KdPrint(("PtpFilterInputRequestCompletionCallback bWheelScrollMode=,%x\n", deviceContext->bWheelScrollMode));
                }
            }
        }

        //windows原版的PTP精确式触摸板操作方式，直接发送PTP报告
        status = SendPtpMultiTouchReport(deviceContext, &ptpReport, sizeof(PTP_REPORT));
        if (!NT_SUCCESS(status)) {
            KdPrint(("PtpFilterInputRequestCompletionCallback SendPtpMultiTouchReport ptpReport failed,%x\n", status));
        }

    }
    else {
        //MouseLikeTouchPad解析器
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
    BYTE lastCollection = 0;//改变量能够用于准确判定PTP、MOUSE集合输入报告的reportID
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

            //下面3个Tlc状态更新是有必要的，可以防止后续相关集合Tlc错误判定发生
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

            //这里计算单个报告数据包的手指数量用来后续判断报告模式及bHybrid_ReportingMode的赋值
            pDevContext->DeviceDescriptorFingerCount++;
            KdPrint(("AnalyzeHidReportDescriptor DeviceDescriptorFingerCount=,%x\n", pDevContext->DeviceDescriptorFingerCount));
        }
        else if (inMouseTlc && depth == 2 && lastCollection == HID_USAGE_GENERIC_MOUSE && lastUsage == HID_USAGE_GENERIC_POINTER) {
            //下层的Mouse集合report本驱动并不会读取，只是作为输出到上层类驱动的Report使用
            pDevContext->REPORTID_MOUSE_COLLECTION = reportId;
            KdPrint(("AnalyzeHidReportDescriptor REPORTID_MOUSE_COLLECTION=,%x\n", pDevContext->REPORTID_MOUSE_COLLECTION));
        }
        else if (inConfigTlc && type == HID_TYPE_FEATURE && lastUsage == HID_USAGE_INPUT_MODE) {
            pDevContext->REPORTSIZE_INPUT_MODE = (reportSize + 7) / 8;//报告数据总长度
            pDevContext->REPORTID_INPUT_MODE = reportId;
            KdPrint(("AnalyzeHidReportDescriptor REPORTID_INPUT_MODE=,%x\n", pDevContext->REPORTID_INPUT_MODE));
            KdPrint(("AnalyzeHidReportDescriptor REPORTSIZE_INPUT_MODE=,%x\n", pDevContext->REPORTSIZE_INPUT_MODE));
            continue;
        }
        else if (inConfigTlc && type == HID_TYPE_FEATURE && lastUsage == HID_USAGE_SURFACE_SWITCH || lastUsage == HID_USAGE_BUTTON_SWITCH) {
            //默认标准规范为HID_USAGE_SURFACE_SWITCH与HID_USAGE_BUTTON_SWITCH各1bit组合低位成1个字节HID_USAGE_FUNCTION_SWITCH报告
            pDevContext->REPORTSIZE_FUNCTION_SWITCH = (reportSize + 7) / 8;//报告数据总长度
            pDevContext->REPORTID_FUNCTION_SWITCH = reportId;
            KdPrint(("AnalyzeHidReportDescriptor REPORTID_FUNCTION_SWITCH=,%x\n", pDevContext->REPORTID_FUNCTION_SWITCH));
            KdPrint(("AnalyzeHidReportDescriptor REPORTSIZE_FUNCTION_SWITCH=,%x\n", pDevContext->REPORTSIZE_FUNCTION_SWITCH));
            continue;
        }
        else if (inTouchTlc && type == HID_TYPE_FEATURE && lastUsage == HID_USAGE_CONTACT_COUNT_MAXIMUM || lastUsage == HID_USAGE_PAD_TYPE) {
            //默认标准规范为HID_USAGE_CONTACT_COUNT_MAXIMUM与HID_USAGE_PAD_TYPE各4bit组合低位成1个字节HID_USAGE_DEVICE_CAPS报告
            pDevContext->REPORTSIZE_DEVICE_CAPS = (reportSize + 7) / 8;//报告数据总长度
            pDevContext->REPORTID_DEVICE_CAPS = reportId;
            KdPrint(("AnalyzeHidReportDescriptor REPORTSIZE_DEVICE_CAPS=,%x\n", pDevContext->REPORTSIZE_DEVICE_CAPS));
            KdPrint(("AnalyzeHidReportDescriptor REPORTID_DEVICE_CAPS=,%x\n", pDevContext->REPORTID_DEVICE_CAPS));
            continue;
        }
        else if (inTouchTlc && type == HID_TYPE_FEATURE && lastUsage == HID_USAGE_LATENCY_MODE) {
            //延迟模式功能报告//默认标准规范为HID_USAGE_LATENCY_MODE低位1bit组合成1个字节HID_USAGE_LATENCY_MODE报告
            pDevContext->REPORTSIZE_LATENCY_MODE = (reportSize + 7) / 8;//报告数据总长度
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

    //判断触摸板报告模式
    if (pDevContext->DeviceDescriptorFingerCount < 4) {//4个手指数据以下
        //Single finger hybrid reporting mode单指混合报告模式确认，驱动不予支持
        KdPrint(("AnalyzeHidReportDescriptor bHybrid_ReportingMode Confirm,%x\n", pDevContext->DeviceDescriptorFingerCount));
        return STATUS_UNSUCCESSFUL;//返回后终止驱动程序
    }


    //计算保存触摸板尺寸分辨率等参数
    //转换为mm长度单位
    if (tp->unit == 0x11) {//cm长度单位
        tp->physical_Width_mm = tp->physicalMax_X * pow(10.0, tp->unitExp) * 10;
        tp->physical_Height_mm = tp->physicalMax_Y * pow(10.0, tp->unitExp) * 10;
    }
    else {//0x13为inch长度单位
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

    tp->TouchPad_DPMM_x = (float)(tp->logicalMax_X / tp->physical_Width_mm);//单位为dot/mm
    tp->TouchPad_DPMM_y = (float)(tp->logicalMax_Y / tp->physical_Height_mm);//单位为dot/mm
    KdPrint(("AnalyzeHidReportDescriptor TouchPad_DPMM_x=,%x\n", (ULONG)tp->TouchPad_DPMM_x));
    KdPrint(("AnalyzeHidReportDescriptor TouchPad_DPMM_y=,%x\n", (ULONG)tp->TouchPad_DPMM_y));
 

    KdPrint(("AnalyzeHidReportDescriptor end,%x\n", status));
    return status;
}



NTSTATUS SetRegisterMouseSensitivity(PDEVICE_CONTEXT pDevContext, ULONG ms_idx)//保存设置到注册表
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


NTSTATUS GetRegisterMouseSensitivity(PDEVICE_CONTEXT pDevContext, ULONG *ms_idx)//获取设置从注册表
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

    //初始化注册表项
    UNICODE_STRING stringKey;
    RtlInitUnicodeString(&stringKey, L"\\Registry\\Machine\\Software\\Microsoft\\Windows\\CurrentVersion\\PrecisionTouchPad");

    //初始化OBJECT_ATTRIBUTES结构
    OBJECT_ATTRIBUTES  ObjectAttributes;
    InitializeObjectAttributes(&ObjectAttributes, &stringKey, OBJ_CASE_INSENSITIVE, NULL, NULL);//OBJ_CASE_INSENSITIVE对大小写敏感

    //创建注册表项
    HANDLE hKey;
    ULONG Des;
    status = ZwCreateKey(&hKey, KEY_ALL_ACCESS, &ObjectAttributes, 0, NULL, REG_OPTION_NON_VOLATILE, &Des);
    if (NT_SUCCESS(status))
    {
        if (Des == REG_CREATED_NEW_KEY)
        {
            KdPrint(("新建注册表项！\n"));
        }
        else
        {
            KdPrint(("要创建的注册表项已经存在！\n"));
        }
    }
    else {
        return status;
    }


    // 数字转为字符串
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


    //设置REG_DWORD键值
    status = ZwSetValueKey(hKey, &strKeyName, 0, REG_DWORD, &nConfigValue, 4);
    if (!NT_SUCCESS(status))
    {
        KdPrint(("设置REG_DWORD键值失败！\n"));
    }

    ZwFlushKey(hKey);
    //关闭注册表句柄
    ZwClose(hKey);
    return status;
}


NTSTATUS GetRegConfig(PDEVICE_CONTEXT pDevContext, WCHAR* strConfigName, PULONG pConfigValue)//GetRegConfig(L"_MouseSensitivity_Index",&nConfigValue);
{
    NTSTATUS status = STATUS_SUCCESS;

    //初始化注册表项
    UNICODE_STRING stringKey;
    RtlInitUnicodeString(&stringKey, L"\\Registry\\Machine\\Software\\Microsoft\\Windows\\CurrentVersion\\PrecisionTouchPad");

    //初始化OBJECT_ATTRIBUTES结构
    OBJECT_ATTRIBUTES  ObjectAttributes;
    InitializeObjectAttributes(&ObjectAttributes, &stringKey, OBJ_CASE_INSENSITIVE, NULL, NULL);//OBJ_CASE_INSENSITIVE对大小写敏感

    //创建注册表项
    HANDLE hKey;
    ULONG Des;
    status = ZwCreateKey(&hKey, KEY_ALL_ACCESS, &ObjectAttributes, 0, NULL, REG_OPTION_NON_VOLATILE, &Des);
    if (NT_SUCCESS(status))
    {
        if (Des == REG_CREATED_NEW_KEY)
        {
            KdPrint(("新建注册表项！\n"));
        }
        else
        {
            KdPrint(("要创建的注册表项已经存在！\n"));
        }
    }
    else {
        return STATUS_UNSUCCESSFUL;
    }


    // 数字转为字符串
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


    //初始化
    ULONG RetSize = 0;

    //第一次调用ZwQueryValueKey  来获取KeyValuePartialInformation类型的大小 然后分配一个buffer
    status = ZwQueryValueKey(hKey, &strKeyName, KeyValuePartialInformation, NULL, 0, &RetSize);
    //运行正确这个函数返回的是STATUS_BUFFER_TOO_SMALL  不能使用NT_SUCCESS来判断
    if (status == STATUS_OBJECT_NAME_NOT_FOUND || status == STATUS_INVALID_PARAMETER || RetSize == 0)
    {
        DbgPrint("查询键值类型失败");
        ZwClose(hKey);
        return status;
    }

    //分配一块内存来接收返回的信息
    PKEY_VALUE_PARTIAL_INFORMATION pvpi = (PKEY_VALUE_PARTIAL_INFORMATION)ExAllocatePool(PagedPool, RetSize);
    if (!pvpi)
    {
        KdPrint(("内存分配失败"));
        ZwClose(hKey);
        return status;
    }
    //查询信息，类型为PKEY_VALUE_PARTIAL_INFORMATION
    status = ZwQueryValueKey(hKey, &strKeyName, KeyValuePartialInformation, pvpi, RetSize, &RetSize);

    if (!NT_SUCCESS(status))
    {
        KdPrint(("查询键值失败\n"));

        return status;
    }
    else
    {
        switch (pvpi->Type)
        {
        case REG_DWORD://获取REG_DWORD键值
            *pConfigValue = *((PULONG)(&pvpi->Data[0]));
            KdPrint(("查询键值为%x\n",*pConfigValue));
            break;
        default:
            KdPrint(("未解析的键值\n"));
            status = STATUS_UNSUCCESSFUL;
            break;
        }

    }

    ZwFlushKey(hKey);
    //关闭注册表句柄
    ZwClose(hKey);
    return status;

}


VOID init(PDEVICE_CONTEXT pDevContext) {
    NTSTATUS status = STATUS_SUCCESS;


    //读取设备类型设置
    pDevContext->DeviceType_Index = 1;

    ULONG dt_idx;
    status = GetRegisterDeviceType(pDevContext, &dt_idx);
    if (!NT_SUCCESS(status))
    {
        KdPrint(("init GetRegisterDeviceType err,%x\n", status));
        status = SetRegisterDeviceType(pDevContext, pDevContext->DeviceType_Index);//初始默认设置
        if (!NT_SUCCESS(status)) {
            KdPrint(("init SetRegisterDeviceType err,%x\n", status));
        }
    }
    else {
        if (dt_idx > 2) {//如果读取的数值错误
            dt_idx = pDevContext->DeviceType_Index;//恢复初始默认值
        }
        pDevContext->DeviceType_Index = (UCHAR)dt_idx;
        KdPrint(("init GetRegisterDeviceType DeviceType_Index=,%x\n", pDevContext->DeviceType_Index));
    }


    //读取触摸板相对空格键对齐位置布局设置
    pDevContext->SpaceLayout_Index = 1;

    ULONG sl_idx;
    status = GetRegisterSpaceLayout(pDevContext, &sl_idx);
    if (!NT_SUCCESS(status))
    {
        KdPrint(("init GetRegisterSpaceLayout err,%x\n", status));
        status = SetRegisterSpaceLayout(pDevContext, pDevContext->SpaceLayout_Index);//初始默认设置
        if (!NT_SUCCESS(status)) {
            KdPrint(("init SetRegisterSpaceLayout err,%x\n", status));
        }
    }
    else {
        if (sl_idx > 2) {//如果读取的数值错误
            sl_idx = pDevContext->SpaceLayout_Index;//恢复初始默认值
        }
        pDevContext->SpaceLayout_Index = (UCHAR)sl_idx;
        KdPrint(("init GetRegisterSpaceLayout DeviceType_Index=,%x\n", pDevContext->DeviceType_Index));
    }


    //读取鼠标灵敏度设置
    pDevContext->MouseSensitivity_Index = 1;//默认初始值为MouseSensitivityTable存储表的序号1项
    pDevContext->MouseSensitivity_Value = MouseSensitivityTable[pDevContext->MouseSensitivity_Index];//默认初始值为1.0

    ULONG ms_idx;
    status = GetRegisterMouseSensitivity(pDevContext, &ms_idx);
    if (!NT_SUCCESS(status))
    {
        KdPrint(("init GetRegisterMouseSensitivity err,%x\n", status));
        status = SetRegisterMouseSensitivity(pDevContext, pDevContext->MouseSensitivity_Index);//初始默认设置
        if (!NT_SUCCESS(status)) {
            KdPrint(("init SetRegisterMouseSensitivity err,%x\n", status));
        }
    }
    else {
        if (ms_idx > 2) {//如果读取的数值错误
            ms_idx = pDevContext->MouseSensitivity_Index;//恢复初始默认值
        }
        pDevContext->MouseSensitivity_Index = (UCHAR)ms_idx;
        pDevContext->MouseSensitivity_Value = MouseSensitivityTable[pDevContext->MouseSensitivity_Index];
        KdPrint(("init GetRegisterMouseSensitivity MouseSensitivity_Index=,%x\n", pDevContext->MouseSensitivity_Index));
    }



    //读取指头大小设置
    pDevContext->ThumbScale_Index = 1;
    pDevContext->ThumbScale_Value = ThumbScaleTable[pDevContext->ThumbScale_Index];

    ULONG ts_idx;
    status = GetRegisterThumbScale(pDevContext, &ts_idx);
    if (!NT_SUCCESS(status))
    {
        KdPrint(("init GetRegisterThumbScale err,%x\n", status));
        status = SetRegisterThumbScale(pDevContext, pDevContext->ThumbScale_Index);//初始默认设置
        if (!NT_SUCCESS(status)) {
            KdPrint(("init SetRegisterThumbScale err,%x\n", status));
        }
    }
    else {
        if (ts_idx > 2) {//如果读取的数值错误
            ts_idx = pDevContext->ThumbScale_Index;//恢复初始默认值
        }
        pDevContext->ThumbScale_Index = (UCHAR)ts_idx;
        pDevContext->ThumbScale_Value = ThumbScaleTable[pDevContext->ThumbScale_Index];
        KdPrint(("init GetRegisterThumbScale ThumbScale_Index=,%x\n", pDevContext->ThumbScale_Index));
    }

    //

    PTP_PARSER* tp = &pDevContext->tp_settings;
    //动态调整手指头大小常量
    tp->thumb_Scale = pDevContext->ThumbScale_Index;//手指头尺寸缩放比例，
    tp->FingerMinDistance = 12 * tp->TouchPad_DPMM_x * tp->thumb_Scale;//定义有效的相邻手指最小距离
    tp->FingerClosedThresholdDistance = 16 * tp->TouchPad_DPMM_x * tp->thumb_Scale;//定义相邻手指合拢时的最小距离
    tp->FingerMaxDistance = tp->FingerMinDistance * 4;//定义有效的相邻手指最大距离(FingerMinDistance*4) 

    tp->PointerSensitivity_x = tp->TouchPad_DPMM_x / 25;
    tp->PointerSensitivity_y = tp->TouchPad_DPMM_y / 25;

    //
    ULONG Offset = 0;
    ULONG SpaceCenterline;
    ULONG HalfWidthX;
    ULONG CornerWidth;

    //TouchpadSpaceLayout(触摸板相对空格键对齐位置布局设计，关联防误触的区域计算）
    if (pDevContext->SpaceLayout_Index == 0) {//居中布局CenterAlign
        Offset = 0;//触摸板中心线较空格键中心线对齐，
    }
    else if (pDevContext->SpaceLayout_Index == 1) {//1-偏右5mm设计RightAlign5
        Offset = (ULONG)(5 * tp->TouchPad_DPMM_x);//触摸板中心线较空格键中心线偏右5mm，
    }
    else if (pDevContext->SpaceLayout_Index == 2) {//2-偏右10mm设计RightAlign10
        //触摸板中心线较空格键中心线偏右10mm，然后不同的触摸板尺寸以此中心线为对称轴设计位置布局，DisabledX_RIGHT触摸板右侧区域防误触范围更大，
        Offset = (LONG)(10 * tp->TouchPad_DPMM_x);//触摸板中心线较空格键中心线偏左10mm，
    }

    SpaceCenterline = (ULONG)(40 * tp->TouchPad_DPMM_x);//起点误触竖线X值为距离空格键中心线左右侧40mm处的X坐标,
    HalfWidthX = tp->logicalMax_X / 2;//触摸板一半宽度数值
    CornerWidth = (ULONG)(10 * tp->TouchPad_DPMM_x);//触摸板边角功能键区域宽度10mm，

    //触摸板边角功能键区域坐标
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


    if (pDevContext->DeviceType_Index == 1) {//外置独立触摸板无需防误触功能
        tp->StartY_TOP = 0;
        tp->StartX_LEFT = 0;
        tp->StartX_RIGHT = tp->logicalMax_X;
    }
    else {
        tp->StartY_TOP = (ULONG)(10 * tp->TouchPad_DPMM_y);////起点误触横线Y值为距离触摸板顶部10mm处的Y坐标

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

    //计算报告频率和时间间隔
    KeQueryTickCount(&tp->current_Ticktime);
    tp->ticktime_Interval.QuadPart = (tp->current_Ticktime.QuadPart - tp->last_Ticktime.QuadPart) * tp->tick_Count / 10000;//单位ms毫秒
    tp->TouchPad_ReportInterval = (float)tp->ticktime_Interval.LowPart;//触摸板报告间隔时间ms
    tp->last_Ticktime = tp->current_Ticktime;


    //保存当前手指坐标
    tp->currentFinger = *pPtpReport;
    UCHAR currentFinger_Count = tp->currentFinger.ContactCount;//当前触摸点数量
    UCHAR lastFinger_Count = tp->lastFinger.ContactCount; //上次触摸点数量
    KdPrint(("MouseLikeTouchPad_parse currentFinger_Count=,%x\n", currentFinger_Count));
    KdPrint(("MouseLikeTouchPad_parse lastFinger_Count=,%x\n", lastFinger_Count));

    UCHAR MAX_CONTACT_FINGER = PTP_MAX_CONTACT_POINTS;
    BOOLEAN allFingerDetached = TRUE;
    for (UCHAR i = 0; i < MAX_CONTACT_FINGER; i++) {//所有TipSwitch为0时判定为手指全部离开，因为最后一个点离开时ContactCount和Confidence始终为1不会置0。
        if (tp->currentFinger.Contacts[i].TipSwitch) {
            allFingerDetached = FALSE;
            currentFinger_Count = tp->currentFinger.ContactCount;//重新定义当前触摸点数量

            KdPrint(("MouseLikeTouchPad_parse allFingerDetached = FALSE,%x\n", runtimes_IOREAD));
            break;
        }
    }
    if (allFingerDetached) {
        currentFinger_Count = 0;
        KdPrint(("MouseLikeTouchPad_parse 手指全部离开,%x\n", runtimes_IOREAD));
    }


    //初始化鼠标事件
    struct mouse_report_t mReport;
    mReport.report_id = FAKE_REPORTID_MOUSE;//FAKE_REPORTID_MOUSE//pDevContext->REPORTID_MOUSE_COLLECTION

    mReport.button = 0;
    mReport.dx = 0;
    mReport.dy = 0;
    mReport.h_wheel = 0;
    mReport.v_wheel = 0;

    BOOLEAN bMouse_LButton_Status = 0; //定义临时鼠标左键状态，0为释放，1为按下，每次都需要重置确保后面逻辑
    BOOLEAN bMouse_MButton_Status = 0; //定义临时鼠标中键状态，0为释放，1为按下，每次都需要重置确保后面逻辑
    BOOLEAN bMouse_RButton_Status = 0; //定义临时鼠标右键状态，0为释放，1为按下，每次都需要重置确保后面逻辑
    BOOLEAN bMouse_BButton_Status = 0; //定义临时鼠标Back后退键状态，0为释放，1为按下，每次都需要重置确保后面逻辑
    BOOLEAN bMouse_FButton_Status = 0; //定义临时鼠标Forward前进键状态，0为释放，1为按下，每次都需要重置确保后面逻辑

    //初始化当前触摸点索引号，跟踪后未再赋值的表示不存在了
    tp->nMouse_Pointer_CurrentIndex = -1;
    tp->nMouse_LButton_CurrentIndex = -1;
    tp->nMouse_RButton_CurrentIndex = -1;
    tp->nMouse_MButton_CurrentIndex = -1;
    tp->nMouse_Wheel_CurrentIndex = -1;


    //所有手指触摸点的索引号跟踪
    for (char i = 0; i < currentFinger_Count; i++) {
        if (!tp->currentFinger.Contacts[i].Confidence || !tp->currentFinger.Contacts[i].TipSwitch) {//必须判断Confidence和TipSwitch才是有效触摸点数据     
            continue;
        }

        if (tp->nMouse_Pointer_LastIndex != -1) {
            if (tp->lastFinger.Contacts[tp->nMouse_Pointer_LastIndex].ContactID == tp->currentFinger.Contacts[i].ContactID) {
                tp->nMouse_Pointer_CurrentIndex = i;//找到指针

                KdPrint(("MouseLikeTouchPad_parse 找到指针 tp->nMouse_Pointer_CurrentIndex=,%x\n", tp->nMouse_Pointer_CurrentIndex));
                continue;//查找其他功能
            }
        }

        if (tp->nMouse_Wheel_LastIndex != -1) {
            if (tp->lastFinger.Contacts[tp->nMouse_Wheel_LastIndex].ContactID == tp->currentFinger.Contacts[i].ContactID) {
                tp->nMouse_Wheel_CurrentIndex = i;//找到滚轮辅助键

                KdPrint(("MouseLikeTouchPad_parse 找到滚轮辅助键 tp->nMouse_Wheel_CurrentIndex=,%x\n", tp->nMouse_Wheel_CurrentIndex));
                continue;//查找其他功能
            }
        }

        if (tp->nMouse_LButton_LastIndex != -1) {
            if (tp->lastFinger.Contacts[tp->nMouse_LButton_LastIndex].ContactID == tp->currentFinger.Contacts[i].ContactID) {
                bMouse_LButton_Status = 1; //找到左键，
                tp->nMouse_LButton_CurrentIndex = i;//赋值左键触摸点新索引号

                KdPrint(("MouseLikeTouchPad_parse 找到左键 tp->nMouse_LButton_CurrentIndex=,%x\n", tp->nMouse_LButton_CurrentIndex));
                continue;//查找其他功能
            }
        }

        if (tp->nMouse_RButton_LastIndex != -1) {
            if (tp->lastFinger.Contacts[tp->nMouse_RButton_LastIndex].ContactID == tp->currentFinger.Contacts[i].ContactID) {
                bMouse_RButton_Status = 1; //找到右键，
                tp->nMouse_RButton_CurrentIndex = i;//赋值右键触摸点新索引号

                KdPrint(("MouseLikeTouchPad_parse 找到右键 tp->nMouse_RButton_CurrentIndex=,%x\n", tp->nMouse_RButton_CurrentIndex));
                continue;//查找其他功能
            }
        }

        if (tp->nMouse_MButton_LastIndex != -1) {
            if (tp->lastFinger.Contacts[tp->nMouse_MButton_LastIndex].ContactID == tp->currentFinger.Contacts[i].ContactID) {
                bMouse_MButton_Status = 1; //找到中键，
                tp->nMouse_MButton_CurrentIndex = i;//赋值中键触摸点新索引号

                KdPrint(("MouseLikeTouchPad_parse 找到中键 tp->nMouse_MButton_CurrentIndex=,%x\n", tp->nMouse_MButton_CurrentIndex));
                continue;//查找其他功能
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


    if (tp->currentFinger.IsButtonClicked) {//触摸板下沿物理按键功能,切换触控板灵敏度/滚轮模式开关等参数设置,需要进行离开判定，因为按键报告会一直发送直到释放
        tp->bPhysicalButtonUp = FALSE;//物理键是否释放标志
        KdPrint(("MouseLikeTouchPad_parse bPhysicalButtonUp FALSE,%x\n", FALSE));
        //准备设置触摸板下沿物理按键相关参数
        if (currentFinger_Count == 1) {//单指重按触控板左下角物理键为鼠标的后退功能键，单指重按触控板右下角物理键为鼠标的前进功能键，单指重按触控板下沿中间物理键为调节鼠标灵敏度（慢/中等/快3段灵敏度），
            if (tp->currentFinger.Contacts[0].ContactID == 0 && tp->currentFinger.Contacts[0].Confidence && tp->currentFinger.Contacts[0].TipSwitch\
                && tp->currentFinger.Contacts[0].Y > (tp->logicalMax_Y * 3 / 4) && tp->currentFinger.Contacts[0].X < tp->CornerX_LEFT) {//首个触摸点坐标在左下角
                bMouse_BButton_Status = 1;//鼠标侧面的后退键按下
            }
            else if (tp->currentFinger.Contacts[0].ContactID == 0 && tp->currentFinger.Contacts[0].Confidence && tp->currentFinger.Contacts[0].TipSwitch\
                && tp->currentFinger.Contacts[0].Y > (tp->logicalMax_Y * 3 / 4) && tp->currentFinger.Contacts[0].X > tp->CornerX_RIGHT) {//首个触摸点坐标在右下角
                bMouse_FButton_Status = 1;//鼠标侧面的前进键按下
            }
            else {//切换鼠标DPI灵敏度，放在物理键释放时执行判断

            }

        }
    }
    else {
        if (!tp->bPhysicalButtonUp) {
            tp->bPhysicalButtonUp = TRUE;
            KdPrint(("MouseLikeTouchPad_parse bPhysicalButtonUp TRUE,%x\n", TRUE));
            if (currentFinger_Count == 1) {//单指重按触控板下沿中间物理键为调节鼠标灵敏度（慢/中等/快3段灵敏度），鼠标的后退/前进功能键不需要判断会自动释放)，

                //tp->currentFinger.Contacts[0].ContactID不一定为0所以不能作为判断条件
                if (tp->currentFinger.Contacts[0].Confidence && tp->currentFinger.Contacts[0].TipSwitch\
                    && tp->currentFinger.Contacts[0].Y > (tp->logicalMax_Y * 3 / 4) && tp->currentFinger.Contacts[0].X > tp->CornerX_LEFT && tp->currentFinger.Contacts[0].X < tp->CornerX_RIGHT) {//首个触摸点坐标在触摸板下沿中间
                    //切换鼠标DPI灵敏度
                    SetNextSensitivity(pDevContext);//循环设置灵敏度
                }
            }
            else if (currentFinger_Count == 2) {//双指重按触控板下沿物理键时设置为开启/关闭双指滚轮功能
                //不采用3指滚轮方式因为判断区分双指先接触的操作必须加大时间阈值使得延迟太高不合适并且不如双指操作舒适,玩游戏较少使用到滚轮功能可选择关闭切换可以极大降低玩游戏时的误操作率，所以采取开启关闭滚轮方案兼顾日常操作和游戏

                pDevContext->bWheelDisabled = !pDevContext->bWheelDisabled;
                KdPrint(("MouseLikeTouchPad_parse bWheelDisabled=,%x\n", pDevContext->bWheelDisabled));
                if (!pDevContext->bWheelDisabled) {//开启滚轮功能时同时也恢复滚轮实现方式为触摸板双指滑动手势
                    pDevContext->bWheelScrollMode = FALSE;//默认初始值为触摸板双指滑动手势
                    KdPrint(("MouseLikeTouchPad_parse bWheelScrollMode=,%x\n", pDevContext->bWheelScrollMode));
                }

                KdPrint(("MouseLikeTouchPad_parse bPhysicalButtonUp currentFinger_Count=,%x\n", currentFinger_Count));
            }
            else if (currentFinger_Count == 3) {//三指重按触控板下沿物理键时设置为切换滚轮模式bWheelScrollMode，定义鼠标滚轮实现方式，TRUE为模仿鼠标滚轮，FALSE为触摸板双指滑动手势
                //因为日常操作滚轮更常用，所以关闭滚轮功能的状态不保存到注册表，电脑重启或休眠唤醒后恢复滚轮功能
                //因为触摸板双指滑动手势的滚轮模式更常用，所以模仿鼠标的滚轮模式状态不保存到注册表，电脑重启或休眠唤醒后恢复到双指滑动手势的滚轮模式
                pDevContext->bWheelScrollMode = !pDevContext->bWheelScrollMode;
                KdPrint(("MouseLikeTouchPad_parse bWheelScrollMode=,%x\n", pDevContext->bWheelScrollMode));

                //切换滚轮实现方式的同时也开启滚轮功能方便用户
                pDevContext->bWheelDisabled = FALSE;
                KdPrint(("MouseLikeTouchPad_parse bWheelDisabled=,%x\n", pDevContext->bWheelDisabled));


                KdPrint(("MouseLikeTouchPad_parse bPhysicalButtonUp currentFinger_Count=,%x\n", currentFinger_Count));
            }
            else if (currentFinger_Count == 4) {//四指按压触控板物理按键时为切换3段手指宽度大小设置并生效，方便用户适配鼠标中键功能。
                SetNextThumbScale(pDevContext); //动态调整手指头大小常量

                KdPrint(("MouseLikeTouchPad_parse bPhysicalButtonUp currentFinger_Count=,%x\n", currentFinger_Count));
            }
            else if (currentFinger_Count == 5) {//五指按压触控板物理按键时切换仿鼠标式触摸板与windows原版的PTP精确式触摸板操作方式
                //因为原版触控板操作方式只是临时使用所以不保存到注册表，电脑重启或休眠唤醒后恢复到仿鼠标式触摸板模式
                // 原版的PTP精确式触摸板操作方式时发送报告在本函数外部执行不需要浪费资源解析，切换回仿鼠标式触摸板模式也在本函数外部判断
                pDevContext->bMouseLikeTouchPad_Mode = FALSE;
                KdPrint(("MouseLikeTouchPad_parse bMouseLikeTouchPad_Mode=,%x\n", pDevContext->bMouseLikeTouchPad_Mode));

                KdPrint(("MouseLikeTouchPad_parse bPhysicalButtonUp currentFinger_Count=,%x\n", currentFinger_Count));
            }

        }
    }

    //开始鼠标事件逻辑判定
    //注意多手指非同时快速接触触摸板时触摸板报告可能存在一帧中同时新增多个触摸点的情况所以不能用当前只有一个触摸点作为定义指针的判断条件
    if (tp->nMouse_Pointer_LastIndex == -1 && currentFinger_Count > 0) {//鼠标指针、左键、右键、中键都未定义,
        //指针触摸点压力、接触面长宽比阈值特征区分判定手掌打字误触和正常操作,压力越小接触面长宽比阈值越大、长度阈值越小
        for (UCHAR i = 0; i < currentFinger_Count; i++) {
            //tp->currentFinger.Contacts[0].ContactID不一定为0所以不能作为判断条件
            if (tp->currentFinger.Contacts[i].Confidence && tp->currentFinger.Contacts[i].TipSwitch\
                && tp->currentFinger.Contacts[i].Y > tp->StartY_TOP && tp->currentFinger.Contacts[i].X > tp->StartX_LEFT && tp->currentFinger.Contacts[i].X < tp->StartX_RIGHT) {//起点坐标在误触横竖线以内
                tp->nMouse_Pointer_CurrentIndex = i;  //首个触摸点作为指针
                tp->MousePointer_DefineTime = tp->current_Ticktime;//定义当前指针起始时间

                KdPrint(("MouseLikeTouchPad_parse 首个触摸点作为指针,%x\n", runtimes_IOREAD));
                break;
            }
        }
    }
    else if (tp->nMouse_Pointer_CurrentIndex == -1 && tp->nMouse_Pointer_LastIndex != -1) {//指针消失
        tp->bMouse_Wheel_Mode = FALSE;//结束滚轮模式
        tp->bMouse_Wheel_Mode_JudgeEnable = TRUE;//开启滚轮判别

        tp->bGestureCompleted = TRUE;//手势模式结束,但tp->bPtpReportCollection不要重置待其他代码来处理

        tp->nMouse_Pointer_CurrentIndex = -1;
        tp->nMouse_LButton_CurrentIndex = -1;
        tp->nMouse_RButton_CurrentIndex = -1;
        tp->nMouse_MButton_CurrentIndex = -1;
        tp->nMouse_Wheel_CurrentIndex = -1;

        KdPrint(("MouseLikeTouchPad_parse 指针消失,%x\n", runtimes_IOREAD));
    }
    else if (tp->nMouse_Pointer_CurrentIndex != -1 && !tp->bMouse_Wheel_Mode) {  //指针已定义的非滚轮事件处理
        //查找指针左侧或者右侧是否有手指作为滚轮模式或者按键模式，当指针左侧/右侧的手指按下时间与指针手指定义时间间隔小于设定阈值时判定为鼠标滚轮否则为鼠标按键，这一规则能有效区别按键与滚轮操作,但鼠标按键和滚轮不能一起使用
        //按键定义后会跟踪坐标所以左键和中键不能滑动食指互相切换需要抬起食指后进行改变，左键/中键/右键按下的情况下不能转变为滚轮模式，
        LARGE_INTEGER MouseButton_Interval;
        MouseButton_Interval.QuadPart = (tp->current_Ticktime.QuadPart - tp->MousePointer_DefineTime.QuadPart) * tp->tick_Count / 10000;//单位ms毫秒
        float Mouse_Button_Interval = (float)MouseButton_Interval.LowPart;//指针左右侧的手指按下时间与指针定义起始时间的间隔ms

        if (currentFinger_Count > 1) {//触摸点数量超过1才需要判断按键操作
            for (char i = 0; i < currentFinger_Count; i++) {
                if (i == tp->nMouse_Pointer_CurrentIndex || i == tp->nMouse_LButton_CurrentIndex || i == tp->nMouse_RButton_CurrentIndex || i == tp->nMouse_MButton_CurrentIndex || i == tp->nMouse_Wheel_CurrentIndex) {//i为正值所以无需检查索引号是否为-1
                    continue;  // 已经定义的跳过
                }
                float dx = (float)(tp->currentFinger.Contacts[i].X - tp->currentFinger.Contacts[tp->nMouse_Pointer_CurrentIndex].X);
                float dy = (float)(tp->currentFinger.Contacts[i].Y - tp->currentFinger.Contacts[tp->nMouse_Pointer_CurrentIndex].Y);
                float distance = (float)sqrt(dx * dx + dy * dy);//触摸点与指针的距离

                BOOLEAN isWheel = FALSE;//滚轮模式成立条件初始化重置，注意bWheelDisabled与bMouse_Wheel_Mode_JudgeEnable的作用不同，不能混淆
                if (!pDevContext->bWheelDisabled) {//滚轮功能开启时
                    // 指针左右侧有手指按下并且与指针手指起始定义时间间隔小于阈值，指针被定义后区分滚轮操作只需判断一次直到指针消失，后续按键操作判断不会被时间阈值约束使得响应速度不受影响
                    isWheel = tp->bMouse_Wheel_Mode_JudgeEnable && fabs(distance) > tp->FingerMinDistance && fabs(distance) < tp->FingerMaxDistance && Mouse_Button_Interval < ButtonPointer_Interval_MSEC;
                }

                if (isWheel) {//滚轮模式条件成立
                    tp->bMouse_Wheel_Mode = TRUE;  //开启滚轮模式
                    tp->bMouse_Wheel_Mode_JudgeEnable = FALSE;//关闭滚轮判别

                    tp->bGestureCompleted = FALSE; //手势操作结束标志,但tp->bPtpReportCollection不要重置待其他代码来处理

                    tp->nMouse_Wheel_CurrentIndex = i;//滚轮辅助参考手指索引值
                    //手指变化瞬间时电容可能不稳定指针坐标突发性漂移需要忽略
                    tp->JitterFixStartTime = tp->current_Ticktime;//抖动修正开始计时
                    tp->Scroll_TotalDistanceX = 0;//累计滚动位移量重置
                    tp->Scroll_TotalDistanceY = 0;//累计滚动位移量重置


                    tp->nMouse_LButton_CurrentIndex = -1;
                    tp->nMouse_RButton_CurrentIndex = -1;
                    tp->nMouse_MButton_CurrentIndex = -1;

                    KdPrint(("MouseLikeTouchPad_parse 开启滚轮模式,%x\n", runtimes_IOREAD));
                    break;
                }
                else {//前面滚轮模式条件判断已经排除了所以不需要考虑与指针手指起始定义时间间隔，
                    if (tp->nMouse_MButton_CurrentIndex == -1 && fabs(distance) > tp->FingerMinDistance && fabs(distance) < tp->FingerClosedThresholdDistance && dx < 0) {//指针左侧有并拢的手指按下
                        bMouse_MButton_Status = 1; //找到中键
                        tp->nMouse_MButton_CurrentIndex = i;//赋值中键触摸点新索引号

                        KdPrint(("MouseLikeTouchPad_parse 找到中键,%x\n", runtimes_IOREAD));
                        continue;  //继续找其他按键，食指已经被中键占用所以原则上左键已经不可用
                    }
                    else if (tp->nMouse_LButton_CurrentIndex == -1 && fabs(distance) > tp->FingerClosedThresholdDistance && fabs(distance) < tp->FingerMaxDistance && dx < 0) {//指针左侧有分开的手指按下
                        bMouse_LButton_Status = 1; //找到左键
                        tp->nMouse_LButton_CurrentIndex = i;//赋值左键触摸点新索引号

                        KdPrint(("MouseLikeTouchPad_parse 找到左键,%x\n", runtimes_IOREAD));
                        continue;  //继续找其他按键
                    }
                    else if (tp->nMouse_RButton_CurrentIndex == -1 && fabs(distance) > tp->FingerMinDistance && fabs(distance) < tp->FingerMaxDistance && dx > 0) {//指针右侧有手指按下
                        bMouse_RButton_Status = 1; //找到右键
                        tp->nMouse_RButton_CurrentIndex = i;//赋值右键触摸点新索引号

                        KdPrint(("MouseLikeTouchPad_parse 找到右键,%x\n", runtimes_IOREAD));
                        continue;  //继续找其他按键
                    }
                }

            }
        }

        //鼠标指针位移设置
        if (currentFinger_Count != lastFinger_Count) {//手指变化瞬间时电容可能不稳定指针坐标突发性漂移需要忽略
            tp->JitterFixStartTime = tp->current_Ticktime;//抖动修正开始计时
            KdPrint(("MouseLikeTouchPad_parse 抖动修正开始计时,%x\n", runtimes_IOREAD));
        }
        else {
            LARGE_INTEGER FixTimer;
            FixTimer.QuadPart = (tp->current_Ticktime.QuadPart - tp->JitterFixStartTime.QuadPart) * tp->tick_Count / 10000;//单位ms毫秒
            float JitterFixTimer = (float)FixTimer.LowPart;//当前抖动时间计时

            float STABLE_INTERVAL;
            if (tp->nMouse_MButton_CurrentIndex != -1) {//中键状态下手指并拢的抖动修正值区别处理
                STABLE_INTERVAL = STABLE_INTERVAL_FingerClosed_MSEC;
            }
            else {
                STABLE_INTERVAL = STABLE_INTERVAL_FingerSeparated_MSEC;
            }

            SHORT diffX = tp->currentFinger.Contacts[tp->nMouse_Pointer_CurrentIndex].X - tp->lastFinger.Contacts[tp->nMouse_Pointer_LastIndex].X;
            SHORT diffY = tp->currentFinger.Contacts[tp->nMouse_Pointer_CurrentIndex].Y - tp->lastFinger.Contacts[tp->nMouse_Pointer_LastIndex].Y;

            float px = (float)(diffX / tp->thumb_Scale);
            float py = (float)(diffY / tp->thumb_Scale);

            if (JitterFixTimer < STABLE_INTERVAL) {//触摸点稳定前修正
                if (tp->nMouse_LButton_CurrentIndex != -1 || tp->nMouse_RButton_CurrentIndex != -1 || tp->nMouse_MButton_CurrentIndex != -1) {//有按键时修正，单指针时不需要使得指针更精确
                    if (fabs(px) <= Jitter_Offset) {//指针轻微抖动修正
                        px = 0;
                    }
                    if (fabs(py) <= Jitter_Offset) {//指针轻微抖动修正
                        py = 0;
                    }
                }
            }

            double xx = pDevContext->MouseSensitivity_Value * px / tp->PointerSensitivity_x;
            double yy = pDevContext->MouseSensitivity_Value * py / tp->PointerSensitivity_y;
            mReport.dx = (CHAR)xx;
            mReport.dy = (CHAR)yy;

            if (fabs(xx) > 0.5 && fabs(xx) < 1) {//慢速精细移动指针修正
                if (xx > 0) {
                    mReport.dx = 1;
                }
                else {
                    mReport.dx = -1;
                }

            }
            if (fabs(yy) > 0.5 && fabs(yy) < 1) {//慢速精细移动指针修正
                if (xx > 0) {
                    mReport.dy = 1;
                }
                else {
                    mReport.dy = -1;
                }
            }

        }
    }
    else if (tp->nMouse_Pointer_CurrentIndex != -1 && tp->bMouse_Wheel_Mode) {//滚轮操作模式，触摸板双指滑动、三指四指手势也归为此模式下的特例设置一个手势状态开关供后续判断使用
        if (!pDevContext->bWheelScrollMode || currentFinger_Count > 2) {//触摸板双指滑动手势模式，三指四指手势也归为此模式
            tp->bPtpReportCollection = TRUE;//发送PTP触摸板集合报告，后续再做进一步判断
            KdPrint(("MouseLikeTouchPad_parse 发送PTP触摸板集合报告，后续再做进一步判断,%x\n", runtimes_IOREAD));
        }
        else {
            //鼠标指针位移设置
            LARGE_INTEGER FixTimer;
            FixTimer.QuadPart = (tp->current_Ticktime.QuadPart - tp->JitterFixStartTime.QuadPart) * tp->tick_Count / 10000;//单位ms毫秒
            float JitterFixTimer = (float)FixTimer.LowPart;//当前抖动时间计时

            float px = (float)(tp->currentFinger.Contacts[tp->nMouse_Pointer_CurrentIndex].X - tp->lastFinger.Contacts[tp->nMouse_Pointer_LastIndex].X) / tp->thumb_Scale;
            float py = (float)(tp->currentFinger.Contacts[tp->nMouse_Pointer_CurrentIndex].Y - tp->lastFinger.Contacts[tp->nMouse_Pointer_LastIndex].Y) / tp->thumb_Scale;

            if (JitterFixTimer < STABLE_INTERVAL_FingerClosed_MSEC) {//只需在触摸点稳定前修正
                if (fabs(px) <= Jitter_Offset) {//指针轻微抖动修正
                    px = 0;
                }
                if (fabs(py) <= Jitter_Offset) {//指针轻微抖动修正
                    py = 0;
                }
            }

            int direction_hscale = 1;//滚动方向缩放比例
            int direction_vscale = 1;//滚动方向缩放比例

            if (fabs(px) > fabs(py) / 4) {//滚动方向稳定性修正
                direction_hscale = 1;
                direction_vscale = 8;
            }
            if (fabs(py) > fabs(px) / 4) {//滚动方向稳定性修正
                direction_hscale = 8;
                direction_vscale = 1;
            }

            px = px / direction_hscale;
            py = py / direction_vscale;

            px = (float)(pDevContext->MouseSensitivity_Value * px / tp->PointerSensitivity_x);
            py = (float)(pDevContext->MouseSensitivity_Value * py / tp->PointerSensitivity_y);

            tp->Scroll_TotalDistanceX += px;//累计滚动位移量
            tp->Scroll_TotalDistanceY += py;//累计滚动位移量

            //判断滚动量
            if (fabs(tp->Scroll_TotalDistanceX) > SCROLL_OFFSET_THRESHOLD_X) {//位移量超过阈值
                int h = (int)(fabs(tp->Scroll_TotalDistanceX) / SCROLL_OFFSET_THRESHOLD_X);
                mReport.h_wheel = (char)(tp->Scroll_TotalDistanceX > 0 ? h : -h);//滚动行数

                float r = (float)(fabs(tp->Scroll_TotalDistanceX) - SCROLL_OFFSET_THRESHOLD_X * h);// 滚动位移量余数绝对值
                tp->Scroll_TotalDistanceX = tp->Scroll_TotalDistanceX > 0 ? r : -r;//滚动位移量余数
            }
            if (fabs(tp->Scroll_TotalDistanceY) > SCROLL_OFFSET_THRESHOLD_Y) {//位移量超过阈值
                int v = (int)(fabs(tp->Scroll_TotalDistanceY) / SCROLL_OFFSET_THRESHOLD_Y);
                mReport.v_wheel = (char)(tp->Scroll_TotalDistanceY > 0 ? v : -v);//滚动行数

                float r = (float)(fabs(tp->Scroll_TotalDistanceY) - SCROLL_OFFSET_THRESHOLD_Y * v);// 滚动位移量余数绝对值
                tp->Scroll_TotalDistanceY = tp->Scroll_TotalDistanceY > 0 ? r : -r;//滚动位移量余数
            }
        }

    }
    else {
        //其他组合无效
        KdPrint(("MouseLikeTouchPad_parse 其他组合无效,%x\n", runtimes_IOREAD));
    }


    if (tp->bPtpReportCollection) {//触摸板集合，手势模式判断
        if (!tp->bMouse_Wheel_Mode) {//以指针手指释放为滚轮模式结束标志，下一帧bPtpReportCollection会设置FALSE所以只会发送一次构造的手势结束报告
            tp->bPtpReportCollection = FALSE;//PTP触摸板集合报告模式结束
            tp->bGestureCompleted = TRUE;//结束手势操作，该数据和bMouse_Wheel_Mode区分开了，因为bGestureCompleted可能会比bMouse_Wheel_Mode提前结束
            KdPrint(("MouseLikeTouchPad_parse bPtpReportCollection bGestureCompleted0,%x\n", status));

            //构造全部手指释放的临时数据包,TipSwitch域归零，windows手势操作结束时需要手指离开的点xy坐标数据
            PTP_REPORT CompletedGestureReport;
            RtlCopyMemory(&CompletedGestureReport, &tp->currentFinger, sizeof(PTP_REPORT));
            for (int i = 0; i < currentFinger_Count; i++) {
                CompletedGestureReport.Contacts[i].TipSwitch = 0;
            }

            //发送ptp报告
            status = SendPtpMultiTouchReport(pDevContext, &CompletedGestureReport, sizeof(PTP_REPORT));
            if (!NT_SUCCESS(status)) {
                KdPrint(("MouseLikeTouchPad_parse SendPtpMultiTouchReport CompletedGestureReport failed,%x\n", status));
            }

        }
        else if (tp->bMouse_Wheel_Mode && currentFinger_Count == 1 && !tp->bGestureCompleted) {//滚轮模式未结束并且剩下指针手指留在触摸板上,需要配合bGestureCompleted标志判断使得构造的手势结束报告只发送一次
            tp->bPtpReportCollection = FALSE;//PTP触摸板集合报告模式结束
            tp->bGestureCompleted = TRUE;//提前结束手势操作，该数据和bMouse_Wheel_Mode区分开了，因为bGestureCompleted可能会比bMouse_Wheel_Mode提前结束
            KdPrint(("MouseLikeTouchPad_parse bPtpReportCollection bGestureCompleted1,%x\n", status));

            //构造指针手指释放的临时数据包,TipSwitch域归零，windows手势操作结束时需要手指离开的点xy坐标数据
            PTP_REPORT CompletedGestureReport2;
            RtlCopyMemory(&CompletedGestureReport2, &tp->currentFinger, sizeof(PTP_REPORT));
            CompletedGestureReport2.Contacts[0].TipSwitch = 0;

            //发送ptp报告
            status = SendPtpMultiTouchReport(pDevContext, &CompletedGestureReport2, sizeof(PTP_REPORT));
            if (!NT_SUCCESS(status)) {
                KdPrint(("MouseLikeTouchPad_parse SendPtpMultiTouchReport CompletedGestureReport2 failed,%x\n", status));
            }
        }

        if (!tp->bGestureCompleted) {//手势未结束，正常发送报告
            KdPrint(("MouseLikeTouchPad_parse bPtpReportCollection bGestureCompleted2,%x\n", status));
            //发送ptp报告
            status = SendPtpMultiTouchReport(pDevContext, pPtpReport, sizeof(PTP_REPORT));
            if (!NT_SUCCESS(status)) {
                KdPrint(("MouseLikeTouchPad_parse SendPtpMultiTouchReport failed,%x\n", status));
            }
        }
    }
    else {//发送MouseCollection
        mReport.button = bMouse_LButton_Status + (bMouse_RButton_Status << 1) + (bMouse_MButton_Status << 2) + (bMouse_BButton_Status << 3) + (bMouse_FButton_Status << 4);  //左中右后退前进键状态合成
        KdPrint(("MouseLikeTouchPad_parse SendPtpMouseReport mReport.report_id=,%x\n", mReport.report_id));
        KdPrint(("MouseLikeTouchPad_parse SendPtpMouseReport mReport.button=,%x\n", mReport.button));
        KdPrint(("MouseLikeTouchPad_parse SendPtpMouseReport mReport.dx=,%x\n", mReport.dx));
        KdPrint(("MouseLikeTouchPad_parse SendPtpMouseReport mReport.dy=,%x\n", mReport.dy));
        KdPrint(("MouseLikeTouchPad_parse SendPtpMouseReport mReport.v_wheel=,%x\n", mReport.v_wheel));
        KdPrint(("MouseLikeTouchPad_parse SendPtpMouseReport mReport.h_wheel=,%x\n", mReport.h_wheel));


        //发送鼠标报告
        status = SendPtpMouseReport(pDevContext, &mReport);
        if (!NT_SUCCESS(status)) {
            KdPrint(("MouseLikeTouchPad_parse SendPtpMouseReport failed,%x\n", status));
        }
    }


    //保存下一轮所有触摸点的初始坐标及功能定义索引号
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

    tp->nMouse_Pointer_CurrentIndex = -1; //定义当前鼠标指针触摸点坐标的数据索引号，-1为未定义
    tp->nMouse_LButton_CurrentIndex = -1; //定义当前鼠标左键触摸点坐标的数据索引号，-1为未定义
    tp->nMouse_RButton_CurrentIndex = -1; //定义当前鼠标右键触摸点坐标的数据索引号，-1为未定义
    tp->nMouse_MButton_CurrentIndex = -1; //定义当前鼠标中键触摸点坐标的数据索引号，-1为未定义
    tp->nMouse_Wheel_CurrentIndex = -1; //定义当前鼠标滚轮辅助参考手指触摸点坐标的数据索引号，-1为未定义

    tp->nMouse_Pointer_LastIndex = -1; //定义上次鼠标指针触摸点坐标的数据索引号，-1为未定义
    tp->nMouse_LButton_LastIndex = -1; //定义上次鼠标左键触摸点坐标的数据索引号，-1为未定义
    tp->nMouse_RButton_LastIndex = -1; //定义上次鼠标右键触摸点坐标的数据索引号，-1为未定义
    tp->nMouse_MButton_LastIndex = -1; //定义上次鼠标中键触摸点坐标的数据索引号，-1为未定义
    tp->nMouse_Wheel_LastIndex = -1; //定义上次鼠标滚轮辅助参考手指触摸点坐标的数据索引号，-1为未定义

    pDevContext->bWheelDisabled = FALSE;//默认初始值为开启滚轮操作功能
    pDevContext->bWheelScrollMode = FALSE;//默认初始值为触摸板双指滑动手势


    tp->bMouse_Wheel_Mode = FALSE;
    tp->bMouse_Wheel_Mode_JudgeEnable = TRUE;//开启滚轮判别

    tp->bGestureCompleted = FALSE; //手势操作结束标志
    tp->bPtpReportCollection = FALSE;//默认鼠标集合

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
    if (ts_idx == 3) {//灵敏度循环设置
        ts_idx = 0;
    }

    pDevContext->ThumbScale_Index = ts_idx;
    pDevContext->ThumbScale_Value = ThumbScaleTable[ts_idx];

    PTP_PARSER* tp = &pDevContext->tp_settings;
    //动态调整手指头大小常量
    tp->thumb_Scale = (float)(pDevContext->ThumbScale_Value);//手指头尺寸缩放比例，
    tp->FingerMinDistance = 12 * tp->TouchPad_DPMM_x * tp->thumb_Scale;//定义有效的相邻手指最小距离
    tp->FingerClosedThresholdDistance = 16 * tp->TouchPad_DPMM_x * tp->thumb_Scale;//定义相邻手指合拢时的最小距离
    tp->FingerMaxDistance = tp->FingerMinDistance * 4;//定义有效的相邻手指最大距离(FingerMinDistance*4) 

    KdPrint(("SetNextThumbScale pDevContext->ThumbScale_Index,%x\n", pDevContext->ThumbScale_Index));

}


NTSTATUS SetRegisterThumbScale(PDEVICE_CONTEXT pDevContext, ULONG ts_idx)//保存设置到注册表
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


NTSTATUS GetRegisterThumbScale(PDEVICE_CONTEXT pDevContext, ULONG* ts_idx)//从注册表读取设置
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
    if (ms_idx == 3) {//灵敏度循环设置
        ms_idx = 0;
    }

    pDevContext->MouseSensitivity_Index = ms_idx;
    pDevContext->MouseSensitivity_Value = MouseSensitivityTable[ms_idx];
    KdPrint(("SetNextSensitivity pDevContext->MouseSensitivity_Index,%x\n", pDevContext->MouseSensitivity_Index));

}


NTSTATUS SetRegisterDeviceType(PDEVICE_CONTEXT pDevContext, ULONG dt_idx)//保存设置到注册表
{
    ////TouchpadDeviceType（触摸板类型，关联否开启防误触功能）
    //0 - TP笔记本电脑内置触摸板Built in（有防误触功能）
    //    1 - 外置独立触摸板External TouchPad（无防误触功能）
    //    2 - 外置触摸板键盘External TouchPad Keyboard（有防误触功能）

    NTSTATUS status = STATUS_SUCCESS;

    status = SetRegConfig(pDevContext, L"DeviceType_Index", dt_idx);
    if (!NT_SUCCESS(status)) {
        KdPrint(("SetRegisterDeviceType err,%x\n", status));
        return status;
    }

    KdPrint(("SetRegisterDeviceType ok,%x\n", status));
    return status;
}


NTSTATUS GetRegisterDeviceType(PDEVICE_CONTEXT pDevContext, ULONG* dt_idx)//从注册表读取设置
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


NTSTATUS SetRegisterSpaceLayout(PDEVICE_CONTEXT pDevContext, ULONG sl_idx)//保存设置到注册表
{
    //TouchpadSpaceLayout(触摸板相对空格键对齐位置布局设计，关联防误触的区域计算）
    //    0 - 居中布局CenterAlign
    //    1 - 偏右5mm设计RightAlign5
    //    2 - 偏右10mm设计RightAlign10

    NTSTATUS status = STATUS_SUCCESS;

    status = SetRegConfig(pDevContext, L"SpaceLayout_Index", sl_idx);
    if (!NT_SUCCESS(status)) {
        KdPrint(("SetRegisterSpaceLayout err,%x\n", status));
        return status;
    }

    KdPrint(("SetRegisterSpaceLayout ok,%x\n", status));
    return status;
}


NTSTATUS GetRegisterSpaceLayout(PDEVICE_CONTEXT pDevContext, ULONG* sl_idx)//从注册表读取设置
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
