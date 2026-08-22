/*
 gpu64: the 320x200x8 framebuffer the command API draws into.

 This owns the HDMI display outright ("architecture A", see
 docs/milestone4_2d_api_design.md#display-architecture): one CBcmFrameBuffer
 at 320x200, depth 8, virtual height 400, so the VideoCore gives us a real
 256-entry 24-bit palette, a real hardware page flip (SetVirtualOffset), and
 upscaling to whatever HDMI mode is attached -- none of which costs ARM time
 inside the bus-watch loop.

 That replaces the previous arrangement, where CScreenDevice's COLOR16
 framebuffer was the display and gpu64 drew into it via SetPixel(). Only one
 framebuffer can own the display, so CScreenDevice is out of the picture
 entirely; the on-screen log now lives here too, as an overlay drawn in one
 reserved palette entry (see GPU64_LOG_INK).
*/
#ifndef _gpu64_fb_h
#define _gpu64_fb_h

#include <circle/bcmframebuffer.h>
#include <circle/types.h>

// The drawing surface. All API coordinates are in this space, with (0,0) at
// its top-left -- the border below is outside it and is not addressable by
// any draw op, exactly like the C64's.
#define GPU64_FB_WIDTH		320
#define GPU64_FB_HEIGHT		200
#define GPU64_FB_PAGES		2

// gpu64: the border. The physical framebuffer is larger than the drawing
// surface and the surface sits centred in it, so SET_BORDER can paint the
// frame the way $D020 does. Sized like the C64's own visible border rather
// than the full PAL overscan: 384x272 total is the geometry emulators
// conventionally use.
#define GPU64_BORDER_W		32
#define GPU64_BORDER_H		36
#define GPU64_FB_TOTAL_W	( GPU64_FB_WIDTH + 2 * GPU64_BORDER_W )		// 384
#define GPU64_FB_TOTAL_H	( GPU64_FB_HEIGHT + 2 * GPU64_BORDER_H )	// 272

// gpu64: reserved palette entry the on-screen log draws its glyph pixels in.
// The API deliberately does not forbid a program from using index 255 as an
// ordinary colour or repointing it with PAL_SET -- the log just draws in
// whatever that entry holds. See docs/api_design.md.
#define GPU64_LOG_INK		255

// Log overlay geometry. This used to use Circle's CCharGenerator, whose only
// face is 8x16 -- 40 columns by 12 rows on a 200-pixel-tall surface, which
// made the log unreadable on a real display: boot messages wrapped, and 12
// lines of scrollback is nothing. gpu64_font8x8.h carries the same Linux
// console font family at half the height, so the same text now gets 25 rows.
#define GPU64_LOG_CHAR_W	8
#define GPU64_LOG_CHAR_H	8
#define GPU64_LOG_COLS		( GPU64_FB_WIDTH / GPU64_LOG_CHAR_W )	// 40
#define GPU64_LOG_ROWS		( GPU64_FB_HEIGHT / GPU64_LOG_CHAR_H )	// 25

class CGpu64FrameBuffer
{
public:
	CGpu64FrameBuffer( void );
	~CGpu64FrameBuffer( void );

	boolean Initialize( void );
	boolean IsInitialized( void ) const	{ return m_bInitialized; }

	// --- pages ---------------------------------------------------------
	// Top-left of the *drawing surface* within the page, so every caller
	// can keep indexing p[ y * pitch + x ] in 320x200 coordinates and never
	// think about the border.
	u8 *PageBuffer( unsigned nPage );
	// Top-left of the whole physical page, border included.
	u8 *PageBase( unsigned nPage );
	unsigned GetPitch( void ) const		{ return m_nPitch; }
	u8 GetDrawPage( void ) const		{ return m_nDrawPage; }
	u8 GetVisiblePage( void ) const		{ return m_nVisiblePage; }
	void SetDrawPage( u8 nPage );
	// Swaps draw and visible page. Returns FALSE if the mailbox call failed.
	boolean Flip( void );
	// The two halves of Flip(), for the vblank-deferred case. PrepareFlip()
	// does everything expensive -- the log overlay and the cache clean --
	// and runs during the command dispatch, where the C64 is halted anyway.
	// CommitFlip() is then just the SetVirtualOffset the frame boundary is
	// actually waiting for, which is what the bus-watch loop runs.
	void PrepareFlip( void );
	boolean CommitFlip( void );
	// Back to the reset arrangement: page 0 drawn and visible.
	void ResetPages( void );

	// Blocks until the display's next vertical sync. A mailbox round-trip
	// to the VideoCore, so this is boot- and setup-time only -- see
	// gpu64_vsync.h. Exposed because the frame clock is calibrated against
	// it and m_pFB is private.
	boolean WaitForVSync( void );

	// --- drawing (all act on the draw page, all clip) -------------------
	void Clear( u8 nColor );
	void SetPixel( int x, int y, u8 nColor );
	void Line( int x0, int y0, int x1, int y1, u8 nColor );
	void Rect( int x, int y, int w, int h, u8 nColor );
	void RectFill( int x, int y, int w, int h, u8 nColor );
	// nKey < 0 = opaque blit; nKey >= 0 = source pixels equal to it are skipped
	void Blit( const u8 *pSrc, int x, int y, unsigned w, unsigned h, int nKey );
	// Not clipped -- the caller must have validated the rectangle (READ_RECT
	// is specified as an error, not a clip, when it runs off the page).
	void ReadRect( u8 *pDst, unsigned x, unsigned y, unsigned w, unsigned h );

	// --- border ---------------------------------------------------------
	// Paints the frame around the drawing surface on *both* pages: the
	// border is a property of the display, not of the page being animated,
	// so a PAGE_FLIP must never make it change colour.
	void SetBorder( u8 nColor );
	u8 GetBorder( void ) const		{ return m_nBorder; }

	// --- palette --------------------------------------------------------
	void SetPaletteEntry( u8 nIndex, u8 r, u8 g, u8 b );
	boolean CommitPalette( void );

	// --- on-screen log overlay -----------------------------------------
	void LogWrite( const char *pString, unsigned nLength );
	void LogEnable( boolean bEnable );
	boolean LogEnabled( void ) const	{ return m_bLogEnabled; }

	// Pushes CPU-side writes out to DRAM. The VideoCore scans out DRAM
	// directly and is not cache-coherent with the ARM core, so nothing drawn
	// is visible until this runs. Rows are relative to the drawing surface,
	// not to the physical page; whole physical rows get cleaned either way,
	// which costs nothing extra and keeps the border covered.
	void CleanRows( unsigned nPage, unsigned y0, unsigned y1 );
	// Whole physical page, border included.
	void CleanPage( unsigned nPage );

private:
	void DrawLogOverlay( unsigned nPage );
	void LogChar( char c );

	CBcmFrameBuffer	*m_pFB;
	u8		*m_pBuffer;
	unsigned	m_nPitch;
	boolean		m_bInitialized;

	u8		m_nDrawPage;
	u8		m_nVisiblePage;
	// Page PrepareFlip() readied and CommitFlip() will make visible.
	u8		m_nPendingVisible;

	u8		m_nBorder;

	boolean		m_bLogEnabled;
	char		m_LogText[ GPU64_LOG_ROWS ][ GPU64_LOG_COLS ];
	unsigned	m_nLogRow;
	unsigned	m_nLogCol;
};

// gpu64: single instance, owned by CRAD (rad_main.h), reachable from the
// bus-watch loop and the API dispatcher without dragging in rad_main.h.
extern CGpu64FrameBuffer *g_pGpu64FB;

#endif
