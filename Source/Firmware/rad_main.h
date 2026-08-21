/*

  {_______            {_          {______
        {__          {_ __               {__
        {__         {_  {__               {__
     {__           {__   {__               {__
 {______          {__     {__              {__
       {__       {__       {__            {__   
         {_________         {______________		Expansion Unit
                
 RADExp - A framework for DMA interfacing with Commodore C64/C128 computers using a Raspberry Pi Zero 2 or 3A+/3B+
 Copyright (c) 2022 Carsten Dachsbacher <frenetic@dachsbacher.de>

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/
#ifndef _RAD_H
#define _RAD_H

#include <circle/startup.h>
#include <circle/bcm2835.h>
#include <circle/memio.h>
#include <circle/memory.h>
#include <circle/koptions.h>
#include <circle/devicenameservice.h>
#include <circle/screen.h>
#include <circle/serial.h>
#include "tee_device.h"
#include <circle/actled.h>
#include <circle/interrupt.h>
#include <circle/timer.h>
#include <circle/logger.h>
#include <circle/sched/scheduler.h>
#include <circle/types.h>
#include <circle/gpioclock.h>
#include <circle/gpiopin.h>
#include <circle/gpiopinfiq.h>
#include <circle/gpiomanager.h>
#include <circle/util.h>

#include "lowlevel_arm64.h"
#include "gpio_defs.h"
#include "helpers.h"

CLogger	*logger;

class CRAD
{
public:
	// gpu64: m_CPUThrottle (CCPUThrottle) removed entirely -- its constructor
	// unconditionally does mailbox property-tag round trips
	// (GetClockRate/GetTemperature) and ends by calling SetSpeed() itself. On
	// this board (RPi 3 Model A+, this exact GPU firmware) that mailbox
	// exchange hangs forever, before ANY of our code -- including the
	// earliest possible serial log line -- can run. Proven by reproducing the
	// identical silent hang in an otherwise fully-working stock Circle sample
	// just by adding this same member+call (mailbox calls for the
	// framebuffer, by contrast, work fine on this board -- it's specific to
	// these particular property tags). CPU speed is left at whatever the GPU
	// firmware defaults to for now; revisit once cycle-accurate bus timing
	// needs to be verified for real.
	CRAD( void )
		: m_Screen( m_Options.GetWidth(), m_Options.GetHeight() ),
		m_HDMIConsole( &m_Screen ),
		m_TeeLog( &m_Serial, &m_HDMIConsole ),
		m_Timer( &m_Interrupt ),
		m_Logger( 5/*m_Options.GetLogLevel()*/, &m_Timer ),
		m_EMMC( &m_Interrupt, &m_Timer, 0 )
	{
	}

	~CRAD( void )
	{
	}

	boolean Initialize( void )
	{
		#ifndef COMPILE_MENU
		logger = &m_Logger;
		#endif
		STANDARD_SETUP_TIMER_INTERRUPT_CYCLECOUNTER_GPIO
		return bOK;
	}

	void Run( void );

private:
	static void FIQHandler( void *pParam );

	CMemorySystem		m_Memory;
	CKernelOptions		m_Options;
	CDeviceNameService	m_DeviceNameService;
	CScreenDevice		m_Screen;
	// gpu64: Tier 1 bring-up -- CRAD previously never instantiated a serial
	// device at all, so CLogger's default log target ("tty1", i.e. m_Screen)
	// meant every logger->Write() only ever reached the HDMI screen, never
	// GPIO14/15, regardless of anything wired to the UART pins. Default
	// (polling, no interrupt system) so it can be brought up before
	// m_Interrupt.Initialize() and survives even if interrupts never work.
	CSerialDevice		m_Serial;
	// gpu64: renders logger text directly via SetPixel(), bypassing
	// CScreenDevice's own text console -- required because that console
	// (like m_Serial) silently drops writes once IRQs are disabled, which is
	// true for essentially all of RAD's runtime after boot. See tee_device.h.
	CHDMIConsole		m_HDMIConsole;
	// gpu64: fans logger->Write() out to both m_Serial (Tier 1, unchanged)
	// and m_HDMIConsole (Tier 2 -- visible even once gpioInit() steals the
	// UART pins for cartridge use, and even with IRQs disabled).
	CTeeDevice			m_TeeLog;
	CInterruptSystem	m_Interrupt;
	CTimer				m_Timer;
	CLogger				m_Logger;
	CScheduler			m_Scheduler;
	CEMMCDevice			m_EMMC;

	// gpu64: draws directly into m_Screen's already-initialized framebuffer,
	// independent of the C64 bus hijack -- milestone 2 "display basic
	// pattern" first cut. (A separate CBcmFrameBuffer at a custom 320x200
	// resolution was tried first and produced nothing visible -- likely
	// fighting m_Screen's own already-negotiated HDMI mode; reusing the
	// framebuffer Circle already proved works avoids that entirely.)
	void showTestPattern( void );
};

#endif
