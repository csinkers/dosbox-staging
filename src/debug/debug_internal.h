#pragma once
#include "cpu.h"
#include "paging.h"

uint32_t PhysMakeProt(uint16_t selector, uint32_t offset);
uint32_t GetAddress(uint16_t seg, uint32_t offset);
int32_t DEBUG_Run(int32_t amount, bool quickexit);
Bitu DEBUG_Loop(void);

enum EBreakpoint {
	BKPNT_UNKNOWN,
	BKPNT_PHYSICAL,
	BKPNT_INTERRUPT,
	BKPNT_MEMORY,
	BKPNT_MEMORY_READ,
	BKPNT_MEMORY_PROT,
	BKPNT_MEMORY_LINEAR
};

#define BPINT_ALL 0x100

class CBreakpoint
{
public:

	CBreakpoint(void);
	void SetAddress (uint16_t seg, uint32_t off)               { location = GetAddress(seg,off); type = BKPNT_PHYSICAL; segment = seg; offset = off; }
	void SetAddress (PhysPt adr)                               { location = adr; type = BKPNT_PHYSICAL; }
	void SetInt     (uint8_t _intNr, uint16_t ah, uint16_t al) { intNr = _intNr, ahValue = ah; alValue = al; type = BKPNT_INTERRUPT; }

	void SetOnce(bool _once) { once = _once; }
	void SetType(EBreakpoint _type) { type = _type; }
	void SetValue(uint8_t value) { ahValue = value; }
	void SetOther(uint8_t other) { alValue = other; }

	void ActivateInner(bool _active);
	void Activate(bool _active);
	void Enable(bool _enabled);

	int GetId()           const noexcept { return id; }
	EBreakpoint GetType() const noexcept { return type; }
	bool IsActive()       const noexcept { return active; }
	bool IsEnabled()      const noexcept { return enabled; }
	bool GetOnce()        const noexcept { return once; }
	PhysPt GetLocation()  const noexcept { return location; }
	uint16_t GetSegment() const noexcept { return segment; }
	uint32_t GetOffset()  const noexcept { return offset; }
	uint8_t GetIntNr()    const noexcept { return intNr; }
	uint16_t GetValue()   const noexcept { return ahValue; }
	uint16_t GetOther()   const noexcept { return alValue; }
#if C_HEAVY_DEBUG
	void FlagMemoryAsRead()
	{
		memory_was_read = true;
	}

	void FlagMemoryAsUnread()
	{
		memory_was_read = false;
	}

	bool WasMemoryRead() const
	{
		return memory_was_read;
	}
#endif

	// statics
	static CBreakpoint* AddBreakpoint(uint16_t seg, uint32_t off, bool once);
	static CBreakpoint* AddIntBreakpoint(uint8_t intNum, uint16_t ah, uint16_t al, bool once);
	static CBreakpoint* AddMemBreakpoint(uint16_t seg, uint32_t off);
	static void DeactivateBreakpoints();
	static void ActivateBreakpoints();
	static void ActivateBreakpointsExceptAt(PhysPt adr);
	static void EnableBreakpoint(int id, bool enable);
	static bool CheckBreakpoint(PhysPt adr);
	static bool CheckBreakpoint(Bitu seg, Bitu off);
	static bool CheckIntBreakpoint(PhysPt adr, uint8_t intNr, uint16_t ahValue, uint16_t alValue);
	static CBreakpoint* FindPhysBreakpoint(uint16_t seg, uint32_t off, bool once);
	static CBreakpoint* FindOtherActiveBreakpoint(PhysPt adr, CBreakpoint* skip);
	static bool IsBreakpoint(uint16_t seg, uint32_t off);
	static bool DeleteBreakpoint(int id);
	static bool DeleteBreakpoint(uint16_t seg, uint32_t off);
	static bool DeleteByIndex(uint16_t index);
	static void DeleteAll(void);
	static void ShowList(void);
	static std::list<CBreakpoint*>::const_iterator begin();
	static std::list<CBreakpoint*>::const_iterator end();

private:
	// Shared
	int id = -2;
	EBreakpoint type = {};
	bool active = false;
	bool enabled = true;
	bool once   = false;

	// Physical
	PhysPt location  = 0;
	uint8_t oldData  = 0xCC;
	uint16_t segment = 0;
	uint32_t offset  = 0;

	// Interrupt
	uint8_t intNr    = 0;
	uint16_t ahValue = 0;
	uint16_t alValue = 0;

#if C_HEAVY_DEBUG
	bool memory_was_read = false;

	friend bool DEBUG_HeavyIsBreakpoint(void);
#endif
};
