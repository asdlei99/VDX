/*
MIT License

Copyright (c) 2016 Benjamin "Nefarius" H�glinger

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/


#include "busenum.h"
#include <wdmsec.h>
#include <usbioctl.h>

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Bus_CreatePdo)
#pragma alloc_text(PAGE, Bus_EvtDeviceListCreatePdo)
#pragma alloc_text(PAGE, Bus_EvtDevicePrepareHardware)
#endif

NTSTATUS Bus_EvtDeviceListCreatePdo(
    WDFCHILDLIST DeviceList,
    PWDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER IdentificationDescription,
    PWDFDEVICE_INIT ChildInit)
{
    PPDO_IDENTIFICATION_DESCRIPTION pDesc;

    PAGED_CODE();

    pDesc = CONTAINING_RECORD(IdentificationDescription, PDO_IDENTIFICATION_DESCRIPTION, Header);

    return Bus_CreatePdo(WdfChildListGetDevice(DeviceList), ChildInit, pDesc);
}

//
// Compares two children on the bus based on their serial numbers.
// 
BOOLEAN Bus_EvtChildListIdentificationDescriptionCompare(
    WDFCHILDLIST DeviceList,
    PWDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER FirstIdentificationDescription,
    PWDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER SecondIdentificationDescription)
{
    PPDO_IDENTIFICATION_DESCRIPTION lhs, rhs;

    UNREFERENCED_PARAMETER(DeviceList);

    lhs = CONTAINING_RECORD(FirstIdentificationDescription,
        PDO_IDENTIFICATION_DESCRIPTION,
        Header);
    rhs = CONTAINING_RECORD(SecondIdentificationDescription,
        PDO_IDENTIFICATION_DESCRIPTION,
        Header);

    return (lhs->SerialNo == rhs->SerialNo) ? TRUE : FALSE;
}

//
// Creates and initializes a PDO (child).
// 
NTSTATUS Bus_CreatePdo(
    _In_ WDFDEVICE Device,
    _In_ PWDFDEVICE_INIT DeviceInit,
    _In_ PPDO_IDENTIFICATION_DESCRIPTION Description)
{
    NTSTATUS                        status;
    PPDO_DEVICE_DATA                pdoData;
    WDFDEVICE                       hChild = NULL;
    WDF_DEVICE_PNP_CAPABILITIES     pnpCaps;
    WDF_DEVICE_POWER_CAPABILITIES   powerCaps;
    WDF_PNPPOWER_EVENT_CALLBACKS    pnpPowerCallbacks;
    WDF_OBJECT_ATTRIBUTES           pdoAttributes;
    WDF_IO_QUEUE_CONFIG             defaultPdoQueueConfig;
    WDFQUEUE                        defaultPdoQueue;
    UNICODE_STRING                  deviceDescription;

    DECLARE_CONST_UNICODE_STRING(deviceLocation, L"Virtual Gamepad Emulation Bus");
    DECLARE_UNICODE_STRING_SIZE(buffer, MAX_INSTANCE_ID_LEN);
    // reserve space for device id
    DECLARE_UNICODE_STRING_SIZE(deviceId, MAX_INSTANCE_ID_LEN);


    PAGED_CODE();

    UNREFERENCED_PARAMETER(Device);

    KdPrint((DRIVERNAME "Entered Bus_CreatePdo\n"));

    // set device type
    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_BUS_EXTENDER);
    // Bus is power policy owner
    WdfDeviceInitSetPowerPolicyOwnership(DeviceInit, FALSE);

    //
    // Catch IRP_MJ_PNP / IRP_MN_QUERY_DEVICE_RELATIONS requests
    // 
    // When emulating an Xbox 360 device, the XUSB.SYS driver tries to
    // query for an attached headset and bring it online. This has to
    // be suppressed when the POD receives IRP_MN_QUERY_DEVICE_RELATIONS.
    //  
    UCHAR mnCodes[] = { IRP_MN_QUERY_DEVICE_RELATIONS };
    WdfDeviceInitAssignWdmIrpPreprocessCallback(DeviceInit, Pdo_EvtDeviceWdmIrpPreprocess, IRP_MJ_PNP, mnCodes, 1);

#pragma region Enter RAW device mode

    status = WdfPdoInitAssignRawDevice(DeviceInit, &GUID_DEVCLASS_VIGEM_RAWPDO);
    if (!NT_SUCCESS(status))
    {
        KdPrint((DRIVERNAME "WdfPdoInitAssignRawDevice failed status 0x%x\n", status));
        return status;
    }

    WdfDeviceInitSetCharacteristics(DeviceInit, FILE_AUTOGENERATED_DEVICE_NAME, TRUE);

    status = WdfDeviceInitAssignSDDLString(DeviceInit, &SDDL_DEVOBJ_SYS_ALL_ADM_RWX_WORLD_RWX_RES_RWX);
    if (!NT_SUCCESS(status))
    {
        KdPrint((DRIVERNAME "WdfDeviceInitAssignSDDLString failed status 0x%x\n", status));
        return status;
    }

#pragma endregion

#pragma region Prepare PDO

    // set parameters matching desired target device
    switch (Description->TargetType)
    {
        //
        // A Xbox 360 device was requested
        // 
    case Xbox360Wired:

        status = Xusb_PreparePdo(
            DeviceInit,
            Description->VendorId,
            Description->ProductId,
            &deviceId,
            &deviceDescription);

        if (!NT_SUCCESS(status))
            return status;

        break;

        //
        // A Sony DualShock 4 device was requested
        // 
    case DualShock4Wired:

        status = Ds4_PreparePdo(DeviceInit, &deviceId, &deviceDescription);

        if (!NT_SUCCESS(status))
            return status;

        break;

        //
        // A Xbox One device was requested
        // 
    case XboxOneWired:

        status = Xgip_PreparePdo(DeviceInit, &deviceId, &deviceDescription);

        if (!NT_SUCCESS(status))
            return status;

        break;

    default:

        KdPrint((DRIVERNAME "Unsupported target type\n"));
        status = STATUS_INVALID_PARAMETER;
        return status;
    }

    // set device id
    status = WdfPdoInitAssignDeviceID(DeviceInit, &deviceId);
    if (!NT_SUCCESS(status))
        return status;

    // prepare instance id
    status = RtlUnicodeStringPrintf(&buffer, L"%02d", Description->SerialNo);
    if (!NT_SUCCESS(status))
        return status;

    // set instance id
    status = WdfPdoInitAssignInstanceID(DeviceInit, &buffer);
    if (!NT_SUCCESS(status))
        return status;

    // set device description (for English operating systems)
    status = WdfPdoInitAddDeviceText(DeviceInit, &deviceDescription, &deviceLocation, 0x409);
    if (!NT_SUCCESS(status))
        return status;

    // default locale is English
    // TODO: add more locales
    WdfPdoInitSetDefaultLocale(DeviceInit, 0x409);

#pragma endregion

#pragma region PNP/Power event callbacks

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpPowerCallbacks);

    pnpPowerCallbacks.EvtDevicePrepareHardware = Bus_EvtDevicePrepareHardware;

    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpPowerCallbacks);

#pragma endregion

    // NOTE: not utilized at the moment
    WdfPdoInitAllowForwardingRequestToParent(DeviceInit);

#pragma region Create PDO

    // Add common device data context
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&pdoAttributes, PDO_DEVICE_DATA);

    status = WdfDeviceCreate(&DeviceInit, &pdoAttributes, &hChild);
    if (!NT_SUCCESS(status))
        return status;

    KdPrint((DRIVERNAME "Created PDO: 0x%X\n", hChild));

    switch (Description->TargetType)
    {
        // Add XUSB-specific device data context
    case Xbox360Wired:
    {
        PXUSB_DEVICE_DATA xusbData = NULL;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&pdoAttributes, XUSB_DEVICE_DATA);

        status = WdfObjectAllocateContext(hChild, &pdoAttributes, (PVOID)&xusbData);
        if (!NT_SUCCESS(status))
        {
            KdPrint((DRIVERNAME "WdfObjectAllocateContext failed status 0x%x\n", status));
            return status;
        }

        break;
    }
    case DualShock4Wired:
    {
        PDS4_DEVICE_DATA ds4Data = NULL;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&pdoAttributes, DS4_DEVICE_DATA);

        status = WdfObjectAllocateContext(hChild, &pdoAttributes, (PVOID)&ds4Data);
        if (!NT_SUCCESS(status))
        {
            KdPrint((DRIVERNAME "WdfObjectAllocateContext failed status 0x%x\n", status));
            return status;
        }

        break;
    }
    case XboxOneWired:
    {
        PXGIP_DEVICE_DATA xgipData = NULL;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&pdoAttributes, XGIP_DEVICE_DATA);

        status = WdfObjectAllocateContext(hChild, &pdoAttributes, (PVOID)&xgipData);
        if (!NT_SUCCESS(status))
        {
            KdPrint((DRIVERNAME "WdfObjectAllocateContext failed status 0x%x\n", status));
            return status;
        }

        break;
    }
    default:
        break;
    }

#pragma endregion

#pragma region Expose USB Interface

    status = WdfDeviceCreateDeviceInterface(Device, (LPGUID)&GUID_DEVINTERFACE_USB_DEVICE, NULL);
    if (!NT_SUCCESS(status))
    {
        KdPrint((DRIVERNAME "WdfDeviceCreateDeviceInterface failed status 0x%x\n", status));
        return status;
    }

#pragma endregion

#pragma region Set PDO contexts

    pdoData = PdoGetData(hChild);

    pdoData->SerialNo = Description->SerialNo;
    pdoData->TargetType = Description->TargetType;
    pdoData->OwnerProcessId = Description->OwnerProcessId;
    pdoData->VendorId = Description->VendorId;
    pdoData->ProductId = Description->ProductId;

    // Initialize additional contexts (if available)
    switch (Description->TargetType)
    {
    case Xbox360Wired:

        status = Xusb_AssignPdoContext(hChild, Description);

        break;

    case DualShock4Wired:

        status = Ds4_AssignPdoContext(hChild, Description);

        break;

    case XboxOneWired:

        status = Xgip_AssignPdoContext(hChild);

        break;

    default:
        break;
    }

    if (!NT_SUCCESS(status))
    {
        KdPrint((DRIVERNAME "Couldn't initialize additional contexts\n"));
        return status;
    }

#pragma endregion

#pragma region Default I/O queue setup

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&defaultPdoQueueConfig, WdfIoQueueDispatchParallel);

    defaultPdoQueueConfig.EvtIoInternalDeviceControl = Pdo_EvtIoInternalDeviceControl;

    status = WdfIoQueueCreate(hChild, &defaultPdoQueueConfig, WDF_NO_OBJECT_ATTRIBUTES, &defaultPdoQueue);
    if (!NT_SUCCESS(status))
    {
        KdPrint((DRIVERNAME "WdfIoQueueCreate failed 0x%x\n", status));
        return status;
    }

#pragma endregion

#pragma region PNP capabilities

    WDF_DEVICE_PNP_CAPABILITIES_INIT(&pnpCaps);

    pnpCaps.Removable = WdfTrue;
    pnpCaps.EjectSupported = WdfTrue;
    pnpCaps.SurpriseRemovalOK = WdfTrue;

    pnpCaps.Address = Description->SerialNo;
    pnpCaps.UINumber = Description->SerialNo;

    WdfDeviceSetPnpCapabilities(hChild, &pnpCaps);

#pragma endregion

#pragma region Power capabilities

    WDF_DEVICE_POWER_CAPABILITIES_INIT(&powerCaps);

    powerCaps.DeviceD1 = WdfTrue;
    powerCaps.WakeFromD1 = WdfTrue;
    powerCaps.DeviceWake = PowerDeviceD1;

    powerCaps.DeviceState[PowerSystemWorking] = PowerDeviceD0;
    powerCaps.DeviceState[PowerSystemSleeping1] = PowerDeviceD1;
    powerCaps.DeviceState[PowerSystemSleeping2] = PowerDeviceD3;
    powerCaps.DeviceState[PowerSystemSleeping3] = PowerDeviceD3;
    powerCaps.DeviceState[PowerSystemHibernate] = PowerDeviceD3;
    powerCaps.DeviceState[PowerSystemShutdown] = PowerDeviceD3;

    WdfDeviceSetPowerCapabilities(hChild, &powerCaps);

#pragma endregion

    return status;
}

NTSTATUS Pdo_EvtDeviceWdmIrpPreprocess(
    _In_    WDFDEVICE Device,
    _Inout_ PIRP      Irp
)
{
    PPDO_DEVICE_DATA    pdoData;

    KdPrint((DRIVERNAME "Pdo_EvtDeviceWdmIrpPreprocess called\n"));

    pdoData = PdoGetData(Device);

    switch (pdoData->TargetType)
    {
    case Xbox360Wired:
        Irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        break;
    default:
        break;
    }

    return WdfDeviceWdmDispatchPreprocessedIrp(Device, Irp);
}

//
// Exposes necessary interfaces on PDO power-up.
// 
NTSTATUS Bus_EvtDevicePrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
)
{
    PPDO_DEVICE_DATA    pdoData;
    NTSTATUS            status = STATUS_UNSUCCESSFUL;

    PAGED_CODE();

    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    KdPrint((DRIVERNAME "Bus_EvtDevicePrepareHardware: 0x%p\n", Device));

    pdoData = PdoGetData(Device);

    switch (pdoData->TargetType)
    {
        // Expose XUSB interfaces
    case Xbox360Wired:

        status = Xusb_PrepareHardware(Device);

        if (!NT_SUCCESS(status))
            return status;

        break;

    case DualShock4Wired:

        status = Ds4_PrepareHardware(Device);

        if (!NT_SUCCESS(status))
            return status;

        break;

    case XboxOneWired:

        status = Xgip_PrepareHardware(Device);

        if (!NT_SUCCESS(status))
            return status;

        break;

    default:
        break;
    }

    return status;
}

//
// Responds to IRP_MJ_INTERNAL_DEVICE_CONTROL requests sent to PDO.
// 
VOID Pdo_EvtIoInternalDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
)
{
    // Regular buffers not used in USB communication
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    NTSTATUS                status = STATUS_INVALID_PARAMETER;
    WDFDEVICE               hDevice;
    PIRP                    irp;
    PURB                    urb;
    PPDO_DEVICE_DATA        pdoData;
    PIO_STACK_LOCATION      irpStack;

    hDevice = WdfIoQueueGetDevice(Queue);
    pdoData = PdoGetData(hDevice);
    // No help from the framework available from here on
    irp = WdfRequestWdmGetIrp(Request);
    irpStack = IoGetCurrentIrpStackLocation(irp);

    switch (IoControlCode)
    {
    case IOCTL_INTERNAL_USB_SUBMIT_URB:

        KdPrint((DRIVERNAME ">> IOCTL_INTERNAL_USB_SUBMIT_URB\n"));

        urb = (PURB)URB_FROM_IRP(irp);

        switch (urb->UrbHeader.Function)
        {
        case URB_FUNCTION_CONTROL_TRANSFER:

            KdPrint((DRIVERNAME ">> >> URB_FUNCTION_CONTROL_TRANSFER\n"));

            // Control transfer can safely be ignored
            status = STATUS_SUCCESS;

            break;

        case URB_FUNCTION_CONTROL_TRANSFER_EX:

            KdPrint((DRIVERNAME ">> >> URB_FUNCTION_CONTROL_TRANSFER_EX\n"));

            status = STATUS_UNSUCCESSFUL;

            break;

        case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:

            KdPrint((DRIVERNAME ">> >> URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER\n"));

            status = UsbPdo_BulkOrInterruptTransfer(urb, hDevice, Request);

            break;

        case URB_FUNCTION_SELECT_CONFIGURATION:

            KdPrint((DRIVERNAME ">> >> URB_FUNCTION_SELECT_CONFIGURATION\n"));

            status = UsbPdo_SelectConfiguration(urb, pdoData);

            break;

        case URB_FUNCTION_SELECT_INTERFACE:

            KdPrint((DRIVERNAME ">> >> URB_FUNCTION_SELECT_INTERFACE\n"));

            status = UsbPdo_SelectInterface(urb, pdoData);

            break;

        case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:

            KdPrint((DRIVERNAME ">> >> URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE\n"));

            switch (urb->UrbControlDescriptorRequest.DescriptorType)
            {
            case USB_DEVICE_DESCRIPTOR_TYPE:

                KdPrint((DRIVERNAME ">> >> >> USB_DEVICE_DESCRIPTOR_TYPE\n"));

                status = UsbPdo_GetDeviceDescriptorType(urb, pdoData);

                break;

            case USB_CONFIGURATION_DESCRIPTOR_TYPE:

                KdPrint((DRIVERNAME ">> >> >> USB_CONFIGURATION_DESCRIPTOR_TYPE\n"));

                status = UsbPdo_GetConfigurationDescriptorType(urb, pdoData);

                break;

            case USB_STRING_DESCRIPTOR_TYPE:

                KdPrint((DRIVERNAME ">> >> >> USB_STRING_DESCRIPTOR_TYPE\n"));

                status = UsbPdo_GetStringDescriptorType(urb, pdoData);

                break;
            case USB_INTERFACE_DESCRIPTOR_TYPE:

                KdPrint((DRIVERNAME ">> >> >> USB_INTERFACE_DESCRIPTOR_TYPE\n"));

                break;

            case USB_ENDPOINT_DESCRIPTOR_TYPE:

                KdPrint((DRIVERNAME ">> >> >> USB_ENDPOINT_DESCRIPTOR_TYPE\n"));

                break;

            default:
                KdPrint((DRIVERNAME ">> >> >> Unknown descriptor type\n"));
                break;
            }

            KdPrint((DRIVERNAME "<< <<\n"));

            break;

        case URB_FUNCTION_GET_STATUS_FROM_DEVICE:

            KdPrint((DRIVERNAME ">> >> URB_FUNCTION_GET_STATUS_FROM_DEVICE\n"));

            // Defaults always succeed
            status = STATUS_SUCCESS;

            break;

        case URB_FUNCTION_ABORT_PIPE:

            KdPrint((DRIVERNAME ">> >> URB_FUNCTION_ABORT_PIPE\n"));

            status = UsbPdo_AbortPipe(hDevice);

            break;

        case URB_FUNCTION_CLASS_INTERFACE:

            KdPrint((DRIVERNAME ">> >> URB_FUNCTION_CLASS_INTERFACE\n"));

            status = UsbPdo_ClassInterface(urb, hDevice, pdoData);

            break;

        case URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE:

            KdPrint((DRIVERNAME ">> >> URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE\n"));

            status = UsbPdo_GetDescriptorFromInterface(urb, pdoData);

            break;

        default:
            KdPrint((DRIVERNAME ">> >> Unknown function: 0x%X\n", urb->UrbHeader.Function));
            break;
        }

        KdPrint((DRIVERNAME "<<\n"));

        break;

    case IOCTL_INTERNAL_USB_GET_PORT_STATUS:

        KdPrint((DRIVERNAME ">> IOCTL_INTERNAL_USB_GET_PORT_STATUS\n"));

        // We report the (virtual) port as always active
        *(unsigned long *)irpStack->Parameters.Others.Argument1 = USBD_PORT_ENABLED | USBD_PORT_CONNECTED;

        status = STATUS_SUCCESS;

        break;

    case IOCTL_INTERNAL_USB_RESET_PORT:

        KdPrint((DRIVERNAME ">> IOCTL_INTERNAL_USB_RESET_PORT\n"));

        // Sure, why not ;)
        status = STATUS_SUCCESS;

        break;

    case IOCTL_INTERNAL_USB_SUBMIT_IDLE_NOTIFICATION:

        KdPrint((DRIVERNAME ">> IOCTL_INTERNAL_USB_SUBMIT_IDLE_NOTIFICATION\n"));

        // TODO: implement
        // This happens if the I/O latency is too high so HIDUSB aborts communication.
        status = STATUS_SUCCESS;

        break;

    default:
        KdPrint((DRIVERNAME ">> Unknown I/O control code 0x%X\n", IoControlCode));
        break;
    }

    if (status != STATUS_PENDING)
    {
        WdfRequestComplete(Request, status);
    }
}

