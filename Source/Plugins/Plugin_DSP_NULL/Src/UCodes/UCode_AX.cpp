// Copyright (C) 2003-2008 Dolphin Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official SVN repository and contact information can be found at
// http://code.google.com/p/dolphin-emu/

#include "FileUtil.h" // For IsDirectory()
#include "StringUtil.h" // For StringFromFormat()
#include <sstream>

#include "../MailHandler.h"

#include "UCodes.h"
#include "UCode_AXStructs.h"
#include "UCode_AX.h"
#include "UCode_AX_Voice.h"

// ------------------------------------------------------------------
// Externals
// -----------
extern bool gSSBM;
extern bool gSSBMremedy1;
extern bool gSSBMremedy2;
extern bool gSequenced;
extern bool gVolume;
extern bool gReset;
extern std::string gpName;

std::vector<std::string> sMailLog, sMailTime;
// -----------


CUCode_AX::CUCode_AX(CMailHandler& _rMailHandler)
	: IUCode(_rMailHandler)
	, m_addressPBs(0xFFFFFFFF)
{
	// we got loaded
	m_rMailHandler.PushMail(0xDCD10000);
	m_rMailHandler.PushMail(0x80000000);  // handshake ??? only (crc == 0xe2136399) needs it ...

	templbuffer = new int[1024 * 1024];
	temprbuffer = new int[1024 * 1024];
}

CUCode_AX::~CUCode_AX()
{
	m_rMailHandler.Clear();
	delete [] templbuffer;
	delete [] temprbuffer;
}


// ============================================
// Save file to harddrive
// ----------------
void CUCode_AX::SaveLogFile(std::string f, int resizeTo, bool type, bool Wii)
{
	if (gpName.length() > 0) // this is currently off in the Release build
	{
		std::ostringstream ci;
		std::ostringstream cType;
	        
		ci << (resizeTo - 1); // write ci
		cType << type; // write cType

		std::string FileName = FULL_MAIL_LOGS_DIR + gpName;
		FileName += "_sep"; FileName += ci.str(); FileName += "_sep"; FileName += cType.str();
		FileName += Wii ? "_sepWii_sep" : "_sepGC_sep"; FileName += ".log";

		FILE* fhandle = fopen(FileName.c_str(), "w");
		fprintf(fhandle, "%s", f.c_str());
		fflush(fhandle); fhandle = NULL;
	}
}


// ============================================
// Save the logged AX mail
// ----------------
void CUCode_AX::SaveLog_(bool Wii, const char* _fmt, va_list ap)
{
    char Msg[512];
    vsprintf(Msg, _fmt, ap);

    TmpMailLog += Msg;
    TmpMailLog += "\n";

}
// ----------------


// ============================================
// Save the whole AX mail
// ----------------
void CUCode_AX::SaveMail(bool Wii, u32 _uMail)
{
}
// ----------------


int ReadOutPBs(u32 pbs_address, AXParamBlock* _pPBs, int _num)
{
	int count = 0;
	u32 blockAddr = pbs_address;

	// reading and 'halfword' swap
	for (int i = 0; i < _num; i++)
	{
		const short *pSrc = (const short *)g_dspInitialize.pGetMemoryPointer(blockAddr);
		if (pSrc != NULL)
		{
			short *pDest = (short *)&_pPBs[i];
			for (size_t p = 0; p < sizeof(AXParamBlock) / 2; p++)
			{
				pDest[p] = Common::swap16(pSrc[p]);

			}
			blockAddr = (_pPBs[i].next_pb_hi << 16) | _pPBs[i].next_pb_lo;
			count++;	

			// Detect the last mail by checking when next_pb = 0
			u32 next_pb = (Common::swap16(pSrc[0]) << 16) | Common::swap16(pSrc[1]);
			if(next_pb == 0) break;
		}
		else
			break;
	}

	// return the number of read PBs
	return count;
}

void WriteBackPBs(u32 pbs_address, AXParamBlock* _pPBs, int _num)
{
	u32 blockAddr = pbs_address;

	// write back and 'halfword'swap
	for (int i = 0; i < _num; i++)
	{
		short* pSrc  = (short*)&_pPBs[i];
		short* pDest = (short*)g_dspInitialize.pGetMemoryPointer(blockAddr);
		for (size_t p = 0; p < sizeof(AXParamBlock) / 2; p++)
		{
			pDest[p] = Common::swap16(pSrc[p]);
		}

		// next block
		blockAddr = (_pPBs[i].next_pb_hi << 16) | _pPBs[i].next_pb_lo;
	}
}

void CUCode_AX::MixAdd(short* _pBuffer, int _iSize)
{
	AXParamBlock PBs[NUMBER_OF_PBS];

	// read out pbs
	int numberOfPBs = ReadOutPBs(m_addressPBs, PBs, NUMBER_OF_PBS);

	if (_iSize > 1024 * 1024)
		_iSize = 1024 * 1024;

	memset(templbuffer, 0, _iSize * sizeof(int));
	memset(temprbuffer, 0, _iSize * sizeof(int));

	// ---------------------------------------------------------------------------------------
	/* Make the updates we are told to do. When there are multiple updates for a block they
	   are placed in memory directly following updaddr. They are mostly for initial time
	   delays, sometimes for the FIR filter or channel volumes. We do all of them at once here.
	   If we get both an on and an off update we chose on. Perhaps that makes the RE1 music
	   work better. */
	// ------------
	for (int i = 0; i < numberOfPBs; i++)
	{
		u16 *pDest = (u16 *)&PBs[i];
		u16 upd0 = pDest[34]; u16 upd1 = pDest[35]; u16 upd2 = pDest[36]; // num_updates
		u16 upd3 = pDest[37]; u16 upd4 = pDest[38];
		u16 upd_hi = pDest[39]; // update addr
		u16	upd_lo = pDest[40];
		int numupd = upd0 + upd1 + upd2 + upd3 + upd4;
		if(numupd > 64) numupd = 64; // prevent crazy values
		const u32 updaddr   = (u32)(upd_hi << 16) | upd_lo;
		int on = false, off = false;
		for (int j = 0; j < numupd; j++)
		{			
			const u16 updpar   = Memory_Read_U16(updaddr + j);
			const u16 upddata   = Memory_Read_U16(updaddr + j + 2);
			// some safety checks, I hope it's enough
			if(updaddr > 0x80000000 && updaddr < 0x817fffff
				&& updpar < 63 && updpar > 3 && upddata >= 0 // updpar > 3 because we don't want to change
				// 0-3, those are important
				//&& (upd0 || upd1 || upd2 || upd3 || upd4) // We should use these in some way to I think
				// but I don't know how or when
				&& gSequenced) // on and off option
			{
				pDest[updpar] = upddata;
			}
			if (updpar == 7 && upddata == 1) on++;
			if (updpar == 7 && upddata == 1) off++;
		}
		// hack: if we get both an on and an off select on rather than off
		if (on > 0 && off > 0) pDest[7] = 1;
	}

	//PrintFile(1, "%08x %04x %04x\n", updaddr, updpar, upddata);
	// ------------

	for (int i = 0; i < numberOfPBs; i++)
	{
		AXParamBlock& pb = PBs[i];
		MixAddVoice(pb, templbuffer, temprbuffer, _iSize, false);
	}

	// write back out pbs
	WriteBackPBs(m_addressPBs, PBs, numberOfPBs);

	for (int i = 0; i < _iSize; i++)
	{
		// Clamp into 16-bit. Maybe we should add a volume compressor here.
		int left  = templbuffer[i] + _pBuffer[0];
		int right = temprbuffer[i] + _pBuffer[1];
		if (left < -32767)  left = -32767;
		if (left > 32767)   left = 32767;
		if (right < -32767) right = -32767;
		if (right >  32767) right = 32767;
		*_pBuffer++ = left;
		*_pBuffer++ = right;
	}

}


// ------------------------------------------------------------------------------
// Handle incoming mail
// -----------
void CUCode_AX::HandleMail(u32 _uMail)
{
	if ((_uMail & 0xFFFF0000) == MAIL_AX_ALIST)
	{
		// a new List
		DebugLog(" >>>> u32 MAIL : General Mail (%08x)", _uMail);
	}
	else
	{
		DebugLog(" >>>> u32 MAIL : AXTask Mail (%08x)", _uMail);
		AXTask(_uMail);
		
	}
}


// ------------------------------------------------------------------------------
// Update with DSP Interrupt
// -----------
void CUCode_AX::Update()
{
	// check if we have to sent something
	if (!m_rMailHandler.IsEmpty())
	{
		g_dspInitialize.pGenerateDSPInterrupt();
	}
}
// -----------


// Shortcut to avoid having to write SaveLog(false, ...) every time
void CUCode_AX::SaveLog(const char* _fmt, ...)
{
}


// ============================================
// AX seems to bootup one task only and waits for resume-callbacks
// everytime the DSP has "spare time" it sends a resume-mail to the CPU
// and the __DSPHandler calls a AX-Callback which generates a new AXFrame
bool CUCode_AX::AXTask(u32& _uMail)
{
	u32 uAddress = _uMail;
	SaveLog("Begin");
	SaveLog("=====================================================================");
	SaveLog("%08x : AXTask - AXCommandList-Addr:", uAddress);

	u32 Addr__AXStudio;
	u32 Addr__AXOutSBuffer;
	u32 Addr__AXOutSBuffer_1;
	u32 Addr__AXOutSBuffer_2;
	u32 Addr__A;
	u32 Addr__12;
	u32 Addr__4_1;
	u32 Addr__4_2;
        //	u32 Addr__4_3;
        //	u32 Addr__4_4;
	u32 Addr__5_1;
	u32 Addr__5_2;
	u32 Addr__6;
	u32 Addr__9;

	bool bExecuteList = true;

	while (bExecuteList)
	{
		static int last_valid_command = 0;
		u16 iCommand = Memory_Read_U16(uAddress);
		uAddress += 2;
		switch (iCommand)
		{
	    case AXLIST_STUDIOADDR: //00
		    Addr__AXStudio = Memory_Read_U32(uAddress);
		    uAddress += 4;
		    SaveLog("%08x : AXLIST studio address: %08x", uAddress, Addr__AXStudio);
		    break;

	    case 0x001: // 2byte x 10
	    {
		    u32 address = Memory_Read_U32(uAddress);
		    uAddress += 4;
		    u16 param1 = Memory_Read_U16(uAddress);
		    uAddress += 2;
		    u16 param2 = Memory_Read_U16(uAddress);
		    uAddress += 2;
		    u16 param3 = Memory_Read_U16(uAddress);
		    uAddress += 2;
		    SaveLog("%08x : AXLIST 1: %08x, %04x, %04x, %04x", uAddress, address, param1, param2, param3);
	    }
		    break;

	    //
	    // Somewhere we should be getting a bitmask of AX_SYNC values
	    // that tells us what has been updated
	    // Dunno if important
	    //
	    case AXLIST_PBADDR: //02
		    {
		    m_addressPBs = Memory_Read_U32(uAddress);
		    uAddress += 4;

		    }
		    break;

	    case 0x0003:
		    SaveLog("%08x : AXLIST command 0x0003 ????");
		    break;

	    case 0x0004:  // AUX?
		    Addr__4_1 = Memory_Read_U32(uAddress);
		    uAddress += 4;
		    Addr__4_2 = Memory_Read_U32(uAddress);
		    uAddress += 4;
		    SaveLog("%08x : AXLIST 4_1 4_2 addresses: %08x %08x", uAddress, Addr__4_1, Addr__4_2);
		    break;

	    case 0x0005:
		    Addr__5_1 = Memory_Read_U32(uAddress);
		    uAddress += 4;
		    Addr__5_2 = Memory_Read_U32(uAddress);
		    uAddress += 4;
		    SaveLog("%08x : AXLIST 5_1 5_2 addresses: %08x %08x", uAddress, Addr__5_1, Addr__5_2);
		    break;

	    case 0x0006:
		    Addr__6   = Memory_Read_U32(uAddress);
		    uAddress += 4;
		    SaveLog("%08x : AXLIST 6 address: %08x", uAddress, Addr__6);
		    break;

	    case AXLIST_SBUFFER:
		    Addr__AXOutSBuffer = Memory_Read_U32(uAddress);
		    uAddress += 4;
		    SaveLog("%08x : AXLIST OutSBuffer address: %08x", uAddress, Addr__AXOutSBuffer);
		    break;

	    case 0x0009:
		    Addr__9   = Memory_Read_U32(uAddress);
		    uAddress += 4;
		    SaveLog("%08x : AXLIST 6 address: %08x", Addr__9);
		    break;

	    case AXLIST_COMPRESSORTABLE:  // 0xa
		    Addr__A   = Memory_Read_U32(uAddress);
		    uAddress += 4;
		    SaveLog("%08x : AXLIST CompressorTable address: %08x", uAddress, Addr__A);
		    break;

	    case 0x000e:
		    Addr__AXOutSBuffer_1 = Memory_Read_U32(uAddress);
		    uAddress += 4;

			// Addr__AXOutSBuffer_2 is the address in RAM that we are supposed to mix to.
			// Although we don't, currently.
		    Addr__AXOutSBuffer_2 = Memory_Read_U32(uAddress);
		    uAddress += 4;
		    SaveLog("%08x : AXLIST sbuf2 addresses: %08x %08x", uAddress, Addr__AXOutSBuffer_1, Addr__AXOutSBuffer_2);
		    break;

	    case AXLIST_END:
		    bExecuteList = false;
		    SaveLog("%08x : AXLIST end", uAddress);
		    break;

	    case 0x0010:  //Super Monkey Ball 2
		    SaveLog("%08x : AXLIST 0x0010", uAddress);
		    //should probably read/skip stuff here
		    uAddress += 8;
		    break;

	    case 0x0011:
		    uAddress += 4;
		    break;

	    case 0x0012:
		    Addr__12  = Memory_Read_U16(uAddress);
		    uAddress += 2;
		    break;

	    case 0x0013:
		    uAddress += 6 * 4; // 6 Addresses.
		    break;

		default:
			{
		    static bool bFirst = true;
		    if (bFirst == true)
		    {
			    char szTemp[2048];
				sprintf(szTemp, "Unknown AX-Command 0x%x (address: 0x%08x). Last valid: %02x\n",
					    iCommand, uAddress - 2, last_valid_command);
			    int num = -32;
			    while (num < 64+32)
			    {
				    char szTemp2[128] = "";
					sprintf(szTemp2, "%s0x%04x\n", num == 0 ? ">>" : "  ", Memory_Read_U16(uAddress + num));
				    strcat(szTemp, szTemp2);
				    num += 2;
			    }

				// Wii AX will always show this
			    PanicAlert(szTemp);
			   // bFirst = false;
		    }

		    // unknown command so stop the execution of this TaskList
		    bExecuteList = false;
			}
			break;
		}
		if (bExecuteList)
			last_valid_command = iCommand;
	}
	SaveLog("AXTask - done, send resume");
	SaveLog("=====================================================================");
	SaveLog("End");

	// i hope resume is okay AX
	m_rMailHandler.PushMail(0xDCD10001);
	return true;
}
