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
 mask bit is set -- Circle's DAIF-based level check can't distinguish "really
 inside an IRQ handler" from "IRQs are just globally masked", so it treats
 both as unsafe to write from. This is exactly what was observed on hardware:
 log lines before the first DisableIRQs() call showed up fine on HDMI, every
 one after it silently vanished, with no hang -- RAD's own bus-hijack loop
 (SetPixel-based, see showTestPattern()) kept working the whole time because
 CScreenDevice::SetPixel() has no such guard.

 So CHDMIConsole below bypasses CScreenDevice's text console entirely and
 blits characters directly via SetPixel(), using Circle's own CCharGenerator
 for correct, already-tested font data (decoupled from RAD's own font_bin,
 which the menu code repurposes as a mutable scratch canvas for its logo --
 not safe to reuse for arbitrary text).

 Layout: a reserved box in the top-left for the future C64-video-passthrough
 output (currently just showTestPattern()'s checkerboard), and the on-screen
 log in the remaining column to its right, using the full screen height.
*/
#ifndef _tee_device_h
#define _tee_device_h

#include <circle/device.h>
#include <circle/serial.h>
#include <circle/screen.h>
#include <circle/chargenerator.h>
#include <circle/bcmframebuffer.h>
#include <circle/synchronize.h>
#include <circle/types.h>

// Reserved top-left box for the eventual upscaled C64 image (320x200 doubled
// = 640x400) plus a border margin. showTestPattern() draws within this same
// box so it never collides with the log column beside it.
#define GPU_OUTPUT_BOX_W	700
#define GPU_OUTPUT_BOX_H	460

// On-screen log column: starts right of the reserved box, uses full height.
// Native char-generator size (no doubling) to fit more lines on screen.
#define HDMI_LOG_X0			(GPU_OUTPUT_BOX_W + 20)
#define HDMI_LOG_SCALE		1

class CHDMIConsole : public CDevice
{
public:
	CHDMIConsole( CScreenDevice *pScreen )
	:	m_pScreen( pScreen ), m_CurRow( 0 ), m_CurCol( 0 )
	{
		m_nCharW = m_CharGen.GetCharWidth() * HDMI_LOG_SCALE;
		m_nCharH = m_CharGen.GetCharHeight() * HDMI_LOG_SCALE;

		unsigned w = m_pScreen->GetWidth();
		unsigned h = m_pScreen->GetHeight();

		m_nCols = ( m_nCharW && w > HDMI_LOG_X0 ) ? ( w - HDMI_LOG_X0 ) / m_nCharW : 0;
		m_nRows = m_nCharH ? h / m_nCharH : 0;
	}

	int Write( const void *pBuffer, size_t nCount ) override
	{
		if ( !m_pScreen || m_nCharW == 0 || m_nCharH == 0 )
			return (int)nCount;

		unsigned rowStart = m_CurRow;

		const char *p = (const char *)pBuffer;
		for ( size_t i = 0; i < nCount; i++ )
			WriteChar( p[ i ] );

		// SetPixel() writes land in cache; the GPU scans out DRAM directly
		// and isn't cache-coherent with the ARM core (same issue as
		// showTestPattern()). RAD's bus-hijack loop is cycle-precise, so
		// (unlike showTestPattern(), a one-off call) flushing the WHOLE
		// framebuffer here -- which can fire many times during live
		// operation, not just at boot -- would burn enough cycles per call
		// to blow the per-rasterline timing budget and corrupt the C64
		// display. Only clean the row(s) actually touched by this call.
		unsigned rowEnd = m_CurRow;
		CBcmFrameBuffer *pFB = m_pScreen->GetFrameBuffer();
		u32 nPitch = pFB->GetPitch();
		u32 nBuffer = pFB->GetBuffer();
		unsigned hFull = m_pScreen->GetHeight();

		unsigned yStart, nRowsDirty;
		if ( rowEnd >= rowStart )
		{
			yStart = rowStart * m_nCharH;
			nRowsDirty = ( rowEnd - rowStart + 1 );
		}
		else
		{
			// wrapped around back to row 0 mid-call -- just flush everything
			yStart = 0;
			nRowsDirty = m_nRows;
		}
		unsigned yLines = nRowsDirty * m_nCharH;
		if ( yStart + yLines > hFull )
			yLines = hFull - yStart;

		CleanDataCacheRange( (u64)(uintptr)( nBuffer + yStart * nPitch ), yLines * nPitch );

		return (int)nCount;
	}

private:
	void ClearRow( unsigned row )
	{
		unsigned wFull = m_pScreen->GetWidth();
		unsigned hFull = m_pScreen->GetHeight();
		unsigned y0 = row * m_nCharH;
		for ( unsigned y = y0; y < y0 + m_nCharH && y < hFull; y++ )
			for ( unsigned x = HDMI_LOG_X0; x < wFull; x++ )
				m_pScreen->SetPixel( x, y, BLACK_COLOR );
	}

	void NewLine( void )
	{
		m_CurCol = 0;
		m_CurRow++;
		if ( m_nRows && m_CurRow >= m_nRows )
			m_CurRow = 0;
		ClearRow( m_CurRow );
	}

	void WriteChar( char c )
	{
		if ( c == '\n' ) { NewLine(); return; }
		if ( c == '\r' ) return;

		if ( m_nCols && m_CurCol >= m_nCols )
			NewLine();

		unsigned x0 = HDMI_LOG_X0 + m_CurCol * m_nCharW;
		unsigned y0 = m_CurRow * m_nCharH;

		unsigned cw = m_CharGen.GetCharWidth();
		unsigned ch = m_CharGen.GetCharHeight();

		for ( unsigned gy = 0; gy < ch; gy++ )
		{
			for ( unsigned gx = 0; gx < cw; gx++ )
			{
				boolean bLit = m_CharGen.GetPixel( c, gx, gy );
				TScreenColor col = bLit ? NORMAL_COLOR : BLACK_COLOR;
				for ( unsigned sy = 0; sy < HDMI_LOG_SCALE; sy++ )
					for ( unsigned sx = 0; sx < HDMI_LOG_SCALE; sx++ )
						m_pScreen->SetPixel( x0 + gx * HDMI_LOG_SCALE + sx,
											  y0 + gy * HDMI_LOG_SCALE + sy, col );
			}
		}

		m_CurCol++;
	}

	CScreenDevice   *m_pScreen;
	CCharGenerator   m_CharGen;
	unsigned         m_nCharW, m_nCharH;
	unsigned         m_nCols, m_nRows;
	unsigned         m_CurRow, m_CurCol;
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
