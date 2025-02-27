#include <GraphicsLib/Terminal.h>
#include <DT/Result.h>
#include <HILib/Intel/HPET.h>
#include <MemLib/CoreAlloc.h>
#include <HDILib/HDI.h>
#include <AHCI/AHCI.h>

static HDIVolume gMainBootVolume;
static Boolean gHDIEnabled = False;

#define HDI_STAT_BUSY 0x80
#define HDI_STAT_READY 0x40
#define HDI_STAT_DEVICE_REQ 0x08
#define HDI_STAT_DEVICE_FAULT 0x20
#define HDI_STAT_ERROR 0x01

static UInt16* 
HDIRead_PIO(struct HDIContext* drive, HDILba lba, UInt8 sectorCount);

static Boolean 
HDIWrite_PIO(struct HDIContext* drive, HDILba lba, UInt8 sectorCount, UInt16* buf);

static 
Int32 PIOWaitBusy(Void);

static 
Int32 
PIOWaitReady(Void);

static 
Int32 PIOWaitBusy(Void) {
	do {
	} while (In8(0x1F7) & HDI_STAT_BUSY);

	return 0;
}

static 
Int32 
PIOWaitReady(Void) {
	do {
	} while(!(In8(0x1F7) & HDI_STAT_READY));

	return 0;
}

HDIVolume* HDIBootVolume(void) { return &gMainBootVolume; }

Boolean HDISeekBootVolume(BootloaderHeader* pBootHdr) {
    if (pBootHdr == NULL) return False;
    gMainBootVolume.bootVolume = BootloaderTag(pBootHdr, EKBOOT_STRUCT_TAG_BOOT_VOLUME_ID);

    if (gMainBootVolume.bootVolume != NULL) {
        gMainBootVolume.isGpt = (gMainBootVolume.bootVolume->flags == 1);

        ConsoleLog( gMainBootVolume.isGpt ?  "This is a GPT bootable partition.\n" :  "This a MBR bootable partition.\n");
        return True;
    }

    return False;
}

Boolean OpenHDI(BootloaderHeader* pBootHdr) {
    if (HDIEnabled()) return False;

    if (pBootHdr != NULL) {
		if (!HDISeekBootVolume(pBootHdr)) 
			return False;
		
		gHDIEnabled = True;
		ConsoleLog("HDI is open!\r\n");
	
		return True;
    }

    return False;
}

#define ATA_PRIMARY_DATA         0x1F0
#define ATA_PRIMARY_ERR          0x1F1
#define ATA_PRIMARY_SECCOUNT     0x1F2
#define ATA_PRIMARY_LBA_LO       0x1F3
#define ATA_PRIMARY_LBA_MID      0x1F4
#define ATA_PRIMARY_LBA_HI       0x1F5
#define ATA_PRIMARY_DRIVE_HEAD   0x1F6
#define ATA_PRIMARY_COMM_REGSTAT 0x1F7
#define ATA_PRIMARY_ALTSTAT_DCR  0x3F6

#define STAT_ERR  (1 << 0) // Indicates an error occurred. Send a new command to clear it
#define STAT_DRQ  (1 << 3) // Set when the drive has PIO data to transfer, or is ready to accept PIO data.
#define STAT_SRV  (1 << 4) // Overlapped Mode Service Request.
#define STAT_DF   (1 << 5) // Drive Fault Error (does not set ERR).
#define STAT_RDY  (1 << 6) // Bit is clear when drive is spun down, or after an error. Set otherwise.
#define STAT_BSY  (1 << 7) // Bit is clear when drive is not hanging, otherwise set.

static Boolean HDIWrite_PIO(struct HDIContext* drive, HDILba lba, UInt8 sectorCount, UInt16* buf) {
	if (!HDIEnabled()) return False;
	if (drive == NULL) return False;
	if (buf == NULL) return False;
	if (WideStringLength(buf) != 256) return False; // if it's bigger than half of a sector, just refuse.
	if (drive->iStatus != 0) return False; // Just don't act when drive is having problems

	if (((drive->iFlags & HDI_PRIVILEGED_DRIVE) != HDI_PRIVILEGED_DRIVE) && lba <= 1024) { drive->iStatus = ERR_BAD_ACCESS; return False; }
	if (((drive->iFlags & HDI_PRIVILEGED_DRIVE) != HDI_PRIVILEGED_DRIVE) && sectorCount == 0) return False;

	PIOWaitBusy();
	
	Out8(0x1F6, 0xE0 | ((lba >> 24) & 0x0F));
	Out8(0x1F2, sectorCount);
	Out8(0x1F3, (UInt8)lba);
	Out8(0x1F4, (UInt8)(lba >> 8));
	Out8(0x1F5, (UInt8)(lba >> 16));

	Out8(0x1F7, 0x30); // And we finally tell him to seek, (WRITE)

	for (UInt8 index = 0; index < sectorCount; index++)
	{
		PIOWaitBusy();
		PIOWaitReady();

		ConsoleLog("%s %i %n", "Wrote buffer index", index);

		for (SizeT i = 0; i < 256; i++)
			Out16(0x1F0, buf[i]);
	}

	return True;
}

Boolean HDIGetPIODiskIO(HDIContext* hdinterface) {
	if (hdinterface) {
		hdinterface->fRead = HDIRead_PIO;
		hdinterface->fWrite = HDIWrite_PIO;

		ConsoleLog("Gave I/O the PIO interface\n");
		return True;
	}

	return False;
}

static 
UInt16* 
HDIRead_PIO(struct HDIContext* drive, HDILba lba, UInt8 sectorCount) 
{
	if (!HDIEnabled()) return NULL;
	if (drive == NULL) return NULL;
	if (drive->iStatus != 0) return NULL;

	if (((drive->iFlags & HDI_PRIVILEGED_DRIVE) != HDI_PRIVILEGED_DRIVE) && lba <= 1024) { drive->iStatus = ERR_BAD_ACCESS; return NULL; }
	if (((drive->iFlags & HDI_PRIVILEGED_DRIVE) != HDI_PRIVILEGED_DRIVE) && sectorCount == 0) return NULL;

	PIOWaitBusy();

	Out8(0x1F6, 0xE0 | ((lba >> 24) & 0x0F));
	Out8(0x1F2, sectorCount);
	Out8(0x1F3, (UInt8)lba);
	Out8(0x1F4, (UInt8)(lba >> 8));
	Out8(0x1F5, (UInt8)(lba >> 16));
	Out8(0x1F7, 0x20); // And we finally tell him to seek, (READ)

	UInt16* mem = MemAlloc((sizeof(Char) * (256 * sectorCount)));

	for (int index = 0; index < sectorCount; index++)
	{
		PIOWaitBusy();
		PIOWaitReady();

		for (SizeT i = 0; i < 256; i++) {
			mem[i] = In16(0x1F0);
		}

		mem += 256;
	}

	ConsoleLog("Done.\n");
	return mem;
}


HDILba 
HDITranslateChs(HDIInt64 cylinder, HDIInt64 head, HDIInt64 sector) { return ((cylinder * head * sector) + (head * sector) + sector - 1); }

HDIContext* 
HDICreateContext(HDIChar* hdName, UInt16 iFlags, Boolean isPrivileged) 
{
	if (IsNull(hdName)) return NULL;
	if (WideStringLength(hdName) > 16) return NULL;

	HDIContext* drive = MemAlloc(sizeof(HDIContext));
	if (drive == NULL) return NULL;

	drive->iStatus = 0;

	CopyMem(hdName, drive->strName, WideStringLength(hdName)); // then give it a name

	drive->iFlags |= isPrivileged ? HDI_PRIVILEGED_DRIVE : 0x0;
	drive->iFlags |= iFlags;

	CopyMem("No FileSystem", drive->strDriveFs, 14);
	ConsoleLog("Created a new Hard Drive Interface!\n");

	return drive;
}

Boolean 
HDIEnabled(Void) 
{ 
	return gHDIEnabled; 
}