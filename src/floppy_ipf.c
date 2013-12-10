/*
  Hatari - floppy_ipf.c

  This file is distributed under the GNU General Public License, version 2 or at
  your option any later version. Read the file gpl.txt for details.

  IPF disk image support.

  IPF files are handled using the capsimage library, which emulates the FDC
  at low level and allows to read complex protections.
*/
const char floppy_ipf_fileid[] = "Hatari floppy_ipf.c : " __DATE__ " " __TIME__;

#include "main.h"
#include "file.h"
#include "floppy.h"
#include "floppy_ipf.h"
#include "fdc.h"
#include "log.h"
#include "memorySnapShot.h"
#include "screen.h"
#include "video.h"
#include "cycles.h"

#ifdef HAVE_CAPSIMAGE
#include <caps/fdc.h>
#endif


typedef struct
{
#ifdef HAVE_CAPSIMAGE
	Uint32			CapsLibRelease;
	Uint32			CapsLibRevision;

	struct CapsFdc		Fdc;				/* Fdc state */
	struct CapsDrive 	Drive[ MAX_FLOPPYDRIVES ];	/* Physical drives */
	CapsLong		CapsImage[ MAX_FLOPPYDRIVES ];	/* For the IPF disk images */

	Sint64			FdcClock;			/* Current value of CyclesGlobalClockCounter */
#endif
} IPF_STRUCT;


static IPF_STRUCT	IPF_State;			/* All variables related to the IPF support */


#ifdef HAVE_CAPSIMAGE
static void	IPF_CallBack_Trk ( struct CapsFdc *pc , CapsULong State );
static void	IPF_CallBack_Irq ( struct CapsFdc *pc , CapsULong State );
static void	IPF_CallBack_Drq ( struct CapsFdc *pc , CapsULong State );
#endif




/*-----------------------------------------------------------------------*/
/**
 * Save/Restore snapshot of local variables('MemorySnapShot_Store' handles type)
 * We must take care of whether Hatari was compiled with IPF support of not
 * when saving/restoring snapshots to avoid incompatibilies.
 */
void IPF_MemorySnapShot_Capture(bool bSave)
{
	int	StructSize;
	int	Drive;

	if ( bSave )					/* Saving snapshot */
	{
		StructSize = sizeof ( IPF_State );	/* 0 if HAVE_CAPSIMAGE is not defined */
		MemorySnapShot_Store(&StructSize, sizeof(StructSize));
fprintf ( stderr , "ipf save %d\n" , StructSize );
		if ( StructSize > 0 )
			MemorySnapShot_Store(&IPF_State, sizeof(IPF_State));
	}

	else						/* Restoring snapshot */
	{
		MemorySnapShot_Store(&StructSize, sizeof(StructSize));
fprintf ( stderr , "ipf load %d\n" , StructSize );
		if ( ( StructSize == 0 ) && ( sizeof ( IPF_State ) > 0 ) )
		{
			Log_AlertDlg(LOG_ERROR, "This memory snapshot doesn't include IPF data but this version of Hatari was built with IPF support");
			return;				/* Continue restoring the rest of the memory snapshot */
		}
		else if ( ( StructSize > 0 ) && ( sizeof ( IPF_State ) == 0 ) )
		{
			Log_AlertDlg(LOG_ERROR, "This memory snapshot includes IPF data but this version of Hatari was not built with IPF support");
			MemorySnapShot_Skip( StructSize );	/* Ignore the IPF data */
			return;				/* Continue restoring the rest of the memory snapshot */
		}
		else if ( ( StructSize > 0 ) && ( StructSize != sizeof ( IPF_State ) ) )
		{
			Log_AlertDlg(LOG_ERROR, "This memory snapshot includes IPF data different from the ones handled in this version of Hatari");
			MemorySnapShot_Skip( StructSize );	/* Ignore the IPF data */
			return;				/* Continue restoring the rest of the memory snapshot */
		}

		if ( StructSize > 0 )
		{
			MemorySnapShot_Store(&IPF_State, sizeof(IPF_State));

#ifdef HAVE_CAPSIMAGE
			/* For IPF structures, we need to update some pointers in Fdc/Drive/CapsImage */
			/* drive : PUBYTE trackbuf, PUDWORD timebuf */
			/* fdc : PCAPSDRIVE driveprc, PCAPSDRIVE drive, CAPSFDCHOOK callback functions */
			CAPSFdcInvalidateTrack ( &IPF_State.Fdc , 0 );	/* Invalidate buffered track data for drive 0 */
			CAPSFdcInvalidateTrack ( &IPF_State.Fdc , 1 );	/* Invalidate buffered track data for drive 1 */

			IPF_State.Fdc.drive = IPF_State.Drive;		/* Connect drives array to the FDC */
			if ( IPF_State.Fdc.driveprc != NULL )		/* Recompute active drive's pointer */
				IPF_State.Fdc.driveprc = IPF_State.Fdc.drive + IPF_State.Fdc.driveact;

			/* Set callback functions */
			IPF_State.Fdc.cbirq = IPF_CallBack_Irq;
			IPF_State.Fdc.cbdrq = IPF_CallBack_Drq;
			IPF_State.Fdc.cbtrk = IPF_CallBack_Trk;
#endif

			/* Call IPF_Insert to recompute IPF_State.CapsImage[ Drive ] */
			for ( Drive=0 ; Drive < MAX_FLOPPYDRIVES ; Drive++ )
				if ( EmulationDrives[Drive].ImageType == FLOPPY_IMAGE_TYPE_IPF )
					if ( IPF_Insert ( Drive , EmulationDrives[Drive].pBuffer , EmulationDrives[Drive].nImageBytes ) == false )
					{
						Log_AlertDlg(LOG_ERROR, "Error restoring IPF image %s in drive %d" ,
							EmulationDrives[Drive].sFileName , Drive );
						return;
					}

		fprintf ( stderr , "ipf load ok\n" );
		}
	}
}




/*-----------------------------------------------------------------------*/
/**
 * Does filename end with a .IPF extension? If so, return true.
 */
bool IPF_FileNameIsIPF(const char *pszFileName, bool bAllowGZ)
{
	return(File_DoesFileExtensionMatch(pszFileName,".ipf")
	       || (bAllowGZ && File_DoesFileExtensionMatch(pszFileName,".ipf.gz")));
}


/*-----------------------------------------------------------------------*/
/**
 * Load .IPF file into memory, set number of bytes loaded and return a pointer
 * to the buffer.
 */
Uint8 *IPF_ReadDisk(const char *pszFileName, long *pImageSize, int *pImageType)
{
#ifndef HAVE_CAPSIMAGE
	Log_AlertDlg(LOG_ERROR, "This version of Hatari was not built with IPF support, this disk image can't be handled.");
	return NULL;

#else
	Uint8 *pIPFFile;

	*pImageSize = 0;

	/* Just load directly a buffer, and set ImageSize accordingly */
	pIPFFile = File_Read(pszFileName, pImageSize, NULL);
	if (!pIPFFile)
	{
		*pImageSize = 0;
		return NULL;
	}
	
	*pImageType = FLOPPY_IMAGE_TYPE_IPF;
	return pIPFFile;
#endif
}


/*-----------------------------------------------------------------------*/
/**
 * Save .IPF file from memory buffer. Returns true is all OK.
 */
bool IPF_WriteDisk(const char *pszFileName, Uint8 *pBuffer, int ImageSize)
{
	/* saving is not supported for IPF files */
	return false;
}




/*
 * Init the FDC and the drives used to handle IPF images
 */
bool	IPF_Init ( void )
{
#ifndef HAVE_CAPSIMAGE
	return true;

#else
	int	i;
	struct CapsVersionInfo	caps_vi;

	fprintf ( stderr , "IPF : IPF_Init\n" );

	if ( CAPSInit() != imgeOk )
        {
		fprintf ( stderr , "IPF : Could not initialize the capsimage library\n" );
		return false;
        }

	if ( CAPSGetVersionInfo ( &caps_vi , 0 ) != imgeOk )
        {
		fprintf ( stderr , "IPF : CAPSVersionInfo failed\n" );
		return false;
        }
	fprintf ( stderr , "IPF : capsimage library version release=%d revision=%d\n" , caps_vi.release , caps_vi.revision );
	IPF_State.CapsLibRelease = caps_vi.release;
	IPF_State.CapsLibRevision = caps_vi.revision;

	/* Default values for each physical drive */
	memset ( IPF_State.Drive , 0 , sizeof ( IPF_State.Drive ) );
	for ( i=0 ; i < MAX_FLOPPYDRIVES ; i++ )
	{
		IPF_State.Drive[ i ].type = sizeof ( struct CapsDrive );
		IPF_State.Drive[ i ].rpm = CAPSDRIVE_35DD_RPM;
		IPF_State.Drive[ i ].maxtrack = CAPSDRIVE_35DD_HST;
	}

	/* Init FDC with 2 physical drives */
	memset ( &IPF_State.Fdc , 0 , sizeof ( IPF_State.Fdc ) );
	IPF_State.Fdc.type = sizeof( struct CapsFdc );
	IPF_State.Fdc.model = cfdcmWD1772;
	IPF_State.Fdc.drive = IPF_State.Drive;
	IPF_State.Fdc.drivecnt = MAX_FLOPPYDRIVES;

	if ( CAPSFdcInit ( &IPF_State.Fdc ) != imgeOk)
	{
		fprintf ( stderr , "IPF : CAPSFdcInit failed\n" );
		return false;
	}

	/* 2 drives */
	IPF_State.Fdc.drivemax = MAX_FLOPPYDRIVES;

	/* FDC clock */
	IPF_State.Fdc.clockfrq = 8000000;

	/* Set callback functions */
	IPF_State.Fdc.cbirq = IPF_CallBack_Irq;
	IPF_State.Fdc.cbdrq = IPF_CallBack_Drq;
	IPF_State.Fdc.cbtrk = IPF_CallBack_Trk;

	CAPSFdcReset ( &IPF_State.Fdc );

	return true;
#endif
}




/*
 * Init the ressources to handle the IPF image inserted into a drive (0=A: 1=B:)
 */
bool	IPF_Insert ( int Drive , Uint8 *pImageBuffer , long ImageSize )
{
#ifndef HAVE_CAPSIMAGE
	return false;

#else
	CapsLong	ImageId;

	ImageId = CAPSAddImage();
	if ( ImageId < 0 )
	{
		fprintf ( stderr , "IPF : error CAPSAddImage\n" );
		return false;
	}

	fprintf ( stderr , "IPF : IPF_Insert drive=%d buf=%p size=%ld imageid=%d\n" , Drive , pImageBuffer , ImageSize , ImageId );

	if ( CAPSLockImageMemory ( ImageId , pImageBuffer , (CapsULong)ImageSize , DI_LOCK_MEMREF ) == imgeOk )
	{
		struct CapsImageInfo cii;
		int		i;

		/* Print some debug infos */
		if ( CAPSGetImageInfo ( &cii , ImageId ) == imgeOk )
		{
			printf("Type: %d\n", (int)cii.type);
			printf("Release: %d\n", (int)cii.release);
			printf("Revision: %d\n", (int)cii.revision);
			printf("Min Cylinder: %d\n", (int)cii.mincylinder);
			printf("Max Cylinder: %d\n", (int)cii.maxcylinder);
			printf("Min Head: %d\n", (int)cii.minhead);
			printf("Max Head: %d\n", (int)cii.maxhead);
			printf("Creation Date: %04d/%02d/%02d %02d:%02d:%02d.%03d\n", (int)cii.crdt.year, (int)cii.crdt.month, (int)cii.crdt.day, (int)cii.crdt.hour, (int)cii.crdt.min, (int)cii.crdt.sec, (int)cii.crdt.tick);
			printf("Platforms:");
			for (i = 0; i < CAPS_MAXPLATFORM; i++)
				if (cii.platform[i] != ciipNA)
					printf ( " %s" , CAPSGetPlatformName(cii.platform[i]) );
			printf("\n");
		}
	}
	else
	{
		CAPSRemImage ( ImageId ) ;
		return false;
	}

	if ( CAPSLoadImage ( ImageId , DI_LOCK_DENALT | DI_LOCK_DENVAR | DI_LOCK_UPDATEFD ) != imgeOk )
	{
		fprintf ( stderr , "IPF : error CAPSLoadImage\n" );
		CAPSUnlockImage ( ImageId );
		CAPSRemImage ( ImageId ) ;
		return false;
	}

	
	IPF_State.CapsImage[ Drive ] = ImageId;

	IPF_State.Drive[ Drive ].diskattr |= ( CAPSDRIVE_DA_IN | CAPSDRIVE_DA_WP );	/* Disk inserted and write protected */

	return true;
#endif
}




/*
 * When ejecting a disk, free the ressources associated with an IPF image
 */
bool	IPF_Eject ( int Drive )
{
#ifndef HAVE_CAPSIMAGE
	return false;

#else
	fprintf ( stderr , "IPF : IPF_Eject drive=%d imageid=%d\n" , Drive , IPF_State.CapsImage[ Drive ] );

	if ( CAPSUnlockImage ( IPF_State.CapsImage[ Drive ] ) < 0 )
	{
		fprintf ( stderr , "IPF : error CAPSUnlockImage\n" );
		return false;
	}

	if ( CAPSRemImage ( IPF_State.CapsImage[ Drive ] ) < 0 )
	{
		fprintf ( stderr , "IPF : error CAPSRemImage\n" );
		return false;
	}

	IPF_State.CapsImage[ Drive ] = -1;

	IPF_State.Drive[ Drive ].diskattr &= ~CAPSDRIVE_DA_IN;

	return true;
#endif
}




/*
 * Reset FDC state when a reset signal was received
 */
void IPF_Reset ( void )
{
#ifdef HAVE_CAPSIMAGE
	CAPSFdcReset ( &IPF_State.Fdc );

	IPF_State.FdcClock = CyclesGlobalClockCounter;
#endif
}




/*
 * Callback function used when track is changed.
 * We need to update the track data by calling CAPSLockTrack
 */
#ifdef HAVE_CAPSIMAGE
static void	IPF_CallBack_Trk ( struct CapsFdc *pc , CapsULong State )
{
	int	Drive = State;				/* State is the drive number in that case */
	struct CapsDrive *pd = pc->drive+Drive;		/* Current drive where the track change occurred */
	struct CapsTrackInfoT1 cti;

	cti.type=1;
	if ( CAPSLockTrack ( &cti , IPF_State.CapsImage[ Drive ] , pd->buftrack , pd->bufside ,
			DI_LOCK_DENALT|DI_LOCK_DENVAR|DI_LOCK_UPDATEFD|DI_LOCK_TYPE ) != imgeOk )
		return;

	LOG_TRACE(TRACE_FDC, "fdc ipf callback trk drive=%d buftrack=%d bufside=%d VBL=%d HBL=%d\n" , Drive ,
		  pd->buftrack , pd->bufside , nVBLs , nHBL );

	pd->ttype	= cti.type;
	pd->trackbuf	= cti.trackbuf;
	pd->timebuf	= cti.timebuf;
	pd->tracklen	= cti.tracklen;
	pd->overlap	= cti.overlap;
}
#endif




/*
 * Callback function used when the FDC change the IRQ signal
 */
#ifdef HAVE_CAPSIMAGE
static void	IPF_CallBack_Irq ( struct CapsFdc *pc , CapsULong State )
{
	LOG_TRACE(TRACE_FDC, "fdc ipf callback irq state=0x%x VBL=%d HBL=%d\n" , State , nVBLs , nHBL );

	if ( State )
		FDC_SetIRQ();				/* IRQ bit was set */
	else
		FDC_ClearIRQ();				/* IRQ bit was reset */
}
#endif




/*
 * Callback function used when the FDC change the DRQ signal
 * -> copy the byte to/from the DMA's FIFO if it's a read or a write to the disk
 */
#ifdef HAVE_CAPSIMAGE
static void	IPF_CallBack_Drq ( struct CapsFdc *pc , CapsULong State )
{
	Uint8	Byte;

	if ( State == 0 )
		return;					/* DRQ bit was reset, do nothing */

	if ( FDC_DMA_GetModeControl_R_WR () != 0 )	/* DMA write mode */
	{
		Byte = FDC_DMA_FIFO_Pull ();		/* Get a byte from the DMA FIFO */
		CAPSFdcWrite ( &IPF_State.Fdc , 3 , Byte );	/* Write to FDC's reg 3 */

		LOG_TRACE(TRACE_FDC, "fdc ipf callback drq state=0x%x write byte 0x%02x VBL=%d HBL=%d\n" , State , Byte , nVBLs , nHBL );
	}

	else						/* DMA read mode */
	{
		Byte = CAPSFdcRead ( &IPF_State.Fdc , 3 );	/* Read from FDC's reg 3 */
		FDC_DMA_FIFO_Push ( Byte );		/* Add byte to the DMA FIFO */

		LOG_TRACE(TRACE_FDC, "fdc ipf callback drq state=0x%x read byte 0x%02x VBL=%d HBL=%d\n" , State , Byte , nVBLs , nHBL );
	}
}
#endif




/*
 * Set the drive and the side to be used for the next FDC commands
 * io_porta_old is the previous value, io_porta_new is the new value
 * to take into account.
 * We report a side change only when a drive is selected.
 */
void	IPF_SetDriveSide ( Uint8 io_porta_old , Uint8 io_porta_new )
{
#ifndef HAVE_CAPSIMAGE
	return;

#else
	int	Side;

	LOG_TRACE(TRACE_FDC, "fdc ipf change drive/side io_porta_old=0x%x io_porta_new=0x%x VBL=%d HBL=%d\n" , io_porta_old , io_porta_new , nVBLs , nHBL );

	Side = ( (~io_porta_new) & 0x01 );		/* Side 0 or 1 */

	IPF_State.Fdc.drivenew = -1;			/* By default, don't select any drive */

	/* Check drive 1 first */
	if ( ( io_porta_new & 0x04 ) == 0 )
	{
		IPF_State.Drive[ 1 ].newside = Side;
		IPF_State.Fdc.drivenew = 1;		/* Select drive 1 */
	}

	/* If both drive 0 and drive 1 are enabled, we keep only drive 0 as newdrive */
	if ( ( io_porta_new & 0x02 ) == 0 )
	{
		IPF_State.Drive[ 0 ].newside = Side;
		IPF_State.Fdc.drivenew = 0;		/* Select drive 0 (and un-select drive 1 if set above) */
	}

	IPF_Emulate();					/* Update emulation's state up to this point, then set new drive/side */
#endif
}




/*
 * Write a byte into one of the FDC registers
 * 0=command   1=track   2=sector   3=data
 */
void	IPF_FDC_WriteReg ( Uint8 Reg , Uint8 Byte )
{
#ifndef HAVE_CAPSIMAGE
	return;						/* This should not be reached (an IPF image can't be inserted without capsimage) */

#else
	LOG_TRACE(TRACE_FDC, "fdc ipf write reg=%d data=0x%x VBL=%d HBL=%d\n" , Reg , Byte , nVBLs , nHBL );

	IPF_Emulate();					/* Update emulation's state up to this point */

	CAPSFdcWrite ( &IPF_State.Fdc , Reg , Byte );
#endif
}




/*
 * Read the content of one of the FDC registers
 * 0=command   1=track   2=sector   3=data
 */
Uint8	IPF_FDC_ReadReg ( Uint8 Reg )
{
#ifndef HAVE_CAPSIMAGE
	return 0;					/* This should not be reached (an IPF image can't be inserted without capsimage) */
#else
	Uint8	Byte;

	IPF_Emulate();					/* Update emulation's state up to this point */

	Byte = CAPSFdcRead ( &IPF_State.Fdc , Reg );
	LOG_TRACE(TRACE_FDC, "fdc ipf read reg=%d data=0x%x VBL=%d HBL=%d\n" , Reg , Byte , nVBLs , nHBL );

	return Byte;
#endif
}




/*
 * Run the FDC emulation during NbCycles cycles (relative to the 8MHz FDC's clock)
 */
void	IPF_Emulate ( void )
{
#ifndef HAVE_CAPSIMAGE
	return;

#else
	int NbCycles;

	NbCycles = CyclesGlobalClockCounter - IPF_State.FdcClock;	/* Number of cycles since last emulation */
	if ( NbCycles < 0 )
		NbCycles = 0;						/* We should call CAPSFdcEmulate even when NbCycles=0 */

	LOG_TRACE(TRACE_FDC, "fdc ipf emulate cycles=%d VBL=%d HBL=%d clock=%lld\n" , NbCycles , nVBLs , nHBL , CyclesGlobalClockCounter );

	CAPSFdcEmulate ( &IPF_State.Fdc , NbCycles );			/* Process at max NbCycles */
	IPF_State.FdcClock += IPF_State.Fdc.clockact;			/* clockact can be < NbCycle in some cases */
#endif
}



