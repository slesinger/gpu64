/*
 gpu64: HDMI on-screen log, for Tier 2 (SD card in the real cartridge) bring-up.

 gpioInit() reprograms GPIO14/15 (the UART pins) to their cartridge-latch ALT
 functions, so once the RAD hijack starts, the serial console (Tier 1) is
 physically unreachable -- there's no UART left to watch. HDMI stays available
 the whole time, so logger output is tee'd to both.

 IMPORTANT: neither CSerialDevice::Write() nor CScreenDevice::Write() can be
 used for this once RAD calls DisableIRQs() (required for its cycle-precise
 C64 bus timing, and left disabled for essentially the entire runtime after
 boot): with REALTIME defined (Source/Firmware/Circle/sysconfig.h), both
 classes check CurrentExecutionLevel() and silently no-op whenever the IRQ
 mask bit is set. This is exactly what was observed on hardware: log lines
 before the first DisableIRQs() call showed up fine on HDMI, every one after
 it silently vanished, with no hang.

 So CHDMIConsole below bypasses Circle's text console entirely. Since
 milestone 4 it is a thin adapter onto CGpu64FrameBuffer's log overlay: the
 display is now gpu64's own 320x200x8 framebuffer (see gpu64_fb.h), the log is
 40 columns x 12 rows drawn straight into it in one reserved palette entry,
 and none of it goes through CScreenDevice.
*/
#ifndef _tee_device_h
#define _tee_device_h

#include <circle/device.h>
#include <circle/serial.h>
#include <circle/types.h>
#include "gpu64_fb.h"

class CHDMIConsole : public CDevice
{
public:
	CHDMIConsole( CGpu64FrameBuffer *pFB )
	:	m_pFB( pFB )
	{
	}

	int Write( const void *pBuffer, size_t nCount ) override
	{
		// The overlay owns its own ring buffer, glyph rendering and cache
		// clean -- see CGpu64FrameBuffer::LogWrite(). Writes that arrive
		// before the framebuffer is up are still accumulated there, so the
		// earliest boot lines appear as soon as it initializes.
		if ( m_pFB )
			m_pFB->LogWrite( (const char *)pBuffer, (unsigned)nCount );
		return (int)nCount;
	}

private:
	CGpu64FrameBuffer *m_pFB;
};

class CTeeDevice : public CDevice
{
public:
	CTeeDevice( CSerialDevice *pSerial, CHDMIConsole *pHDMI )
	:	m_pSerial( pSerial ), m_pHDMI( pHDMI )
	{
	}

	int Write( const void *pBuffer, size_t nCount ) override
	{
		// gpu64: both targets silently no-op once IRQs are disabled (see
		// CSerialDevice::Write()'s own REALTIME guard) -- m_pSerial is kept
		// here regardless since it's still correct and free on the bench
		// (Tier 1, IRQs never disabled there). m_pHDMI bypasses that guard
		// entirely (SetPixel-based), so it's the one that actually works
		// once the hijack starts.
		if ( m_pSerial )
			m_pSerial->Write( pBuffer, nCount );
		if ( m_pHDMI )
			m_pHDMI->Write( pBuffer, nCount );
		return (int)nCount;
	}

private:
	CSerialDevice *m_pSerial;
	CHDMIConsole  *m_pHDMI;
};

#endif
