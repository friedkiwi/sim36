    // ---- transfer control, the loader and task termination (As36Transfer.cpp) ----

    // The transfer table entry's attribute byte, SA21-9436 3-75.
    static constexpr uint8_t kAttributeTranslated = 0x80;
    static constexpr uint8_t kAttributeSwapped = 0x40;
    static constexpr uint8_t kAttributePrivileged = 0x20;
    static constexpr uint8_t kAttributeResident = 0x10;
    static constexpr uint8_t kAttributeCoreSizeDiffers = 0x04;
    static constexpr uint8_t kAttributeNeedsTransientArea = 0x02;

    // Identifier 4 is the reserved "terminate this task"; identifiers run
    // 0..79 so the system transfer control table is 80 x 5 = 400 bytes.
    static constexpr uint8_t kTransferControlTerminate = 4;
    static constexpr uint8_t kTransferControlLastId = 79;
    static constexpr int kTransferControlEntryBytes = 5;
    // The abnormal-termination continuation is transfer slot 0x1D.
    static constexpr uint8_t kAbnormalTerminationTransferId = 0x1D;
    // The task dump reads the task block from queue header 37.
    static constexpr int kDumpTaskQueueHeader = 37;
    // The event type a return ACE is stamped with before it is posted.
    static constexpr uint16_t kReturnAceEventType = 0x13;

    // tb+57: the program block a transfer is in flight to.
    static constexpr int kTbTransferInterlock = 57;
    // Queue header 38 is where the dispatcher and the exit publish tb+69..71.
    static constexpr int kTaskWorkBaseQueue = 38;
    // A 3-byte chain head at guest 0xD00 + 4n is addressed by its LAST byte.
    static constexpr int kProgramBlockHashTable = 0xD00;
    static constexpr int kProgramBlockChainLast = 3;
    // Halfword giving the entry point table's offset within the module.
    static constexpr int kEntryPointTableOffset = 10;
    // rb+44 bit 0x40: the exit clears it and rebuilds the ATRs.
    static constexpr uint8_t kAtrStaleFlag = 0x40;
    // kTaskBlockBytes (160, the task block's share of the composite task
    // allocation) is declared by the task-creation family.

    // The fast transfer table at guest 0x270: a 4-byte save area, then
    // four-byte entries at +4, +8, +0xC, +0x10 and +0x14.
    static constexpr int kFastTransferSaveArea = 0x270;
    static constexpr int kFastTransferEntries = kFastTransferSaveArea + 4;
    static constexpr int kFastTransferLastEntry = 0x10;
    static constexpr uint8_t kFastTransferModeClear = 0x0B;
    static constexpr uint8_t kFastTransferModeSet = 0x80;

    // The relocating loader's 17-byte parameter list and request type bits.
    static constexpr int kLoaderPlDiskAddress = 0;
    static constexpr int kLoaderPlSectors = 3;
    static constexpr int kLoaderPlLinkAddress = 5;
    static constexpr int kLoaderPlStartControl = 8;
    static constexpr int kLoaderPlRldOffset = 11;
    static constexpr int kLoaderPlLoadAddress = 14;
    static constexpr int kLoaderPlBytes = 17;
    static constexpr uint8_t kLoaderRelativeAddress = 0x01;
    static constexpr uint8_t kLoaderToAddress = 0x02;
    static constexpr uint8_t kLoaderFetch = 0x04;
    static constexpr uint8_t kLoaderSystem = 0x08;
    static constexpr uint8_t kLoaderMemoryResidentOverlay = 0x10;
    static constexpr uint8_t kLoaderQNotRefreshable = 0x02;
    static constexpr int kTbRelocationFactor = 72;
    static constexpr int kTbLoaderDiskAddress = 74;
    static constexpr uint8_t kRldInvalid = 0xFD;
    static constexpr uint8_t kRldSkip = 0x80;
    static constexpr uint8_t kRldEnd = 0xFF;

public:
    // A native caller of the transfer routine that stays suspended while
    // guest code runs: the machine keeps this call stack outside guest
    // storage, so a per-task stack is the equivalent identity.
    enum class NativeTransferContinuation { NuptermSlot4, NuabSlot1D };

private:
    std::map<int, std::vector<NativeTransferContinuation>> nativeTransferContinuations_;
    void pushNativeTransferContinuation(int tb, NativeTransferContinuation kind);

    bool transferControlByAddress(SvcRequest& req);
    bool arrayTransfer(SvcRequest& req);
    bool transferControlById(SvcRequest& req);
    bool transferControl(SvcRequest& req, const std::string& call, int entry, int entryPoint);
    bool performTransfer(SvcRequest& req, const std::string& call, int pb, int entryPoint);
    static std::string describeAttribute(uint8_t a);
    std::string tagSuffix(int entry);
    std::string entryTag(int entry);
    void freeChainedRequestBlock(int pointerField, const std::string& call, const std::string& why);
    void freeChainedRequestBlock(int pointerField, int taskBlock, const std::string& call, const std::string& why);
    void freeTerminatedTaskBlock(int tb, const std::string& call);
    int releaseTerminatingTaskProgramState(int tb, const std::string& call);
    int releaseRequestChainProgramState(int rb, const std::string& call);
    void releaseRequestMappedBlocks(int rb, int pb, const std::string& call);
    int releaseTaskWorkSpaces(int tb, const std::string& call);
    void retireTerminatingTaskFromScheduler(int tb, const std::string& call);
    int releaseTaskTerminationIoQueues(int tb, const std::string& call);
    int releaseTaskQueueMatches(uint8_t queue, int tb, int keyLastByte, int chainLastByte, int bytes,
                                bool honorRetainFlag, const std::string& routine, const std::string& call);
    void saveTaskWorkBase(int taskBlock, const std::string& call);
    bool mainStorageExit(SvcRequest& req);
    bool terminateTaskRoot(SvcRequest& req);
    int scanTerminationDependents(int terminatingTb, int& immediate, const std::string& call);
    bool freeSecondRequestBlock(SvcRequest& req);
    int findProgramBlock(int bucket, int sector, const std::string& call);
    void chainProgramBlock(int bucket, int pb, const std::string& call);
    void recordLoadedMember(int pb, const uint8_t* nameBytes, int nameLength, long long extentSector);
    bool copyCallerAddressability(int callerRb, int calleeRb, int calleePb, const std::string& call);
    void referenceInheritedMapObject(int block, const std::string& call);
    int buildProgramBlock(int sector, int sectors, uint8_t attribute, const std::string& call);
    bool makeProgramBlockReady(int pb, const std::string& call);
    void invalidateSystemTransientOwner(const std::string& call);
    void invalidateSystemTransientForWrite(int at, int bytes, const std::string& call);
    bool alreadyResident(int at, const std::vector<uint8_t>& image);
    int requestBlockUnits(int pb, int callerRb);
    int resolveEntryPoint(int moduleAddress, int entryPointNumber);
    int readTransferVector(int at);
    void writeTransferVector(int at, uint8_t mode, uint8_t prefix, uint16_t iar);
    void applyTransferVector(int rb, int vector);
    bool fastTransfer(SvcRequest& req);
    bool fastExit(SvcRequest& req);
    bool dumpTask(SvcRequest& req);
    static int hexToDecimal(int v);
    void traceSvtubCloneGate(int old, int cfg);
    bool mainStorageRelocatingLoader(SvcRequest& req);
    bool readModule(int diskAddress, int sectors, int at);
    bool relocateModule(int at, int delta, int diskAddress, int sectors, int rldOffset);

public:
    // Read-only monitor snapshot of one program block whose load header was
    // observed by recordLoadedMember.  Both module addresses are intentional:
    // logicalBase is the address the guest executes, physicalBase the
    // main-storage backing this model uses.
    struct LoadedProgramBlock {
        int programBlock;
        std::string name;
        long long extentSector;
        int logicalBase;
        int physicalBase;
        int programBlockSector;
        uint8_t attribute;
        int pagesReady;
        int pageCount;
        bool ready;
    };
    std::vector<LoadedProgramBlock> getLoadedProgramBlocks();
    int getActiveProgramBlock(int taskBlock) const { return activeProgramBlock(taskBlock); }
    // Monitor seam: the termination dependency scan, run against the current
    // task without synthesising a supervisor call.
    int scanTerminationDependentsExperiment(int& immediate);
    // Monitor seam: the supervisor-only slot-4 termination continuation
    // entered on the current task's real register save.
    bool enterTerminationContinuationExperiment();

private:
