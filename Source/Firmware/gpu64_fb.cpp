/*
 gpu64: 320x200x8 framebuffer implementation -- see gpu64_fb.h for why this
 owns the display rather than sharing CScreenDevice's.
*/
#include "gpu64_fb.h"
#include "gpu64_font8x8.h"
#include "gpu64_flip.h"
#include <circle/synchronize.h>
#include <circle/util.h>

CGpu64FrameBuffer *g_pGpu64FB = 0;

// gpu64: SetPalette32() takes 0xAABBGGRR -- red in the LOW byte, not the
// high one. Getting this backwards is not subtle on screen but is easy to
// misread as a palette-index bug: the first hardware test showed the C64's
// blue background (53,40,121) as red, because the blue channel was landing
// in the red slot. Alpha is 0xff throughout; the VideoCore ignores it at
// depth 8, but 0 would be the wrong thing to store if it ever stopped.
static inline u32 PackRGB( u8 r, u8 g, u8 b )
{
	return (u32)( 0xFF000000 | ( (u32)b << 16 ) | ( (u32)g << 8 ) | r );
}

// gpu64: standard C64 16-colour palette (Pepto's values), loaded into palette
// entries 0-15 at reset so the mirror -- and any C64 program that thinks in
// C64 colour numbers -- gets the expected colours for free. Full 8 bits per
// channel here: unlike the old COLOR16 path, the hardware palette takes
// 24-bit RGB, so nothing is truncated.
static const u8 c64Palette[ 16 ][ 3 ] = {
	{   0,   0,   0 },		// 0 black
	{ 255, 255, 255 },		// 1 white
	{ 104,  55,  43 },		// 2 red
	{ 112, 164, 178 },		// 3 cyan
	{ 111,  61, 134 },		// 4 purple
	{  88, 141,  67 },		// 5 green
	{  53,  40, 121 },		// 6 blue
	{ 184, 199, 111 },		// 7 yellow
	{ 111,  79,  37 },		// 8 orange
	{  67,  57,   0 },		// 9 brown
	{ 154, 103,  89 },		// 10 light red
	{  68,  68,  68 },		// 11 dark grey
	{ 108, 108, 108 },		// 12 grey
	{ 154, 210, 132 },		// 13 light green
	{ 108,  94, 181 },		// 14 light blue
	{ 149, 149, 149 },		// 15 light grey
};

CGpu64FrameBuffer::CGpu64FrameBuffer( void )
:	m_pFB( 0 ),
	m_pBuffer( 0 ),
	m_nPitch( 0 ),
	m_bInitialized( FALSE ),
	m_nDrawPage( 0 ),
	m_nVisiblePage( 0 ),
	m_nPendingVisible( 0 ),
	m_nBorder( 0 ),
	m_bLogEnabled( TRUE ),
	m_nLogRow( 0 ),
	m_nLogCol( 0 )
{
	for ( unsigned r = 0; r < GPU64_LOG_ROWS; r++ )
		for ( unsigned c = 0; c < GPU64_LOG_COLS; c++ )
			m_LogText[ r ][ c ] = ' ';

	g_pGpu64FB = this;
}

CGpu64FrameBuffer::~CGpu64FrameBuffer( void )
{
	delete m_pFB;
	m_pFB = 0;
	g_pGpu64FB = 0;
}

boolean CGpu64FrameBuffer::Initialize( void )
{
	// Virtual height is twice the physical height: page 1 lives directly
	// below page 0 in the same allocation, and SetVirtualOffset() picks
	// which one the scanout starts at. That is the hardware page flip.
	m_pFB = new CBcmFrameBuffer( GPU64_FB_TOTAL_W, GPU64_FB_TOTAL_H, 8,
				     GPU64_FB_TOTAL_W, GPU64_FB_TOTAL_H * GPU64_FB_PAGES,
				     0, FALSE );
	if ( m_pFB == 0 )
		return FALSE;

	for ( unsigned i = 0; i < 16; i++ )
		SetPaletteEntry( (u8)i, c64Palette[ i ][ 0 ], c64Palette[ i ][ 1 ], c64Palette[ i ][ 2 ] );
	for ( unsigned i = 16; i < 255; i++ )
		SetPaletteEntry( (u8)i, 0, 0, 0 );
	SetPaletteEntry( GPU64_LOG_INK, 255, 255, 255 );

	if ( !m_pFB->Initialize() )
		return FALSE;

	if ( m_pFB->GetDepth() != 8 )
		return FALSE;

	m_pBuffer = (u8 *)(uintptr)m_pFB->GetBuffer();
	m_nPitch  = m_pFB->GetPitch();
	if ( m_pBuffer == 0 || m_nPitch < GPU64_FB_TOTAL_W )
		return FALSE;

	m_bInitialized = TRUE;

	// Contents of a fresh allocation are undefined; the API says so too, but
	// a screenful of noise at boot looks like a fault, so start both pages
	// black.
	for ( unsigned p = 0; p < GPU64_FB_PAGES; p++ )
	{
		memset( PageBase( p ), 0, m_nPitch * GPU64_FB_TOTAL_H );
		CleanPage( p );
	}

	m_pFB->SetVirtualOffset( 0, 0 );

	// Not fatal if it fails: CommitFlip() just keeps using the blocking
	// mailbox call the way milestone 4b shipped it.
	gpu64_flipInit();

	return TRUE;
}

u8 *CGpu64FrameBuffer::PageBase( unsigned nPage )
{
	if ( !m_bInitialized || nPage >= GPU64_FB_PAGES )
		return 0;
	return m_pBuffer + (size_t)nPage * GPU64_FB_TOTAL_H * m_nPitch;
}

u8 *CGpu64FrameBuffer::PageBuffer( unsigned nPage )
{
	u8 *p = PageBase( nPage );
	if ( p == 0 )
		return 0;
	return p + (size_t)GPU64_BORDER_H * m_nPitch + GPU64_BORDER_W;
}

void CGpu64FrameBuffer::CleanRows( unsigned nPage, unsigned y0, unsigned y1 )
{
	if ( !m_bInitialized || nPage >= GPU64_FB_PAGES )
		return;
	if ( y1 > GPU64_FB_HEIGHT ) y1 = GPU64_FB_HEIGHT;
	if ( y0 >= y1 )
		return;

	u8 *p = PageBase( nPage ) + (size_t)( GPU64_BORDER_H + y0 ) * m_nPitch;
	CleanDataCacheRange( (u64)(uintptr)p, (size_t)( y1 - y0 ) * m_nPitch );
}

void CGpu64FrameBuffer::CleanPage( unsigned nPage )
{
	if ( !m_bInitialized || nPage >= GPU64_FB_PAGES )
		return;
	// Whole physical page, border bands included -- this is also what
	// SetBorder() relies on.
	CleanDataCacheRange( (u64)(uintptr)PageBase( nPage ),
			     (size_t)GPU64_FB_TOTAL_H * m_nPitch );
}

void CGpu64FrameBuffer::SetBorder( u8 nColor )
{
	if ( !m_bInitialized )
		return;

	m_nBorder = nColor;

	for ( unsigned n = 0; n < GPU64_FB_PAGES; n++ )
	{
		u8 *p = PageBase( n );
		if ( p == 0 )
			continue;

		// Top and bottom bands: full physical rows.
		memset( p, nColor, (size_t)GPU64_BORDER_H * m_nPitch );
		memset( p + (size_t)( GPU64_BORDER_H + GPU64_FB_HEIGHT ) * m_nPitch,
			nColor, (size_t)GPU64_BORDER_H * m_nPitch );

		// Left and right margins of every row the drawing surface spans.
		u8 *pRow = p + (size_t)GPU64_BORDER_H * m_nPitch;
		for ( unsigned y = 0; y < GPU64_FB_HEIGHT; y++, pRow += m_nPitch )
		{
			memset( pRow, nColor, GPU64_BORDER_W );
			memset( pRow + GPU64_BORDER_W + GPU64_FB_WIDTH, nColor,
				m_nPitch - GPU64_BORDER_W - GPU64_FB_WIDTH );
		}

		CleanPage( n );
	}
}

void CGpu64FrameBuffer::SetDrawPage( u8 nPage )
{
	if ( nPage < GPU64_FB_PAGES )
		m_nDrawPage = nPage;
}

void CGpu64FrameBuffer::PrepareFlip( void )
{
	if ( !m_bInitialized )
		return;

	m_nPendingVisible = m_nDrawPage;

	// The log overlay is baked into page pixels, so it has to be re-applied
	// to whichever page is about to be shown -- otherwise a program that
	// draws its frame into the back page would flip the log away.
	//
	// This is the whole cost of a flip apart from the mailbox call, and it
	// deliberately happens here rather than in CommitFlip(): a deferred flip
	// commits from inside the bus-watch loop, and the less that path does
	// while holding the C64 halted, the better.
	DrawLogOverlay( m_nPendingVisible );
	CleanPage( m_nPendingVisible );
}

boolean CGpu64FrameBuffer::CommitFlip( void )
{
	if ( !m_bInitialized )
		return FALSE;

	u32 nOffsetY = m_nPendingVisible * GPU64_FB_TOTAL_H;

	// The fast path posts the request and returns without waiting for the
	// VideoCore -- see gpu64_flip.h. It is the whole reason this function is
	// safe to call from inside the DMA hold at a frame boundary. If it is
	// unavailable (init failed, or a drain timed out and disarmed it) fall
	// back to Circle's blocking call, which is exactly how milestone 4b
	// shipped: correct, and slow enough to notice.
	if ( !gpu64_flipPost( nOffsetY ) )
	{
		gpu64FlipStats.slowCount++;
		if ( !m_pFB->SetVirtualOffset( 0, nOffsetY ) )
			return FALSE;
	}

	m_nDrawPage = m_nVisiblePage;
	m_nVisiblePage = m_nPendingVisible;
	return TRUE;
}

boolean CGpu64FrameBuffer::Flip( void )
{
	if ( !m_bInitialized )
		return FALSE;

	PrepareFlip();
	return CommitFlip();
}

boolean CGpu64FrameBuffer::WaitForVSync( void )
{
	if ( !m_bInitialized )
		return FALSE;
	gpu64_flipDrain();				// as in ResetPages(): Circle's mailbox, not ours
	return m_pFB->WaitForVerticalSync();
}

void CGpu64FrameBuffer::ResetPages( void )
{
	if ( !m_bInitialized )
		return;
	m_nDrawPage = m_nVisiblePage = m_nPendingVisible = 0;
	// Circle's mailbox call below would otherwise swallow an in-flight
	// flip's reply -- and pay 20 ms for the privilege, per CBcmMailBox::Flush().
	gpu64_flipDrain();
	m_pFB->SetVirtualOffset( 0, 0 );
}

void CGpu64FrameBuffer::Clear( u8 nColor )
{
	u8 *p = PageBuffer( m_nDrawPage );
	if ( p == 0 )
		return;

	// Row by row rather than one memset: pitch can exceed 320, and the bytes
	// past the visible width belong to nobody.
	for ( unsigned y = 0; y < GPU64_FB_HEIGHT; y++ )
		memset( p + (size_t)y * m_nPitch, nColor, GPU64_FB_WIDTH );

	CleanPage( m_nDrawPage );
}

void CGpu64FrameBuffer::SetPixel( int x, int y, u8 nColor )
{
	if ( x < 0 || y < 0 || x >= GPU64_FB_WIDTH || y >= GPU64_FB_HEIGHT )
		return;
	u8 *p = PageBuffer( m_nDrawPage );
	if ( p == 0 )
		return;
	p[ (size_t)y * m_nPitch + x ] = nColor;
	CleanRows( m_nDrawPage, y, y + 1 );
}

void CGpu64FrameBuffer::Line( int x0, int y0, int x1, int y1, u8 nColor )
{
	u8 *p = PageBuffer( m_nDrawPage );
	if ( p == 0 )
		return;

	int dx = x1 - x0; if ( dx < 0 ) dx = -dx;
	int dy = y1 - y0; if ( dy < 0 ) dy = -dy;
	int sx = ( x0 < x1 ) ? 1 : -1;
	int sy = ( y0 < y1 ) ? 1 : -1;
	int err = dx - dy;

	int minY = ( y0 < y1 ) ? y0 : y1;
	int maxY = ( y0 < y1 ) ? y1 : y0;

	// Bresenham with per-pixel clipping. Clipping the endpoints up front
	// would be faster, but a line's cost is bounded by its length either
	// way, and this cannot get the slope subtly wrong.
	while ( 1 )
	{
		if ( x0 >= 0 && y0 >= 0 && x0 < GPU64_FB_WIDTH && y0 < GPU64_FB_HEIGHT )
			p[ (size_t)y0 * m_nPitch + x0 ] = nColor;

		if ( x0 == x1 && y0 == y1 )
			break;

		int e2 = err * 2;
		if ( e2 > -dy ) { err -= dy; x0 += sx; }
		if ( e2 <  dx ) { err += dx; y0 += sy; }
	}

	if ( minY < 0 ) minY = 0;
	CleanRows( m_nDrawPage, (unsigned)minY, (unsigned)( maxY + 1 ) );
}

void CGpu64FrameBuffer::RectFill( int x, int y, int w, int h, u8 nColor )
{
	if ( w <= 0 || h <= 0 )
		return;
	u8 *p = PageBuffer( m_nDrawPage );
	if ( p == 0 )
		return;

	int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
	if ( x0 < 0 ) x0 = 0;
	if ( y0 < 0 ) y0 = 0;
	if ( x1 > GPU64_FB_WIDTH ) x1 = GPU64_FB_WIDTH;
	if ( y1 > GPU64_FB_HEIGHT ) y1 = GPU64_FB_HEIGHT;
	if ( x0 >= x1 || y0 >= y1 )
		return;

	for ( int yy = y0; yy < y1; yy++ )
		memset( p + (size_t)yy * m_nPitch + x0, nColor, (size_t)( x1 - x0 ) );

	CleanRows( m_nDrawPage, (unsigned)y0, (unsigned)y1 );
}

void CGpu64FrameBuffer::Rect( int x, int y, int w, int h, u8 nColor )
{
	if ( w <= 0 || h <= 0 )
		return;

	// Four one-pixel-thick fills; each clips itself, so an outline partly off
	// the page keeps the edges that are still on it.
	RectFill( x, y, w, 1, nColor );
	RectFill( x, y + h - 1, w, 1, nColor );
	if ( h > 2 )
	{
		RectFill( x, y + 1, 1, h - 2, nColor );
		RectFill( x + w - 1, y + 1, 1, h - 2, nColor );
	}
}

void CGpu64FrameBuffer::Blit( const u8 *pSrc, int x, int y, unsigned w, unsigned h, int nKey )
{
	if ( pSrc == 0 || w == 0 || h == 0 )
		return;
	u8 *p = PageBuffer( m_nDrawPage );
	if ( p == 0 )
		return;

	for ( unsigned sy = 0; sy < h; sy++ )
	{
		int dy = y + (int)sy;
		if ( dy < 0 || dy >= GPU64_FB_HEIGHT )
			continue;

		const u8 *pRow = pSrc + (size_t)sy * w;
		u8 *pDst = p + (size_t)dy * m_nPitch;

		for ( unsigned sx = 0; sx < w; sx++ )
		{
			int dx = x + (int)sx;
			if ( dx < 0 || dx >= GPU64_FB_WIDTH )
				continue;
			u8 v = pRow[ sx ];
			if ( nKey >= 0 && v == (u8)nKey )
				continue;
			pDst[ dx ] = v;
		}
	}

	int y0 = y < 0 ? 0 : y;
	CleanRows( m_nDrawPage, (unsigned)y0, (unsigned)( y + (int)h ) );
}

void CGpu64FrameBuffer::ReadRect( u8 *pDst, unsigned x, unsigned y, unsigned w, unsigned h )
{
	u8 *p = PageBuffer( m_nDrawPage );
	if ( p == 0 || pDst == 0 )
		return;

	for ( unsigned sy = 0; sy < h; sy++ )
		memcpy( pDst + (size_t)sy * w, p + (size_t)( y + sy ) * m_nPitch + x, w );
}

void CGpu64FrameBuffer::SetPaletteEntry( u8 nIndex, u8 r, u8 g, u8 b )
{
	// The shadow is updated even when there is no framebuffer, so
	// BUILD_COLORMAP sees the same palette a display would have shown.
	m_Palette[ nIndex * 3 + 0 ] = r;
	m_Palette[ nIndex * 3 + 1 ] = g;
	m_Palette[ nIndex * 3 + 2 ] = b;

	if ( m_pFB == 0 )
		return;
	m_pFB->SetPalette32( nIndex, PackRGB( r, g, b ) );
}

boolean CGpu64FrameBuffer::CommitPalette( void )
{
	if ( m_pFB == 0 )
		return FALSE;
	gpu64_flipDrain();				// as in ResetPages(): Circle's mailbox, not ours
	return m_pFB->UpdatePalette();
}

void CGpu64FrameBuffer::LogEnable( boolean bEnable )
{
	if ( m_bLogEnabled == bEnable )
		return;

	m_bLogEnabled = bEnable;

	// Turning the log off can't unpaint pixels that are already baked into
	// the page -- but the next flip repaints the page being shown, and in the
	// common case (no paging) the program is about to redraw anyway.
	if ( bEnable )
	{
		DrawLogOverlay( m_nVisiblePage );
		CleanPage( m_nVisiblePage );
	}
}

void CGpu64FrameBuffer::LogChar( char c )
{
	if ( c == '\r' )
		return;

	if ( c == '\n' || m_nLogCol >= GPU64_LOG_COLS )
	{
		m_nLogCol = 0;

		if ( m_nLogRow + 1 < GPU64_LOG_ROWS )
			m_nLogRow++;
		else
		{
			// Scroll, rather than wrap. This used to be a ring: the write
			// point moved back to row 0 and the line about to be written was
			// blanked, so the wrap was at least visible as a gap. On a real
			// display that reads as broken -- the newest line sits at the
			// top with older lines below it, and there is no cue that the
			// bottom of the screen is the *oldest* text rather than the end
			// of the boot sequence. A terminal scroll is what everyone
			// expects, and it costs one memmove of a 1000-byte array per
			// line, on a path that already repaints the whole overlay.
			memmove( &m_LogText[ 0 ][ 0 ], &m_LogText[ 1 ][ 0 ],
				 (size_t)( GPU64_LOG_ROWS - 1 ) * GPU64_LOG_COLS );
		}

		for ( unsigned i = 0; i < GPU64_LOG_COLS; i++ )
			m_LogText[ m_nLogRow ][ i ] = ' ';

		if ( c == '\n' )
			return;
	}

	m_LogText[ m_nLogRow ][ m_nLogCol++ ] = c;
}

void CGpu64FrameBuffer::LogWrite( const char *pString, unsigned nLength )
{
	for ( unsigned i = 0; i < nLength; i++ )
		LogChar( pString[ i ] );

	if ( !m_bInitialized || !m_bLogEnabled )
		return;

	DrawLogOverlay( m_nVisiblePage );
	CleanPage( m_nVisiblePage );
}

void CGpu64FrameBuffer::DrawLogOverlay( unsigned nPage )
{
	if ( !m_bInitialized || !m_bLogEnabled )
		return;

	u8 *p = PageBuffer( nPage );
	if ( p == 0 )
		return;

	// Each row that holds any text is painted as an opaque band: the whole
	// 320-pixel line is cleared first, then the glyphs go on top.
	//
	// Drawing glyph pixels alone -- which is what this did -- leaves no way
	// to erase. Nothing here knows what was underneath a pixel it lit, so a
	// line that scrolls up leaves its old glyphs behind and the log reads as
	// doubled text. Clearing whole rows is the only erase available without
	// keeping a shadow copy of the page.
	//
	// Rows with no text are left alone, so an overlay that only fills part
	// of the screen still lets the rest show through -- which is what the
	// milestone 3 mirror wants. Nothing is reserved for the background: it
	// clears to palette index 0, the same entry a program is free to
	// repaint, exactly as GPU64_LOG_INK is.
	for ( unsigned row = 0; row < GPU64_LOG_ROWS; row++ )
	{
		unsigned col;
		for ( col = 0; col < GPU64_LOG_COLS; col++ )
			if ( m_LogText[ row ][ col ] != ' ' && m_LogText[ row ][ col ] != 0 )
				break;
		if ( col == GPU64_LOG_COLS )
			continue;

		for ( unsigned gy = 0; gy < GPU64_LOG_CHAR_H; gy++ )
			memset( p + (size_t)( row * GPU64_LOG_CHAR_H + gy ) * m_nPitch, 0, GPU64_FB_WIDTH );

		for ( col = 0; col < GPU64_LOG_COLS; col++ )
		{
			char c = m_LogText[ row ][ col ];
			if ( c == ' ' || c == 0 )
				continue;

			const u8 *pGlyph = gpu64Font8x8[ (u8)c ];

			for ( unsigned gy = 0; gy < GPU64_LOG_CHAR_H; gy++ )
			{
				u8 bits = pGlyph[ gy ];
				if ( bits == 0 )
					continue;

				u8 *pDst = p + (size_t)( row * GPU64_LOG_CHAR_H + gy ) * m_nPitch
					     + col * GPU64_LOG_CHAR_W;
				for ( unsigned gx = 0; gx < GPU64_LOG_CHAR_W; gx++ )
					if ( bits & ( 0x80 >> gx ) )
						pDst[ gx ] = GPU64_LOG_INK;
			}
		}
	}
}

// gpu64: free-function indirection for the bus-watch loop, the same pattern
// gpu64_showTestPattern()/gpu64_showMirror() use in rad_main.cpp -- it lets
// rad_reu.cpp commit a deferred page flip without including this header and
// the Circle framebuffer stack behind it.
boolean gpu64_commitFlip( void )
{
	if ( g_pGpu64FB == 0 )
		return FALSE;

	// gpu64: this is the measurement point for the flip's cost, not
	// gpu64_flipPost() -- what matters is the whole chain, cold instruction
	// cache included, because that is exactly how long the frame boundary
	// holds the C64 halted. Two system-timer reads, on flips only.
	//
	// Only *deferred* flips are counted: an immediate PAGE_FLIP calls
	// CommitFlip() directly and never comes through here. That is
	// deliberate -- the cost this module exists to remove is the one paid
	// inside the bus-watch loop's hold, and only the deferred flip pays it.
	u32 t0 = read32( ARM_SYSTIMER_CLO );
	boolean bOK = g_pGpu64FB->CommitFlip();
	u32 dt = read32( ARM_SYSTIMER_CLO ) - t0;

	gpu64FlipStats.postCount++;
	gpu64FlipStats.postTotalUs += dt;
	if ( dt < gpu64FlipStats.postMinUs ) gpu64FlipStats.postMinUs = dt;
	if ( dt > gpu64FlipStats.postMaxUs ) gpu64FlipStats.postMaxUs = dt;

	return bOK;
}
