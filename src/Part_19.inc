        CVector leftSource;
        if (!ComputeLeftFireSource(ped, rightSource, leftSource))
            return firedRight;

        ++gSecondShotAttempts;
        const bool firedLeft = fireFn(weapon, shooter, &leftSource);
        if (firedLeft) {
            ++gSecondShotSuccess;
            if (!gLoggedFirstSecondShot) {
                char line[320];
                std::snprintf(line, sizeof(line),
                    "DualWieldVC v12: second native shot succeeded at muzzle %.3f %.3f %.3f.",
                    leftSource.x, leftSource.y, leftSource.z);
                Log(line);
                gLoggedFirstSecondShot = true;
            }
        }

        return firedRight;
    }

    static bool __fastcall AttackFireHook1(CWeapon* weapon, void*, CEntity* shooter, CVector* rightSource) {
        return AttackFireHookImpl(gFirePatch1, weapon, shooter, rightSource);
    }

    static bool __fastcall AttackFireHook2(CWeapon* weapon, void*, CEntity* shooter, CVector* rightSource) {
        return AttackFireHookImpl(gFirePatch2, weapon, shooter, rightSource);
    }

    static bool DecodeRelativeCall(uintptr_t address, uintptr_t& target, int32_t& rel) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(address);
        if (p[0] != 0xE8)
            return false;
        std::memcpy(&rel, p + 1, sizeof(rel));
        target = address + 5 + rel;
        return true;
    }

    static bool IsExecutableAddress(uintptr_t address) {
        if (!address)
            return false;

        MEMORY_BASIC_INFORMATION mbi = {};
        if (!VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)))
            return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return false;

        const DWORD prot = mbi.Protect & 0xFFu;
        return prot == PAGE_EXECUTE || prot == PAGE_EXECUTE_READ ||
            prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
    }

    static bool InstallCallPatch(uintptr_t address, uintptr_t expectedTarget, void* hook, CallPatch& patch);

    static bool ResolveAttackFireCalls(uintptr_t& first, uintptr_t& second) {
        first = second = 0;
        HMODULE exe = GetModuleHandleA(0);
        if (!exe)
            return false;
        const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return false;
        const IMAGE_NT_HEADERS* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(exe) + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return false;

        const uint8_t prefix[] = { 0x8D,0x0C,0xCD,0,0,0,0,0x8D,0x0C,0x49,0x01,0xD9,0x81,0xC1,0x08,0x04,0,0,0xE8 };
        const uint8_t suffix[] = { 0x0F,0xBE,0x83,0x04,0x05,0,0 };
        const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        uintptr_t hits[4] = {};
        int hitCount = 0;

        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            if ((sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
                continue;
            const uint8_t* begin = reinterpret_cast<const uint8_t*>(exe) + sec[i].VirtualAddress;
            const size_t size = sec[i].Misc.VirtualSize;
            if (size < sizeof(prefix) + 4 + sizeof(suffix))
                continue;
            for (size_t off = 0; off + sizeof(prefix) + 4 + sizeof(suffix) <= size; ++off) {
                if (std::memcmp(begin + off, prefix, sizeof(prefix)) != 0)
                    continue;
                if (std::memcmp(begin + off + sizeof(prefix) + 4, suffix, sizeof(suffix)) != 0)
                    continue;
                const uintptr_t call = reinterpret_cast<uintptr_t>(begin + off + sizeof(prefix) - 1);
                uintptr_t target = 0; int32_t rel = 0;
                if (!DecodeRelativeCall(call, target, rel) || !IsExecutableAddress(target))
                    continue;
                if (hitCount < 4)
                    hits[hitCount] = call;
                ++hitCount;
            }
        }

        if (hitCount != 2)
            return false;
        if (hits[0] > hits[1]) { uintptr_t tmp = hits[0]; hits[0] = hits[1]; hits[1] = tmp; }
        first = hits[0]; second = hits[1];
        return true;
    }

    static bool gFireHooksResolved = false;

    static bool EnsureFireHooksInstalled() {
        if (gFirePatch1.installed && gFirePatch2.installed)
            return true;
        if (gFireHooksResolved)
            return gFirePatch1.installed || gFirePatch2.installed;
        gFireHooksResolved = true;

        uintptr_t call1 = 0, call2 = 0;
        if (!ResolveAttackFireCalls(call1, call2)) {
            // Exact 1.0 fallback from the supplied Vice City IDB.
            call1 = ADDR_ATTACK_FIRE_CALL_1;
            call2 = ADDR_ATTACK_FIRE_CALL_2;
        }

