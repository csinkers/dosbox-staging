#include "debug_internal.h"

static std::list<CBreakpoint*> BPoints = {}; // Must be kept sorted by id

static int GetAvailableId()
{
	int newId = 0;
	for (auto& it : BPoints) {
		int id = it->GetId();
		if (id == newId) {
			newId++;
		} else {
			return newId;
		}
	}
	return newId;
}

CBreakpoint::CBreakpoint()
	: id(GetAvailableId())
{
}

void CBreakpoint::ActivateInner(bool _active)
{
	if (_active) {
		// Set 0xCC and save old value
		uint8_t data = mem_readb(location);
		if (data != 0xCC) {
			oldData = data;
			mem_writeb(location, 0xCC);
		} else if (!active) {
			// Another activate breakpoint is already here.
			// Find it, and copy its oldData value
			CBreakpoint* bp = FindOtherActiveBreakpoint(location, this);

			if (!bp || bp->oldData == 0xCC) {
				// This might also happen if there is a real 0xCC
				// instruction here
				DEBUG_ShowMsg("DEBUG: Internal error while activating breakpoint.\n");
				oldData = 0xCC;
			} else {
				oldData = bp->oldData;
			}
		}
	} else {
		if (mem_readb(location) == 0xCC) {
			if (oldData == 0xCC) {
				DEBUG_ShowMsg("DEBUG: Internal error while deactivating breakpoint.\n");
			}

			// Check if we are the last active breakpoint at this location
			bool otherActive = FindOtherActiveBreakpoint(location, this) != 0;

			// If so, remove 0xCC and set old value
			if (!otherActive) {
				mem_writeb(location, oldData);
			}
		}
	}
}

void CBreakpoint::Activate(bool _active)
{
#if !C_HEAVY_DEBUG
	if (GetType() == BKPNT_PHYSICAL) {
		ActivateInner(_active && enabled);
	}
#endif
	active = _active;
}

void CBreakpoint::Enable(bool _enabled)
{
	if (enabled == _enabled)
		return; // Nothing to do

	enabled = _enabled;
	ActivateInner(active && enabled);
}

// Statics

std::list<CBreakpoint*>::const_iterator CBreakpoint::begin() { return BPoints.begin(); }
std::list<CBreakpoint*>::const_iterator CBreakpoint::end() { return BPoints.end(); }

static void AddToList(CBreakpoint *bp)
{
	const auto it = std::lower_bound(BPoints.begin(),
	                                 BPoints.end(),
	                                 bp,
	                                 [](const CBreakpoint* x, const CBreakpoint* y) {
		                                 return x->GetId() < y->GetId();
	                                 });

	BPoints.insert(it, bp);
}

CBreakpoint* CBreakpoint::AddBreakpoint(uint16_t seg, uint32_t off, bool once)
{
	auto bp = new CBreakpoint();
	bp->SetAddress(seg, off);
	bp->SetOnce(once);
	AddToList(bp);
	return bp;
}

CBreakpoint* CBreakpoint::AddIntBreakpoint(uint8_t intNum, uint16_t ah,
                                           uint16_t al, bool once)
{
	auto bp = new CBreakpoint();
	bp->SetInt(intNum, ah, al);
	bp->SetOnce(once);
	AddToList(bp);
	return bp;
}

CBreakpoint* CBreakpoint::AddMemBreakpoint(uint16_t seg, uint32_t off)
{
	auto bp = new CBreakpoint();
	bp->SetAddress(seg, off);
	bp->SetOnce(false);
	bp->SetType(BKPNT_MEMORY);
	AddToList(bp);
	return bp;
}

void CBreakpoint::ActivateBreakpoints()
{
	// activate all breakpoints
	for (auto& bp : BPoints) {
		bp->Activate(true);
	}
}

void CBreakpoint::DeactivateBreakpoints()
{
	// deactivate all breakpoints
	for (auto& bp : BPoints) {
		bp->Activate(false);
	}
}

void CBreakpoint::ActivateBreakpointsExceptAt(PhysPt adr)
{
	// activate all breakpoints, except those at adr
	for (auto& bp : BPoints) {
		// Do not activate breakpoints at adr
		if (bp->GetType() == BKPNT_PHYSICAL && bp->GetLocation() == adr) {
			continue;
		}
		bp->Activate(true);
	}
}

void CBreakpoint::EnableBreakpoint(int id, bool enable)
{
	for (auto& bp : BPoints) {
		// Do not activate breakpoints at adr
		if (bp->GetId() == id) {
			bp->Enable(enable);
			return;
		}
	}
}

// Checks if breakpoint is valid and should stop execution
bool CBreakpoint::CheckBreakpoint(Bitu seg, Bitu off)
{
	// Quick exit if there are no breakpoints
	if (BPoints.empty()) {
		return false;
	}

	// Search matching breakpoint
	for (auto i = BPoints.begin(); i != BPoints.end(); ++i) {
		auto bp = *i;

		if (bp->GetType() == BKPNT_PHYSICAL && bp->IsActive() &&
		    bp->GetLocation() == GetAddress(seg, off)) {
			// Found
			if (bp->GetOnce()) {
				// delete it, if it should only be used once
				BPoints.erase(i);
				bp->Activate(false);
				delete bp;
			} else {
				// Also look for once-only breakpoints at this address
				bp = FindPhysBreakpoint(seg, off, true);
				if (bp) {
					BPoints.remove(bp);
					bp->Activate(false);
					delete bp;
				}
			}
			return true;
		}
#if C_HEAVY_DEBUG
		// Memory breakpoint support
		else if (bp->IsActive()) {
			if ((bp->GetType() == BKPNT_MEMORY) ||
			    (bp->GetType() == BKPNT_MEMORY_PROT) ||
			    (bp->GetType() == BKPNT_MEMORY_LINEAR)) {

				// Watch Protected Mode MemoryOnly in pmode
				if (bp->GetType() == BKPNT_MEMORY_PROT) {
					// Check if pmode is active
					if (!cpu.pmode) {
						return false;
					}

					// Check if descriptor is valid
					Descriptor desc;
					if (!cpu.gdt.GetDescriptor(bp->GetSegment(), desc)) {
						return false;
					}

					if (desc.GetLimit() == 0) {
						return false;
					}
				}

				Bitu address;
				if (bp->GetType() == BKPNT_MEMORY_LINEAR) {
					address = bp->GetOffset();
				} else {
					address = GetAddress(bp->GetSegment(), bp->GetOffset());
				}

				uint8_t value = 0;
				if (mem_readb_checked(address, &value)) {
					return false;
				}

				if (bp->GetValue() != value) {
					// Yup, memory value changed
					DEBUG_ShowMsg("DEBUG: Memory breakpoint %s: %04X:%04X - %02X -> %02X\n",
					              (bp->GetType() == BKPNT_MEMORY_PROT) ? "(Prot)" : "",
					              bp->GetSegment(),
					              bp->GetOffset(),
					              bp->GetValue(),
					              value);
					bp->SetValue(value);
					return true;
				}
			}
		}
#endif
	}
	return false;
}

bool CBreakpoint::CheckIntBreakpoint([[maybe_unused]] PhysPt adr, uint8_t intNr,
                                     uint16_t ahValue, uint16_t alValue)
// Checks if interrupt breakpoint is valid and should stop execution
{
	if (BPoints.empty()) {
		return false;
	}

	// Search matching breakpoint
	for (auto i = BPoints.begin(); i != BPoints.end(); ++i) {
		auto bp = *i;
		if (bp->GetType() == BKPNT_INTERRUPT && bp->IsActive() &&
		    bp->GetIntNr() == intNr) {
			if ((bp->GetValue() == BPINT_ALL || bp->GetValue() == ahValue) &&
			    (bp->GetOther() == BPINT_ALL || bp->GetOther() == alValue)) {
				// Ignore it once ?
				// Found
				if (bp->GetOnce()) {
					// delete it, if it should only be used once
					BPoints.erase(i);
					bp->Activate(false);
					delete bp;
				}
				return true;
			}
		}
	}
	return false;
}

void CBreakpoint::DeleteAll()
{
	for (auto& bp : BPoints) {
		bp->Activate(false);
		delete bp;
	}
	BPoints.clear();
}

bool CBreakpoint::DeleteByIndex(uint16_t index)
{
	// Request is past the end
	if (index >= BPoints.size()) {
		return false;
	}

	auto it = BPoints.begin();
	std::advance(it, index);
	auto bp = *it;

	BPoints.erase(it);
	bp->Activate(false);
	delete bp;
	return true;
}

CBreakpoint* CBreakpoint::FindPhysBreakpoint(uint16_t seg, uint32_t off, bool once)
{
	if (BPoints.empty()) {
		return 0;
	}
#if !C_HEAVY_DEBUG
	PhysPt adr = GetAddress(seg, off);
#endif
	// Search for matching breakpoint
	for (auto& bp : BPoints) {
#if C_HEAVY_DEBUG
		// Heavy debugging breakpoints are triggered by matching seg:off
		bool atLocation = bp->GetSegment() == seg && bp->GetOffset() == off;
#else
		// Normal debugging breakpoints are triggered at an address
		bool atLocation = bp->GetLocation() == adr;
#endif

		if (bp->GetType() == BKPNT_PHYSICAL && atLocation && bp->GetOnce() == once) {
			return bp;
		}
	}

	return 0;
}

CBreakpoint* CBreakpoint::FindOtherActiveBreakpoint(PhysPt adr, CBreakpoint* skip)
{
	for (auto& bp : BPoints) {
		if (bp != skip && bp->GetType() == BKPNT_PHYSICAL &&
		    bp->GetLocation() == adr && bp->IsActive()) {
			return bp;
		}
	}
	return 0;
}

// is there a permanent breakpoint at address ?
bool CBreakpoint::IsBreakpoint(uint16_t seg, uint32_t off)
{
	return FindPhysBreakpoint(seg, off, false) != 0;
}

bool CBreakpoint::DeleteBreakpoint(int id)
{
	for (auto i = BPoints.begin(); i != BPoints.end(); ++i) {
		auto bp = *i;

		if (bp->GetId() != id) {
			continue;
		}

		BPoints.remove(bp);
		bp->Activate(false);
		delete bp;
		return true;
	}

	return false;
}

bool CBreakpoint::DeleteBreakpoint(uint16_t seg, uint32_t off)
{
	CBreakpoint* bp = FindPhysBreakpoint(seg, off, false);
	if (bp) {
		BPoints.remove(bp);
		bp->Activate(false);
		delete bp;
		return true;
	}

	return false;
}

void CBreakpoint::ShowList(void)
{
	// iterate list
	int nr = 0;
	for (auto& bp : BPoints) {
		if (bp->GetType() == BKPNT_PHYSICAL) {
			DEBUG_ShowMsg("%02X. BP %04X:%04X\n", nr, bp->GetSegment(), bp->GetOffset());
		} else if (bp->GetType() == BKPNT_INTERRUPT) {
			if (bp->GetValue() == BPINT_ALL) {
				DEBUG_ShowMsg("%02X. BPINT %02X\n", nr, bp->GetIntNr());
			} else if (bp->GetOther() == BPINT_ALL) {
				DEBUG_ShowMsg("%02X. BPINT %02X AH=%02X\n",
				              nr,
				              bp->GetIntNr(),
				              bp->GetValue());
			} else {
				DEBUG_ShowMsg("%02X. BPINT %02X AH=%02X AL=%02X\n",
				              nr,
				              bp->GetIntNr(),
				              bp->GetValue(),
				              bp->GetOther());
			}
		} else if (bp->GetType() == BKPNT_MEMORY) {
			DEBUG_ShowMsg("%02X. BPMEM %04X:%04X (%02X)\n",
			              nr,
			              bp->GetSegment(),
			              bp->GetOffset(),
			              bp->GetValue());
		} else if (bp->GetType() == BKPNT_MEMORY_PROT) {
			DEBUG_ShowMsg("%02X. BPPM %04X:%08X (%02X)\n",
			              nr,
			              bp->GetSegment(),
			              bp->GetOffset(),
			              bp->GetValue());
		} else if (bp->GetType() == BKPNT_MEMORY_LINEAR) {
			DEBUG_ShowMsg("%02X. BPLM %08X (%02X)\n", nr, bp->GetOffset(), bp->GetValue());
		}
		nr++;
	}
}
