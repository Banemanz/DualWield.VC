        ~Mod() {
            // RW teardown is handled by shutdownRwEvent while the engine is alive.
            LogRuntimeSummary();
            RestoreCallPatch(gPreRenderAnimPatch);
            RestoreCallPatch(gFirePatch2);
            RestoreCallPatch(gFirePatch1);
        }
    };

    static Mod gMod;
}
