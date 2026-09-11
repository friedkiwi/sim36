// Milestone 5, task creation family: declarations pasted into the private
// section of As36ControlStorageProcessor.  Task creation (the asynchronous
// transfer's task builder), SVC 31/32 and the control-block factory, SVC 12
// and 13, the task work area allocator (SVC 33/34/35), SVC 36, SVC 26 and
// the control storage transient bodies.

    // ---- the geometry of one task-creation allocation -------------------------
    // A task block is 160 bytes: ten 16-byte units added to the request
    // block's length, and the cursor over the same allocation steps by 160
    // before anything else is built in it.  One allocation holds, in order:
    //   [ task block 160 ][ ACE 32, iff Q bit 7 ][ SM block 64, iff SMF ][ request block ]
    static constexpr int kTaskBlockBytes = 160;
    static constexpr int kTaskBlockUnits = kTaskBlockBytes / GuestHeap::kGranularity;
    // Q bit 7, "return control to the requesting program", adds two units
    // and an action control element is built in them.
    static constexpr int kReturnAceUnits = ActionControlElement::kSize / GuestHeap::kGranularity;
    // tb+29..31: the chain a created task is linked on; the head is
    // tb+29..31 of the task block at guest 0x0F00, so head and link are the
    // same field of two different task blocks.
    static constexpr int kTbChildChainLast = 31;
    // tb+13 = 250, the same byte a long wait sets, so a task starts life
    // marked the way a long waiter is.
    static constexpr int kTbLongWaitMark = 13;
    // tb+4 | tb+5 as one halfword: the new task is WAITING (0x80) on
    // program-not-ready (TB_STAT2 bit 0x40, kWaitProgramNotReady in the
    // dispatch family), which is why it does not run before its module is in
    // storage; the loader's readiness post clears it.
    static constexpr uint16_t kNewTaskState = (0x80 << 8) | 0x40;
    // tb+32 = 0x60: TB_STAT bits 0x40 and 0x20 are not identified; the
    // constant is transcribed rather than interpreted.
    static constexpr uint8_t kNewTaskStatus = 0x60;
    // A synthetic Q-byte meaning "this attach came from inside the
    // supervisor": do not report the identifier back in WR5, and give the
    // task priority floor 0xF0.
    static constexpr uint8_t kInternalAttachQByte = 0xFE;
    static constexpr uint8_t kInternalAttachPriority = 0xF0;
    // The task priority queue insert: priority, system request, sort key at
    // displacement 7.
    static constexpr uint8_t kTaskPriorityFlags = 0xC7;
    // rb+47, written zero by the request block builder.
    static constexpr int kRequestBlockTransientMark = 47;

    // The task identifier counter: host state on the machine, host state
    // here.  Identifiers allocated this way are always even.
    int taskIdCounter_ = 0;
    // The request block that was current when a task was created; written
    // and never read back, kept because the store is real.
    int requestBlockBeforeAttach_ = 0;
    // The system measurement facility's state bit, native controller state
    // kept outside guest low storage; task creation reads it to decide the
    // shape of every subsequently created task.
    bool systemMeasurementEnabled_ = false;

public:
    bool systemMeasurementEnabled() const { return systemMeasurementEnabled_; }
    // Apply the state-changing half of native action 0x1A (the system
    // measurement facility's size/stop actions).
    bool applySystemMeasurementAction(uint8_t action, const std::string& call);

private:
    int attachNewTask(int pb, int callerTb, int callerRb, uint8_t q, const std::string& call, int& rb);
    int allocateTaskId(const std::string& call);
    int systemMeasurementBlockUnits(const std::string& call);
    void buildRequestBlock(int rb, int units, int pb, int tb, int callerRb);

    // ---- SVC 31, 32 and the control-block factory -----------------------------
    bool attachTask(SvcRequest& req);
    bool attachTaskFailed(SvcRequest& req, int failure, bool waitRequested);
    bool detachTask(SvcRequest& req);
    int buildControlBlock(uint8_t type, uint8_t flags, int arg3, int arg4, int taskBlock, int entry,
                          const std::string& call, int& failureCode);

    // ---- SVC 12 and 13 ----------------------------------------------------------
    // pb+8 bits the region grower requires before it will expand a region.
    static constexpr uint8_t kGrowableFlags = 0x50;
    bool getPage(SvcRequest& req);
    bool maintainUserAreaPages(SvcRequest& req);
    static const char* userAreaFunction(uint8_t f);

    // ---- the task work area allocator: SVC 33, 34, 35 ---------------------------
    int allocateTaskWorkArea(int sectors, std::string& why);
    bool freeTaskWorkArea(int relative, int sectors, std::string& why);
    void unchainTaskWorkAreaElement(int header, int element);
    bool taskWorkAreaAllocate(SvcRequest& req);
    bool taskWorkAreaFree(SvcRequest& req);
    bool workSpaceMaintenance(SvcRequest& req);

    // ---- SVC 36 -----------------------------------------------------------------
    // Q-byte bit 2: on = receive error, off = transmit error.
    static constexpr uint8_t kSmfcReceiveError = 0x20;
    bool smfc(SvcRequest& req);

    // ---- SVC 26, Prepare Print Buffer ----------------------------------------------
    // The printer IOB fields, as the handler and its helpers place them.
    static constexpr int kIobpFlags = 7;
    static constexpr uint8_t kIobpFlagSpooled = 0x02;
    static constexpr int kIobpBuffer = 13;
    static constexpr int kIobpLength = 16;
    static constexpr int kIobpUnitBlock = 21;
    static constexpr int kIobpFormsLength = 24;
    static constexpr int kIobpCurrentLine = 25;
    static constexpr int kIobpControl = 26;
    static constexpr uint8_t kIobpCtlPrint = 0x40;
    static constexpr uint8_t kIobpCtl2ByteBeginScan = 0x10;
    static constexpr uint8_t kIobpCtl2ByteMode = 0x08;
    static constexpr uint8_t kIobpCtlScanOutputs = 0x16;
    static constexpr int kIobpSkipBefore = 27;
    static constexpr int kIobpSpaceBefore = 28;
    static constexpr int kIobpSkipAfter = 29;
    static constexpr int kIobpSpaceAfter = 30;
    static constexpr int kIobpFormFeedsBefore = 31;
    static constexpr int kIobpFormFeedsAfter = 32;
    static constexpr int kPubFormsLength = 69;
    static constexpr int kPubCurrentLine = 70;
    // The SCS control codes.
    static constexpr uint8_t kScsFormFeed = 0x0C;
    static constexpr uint8_t kScsCarriageReturn = 0x0D;
    static constexpr uint8_t kScsPresentationPosition = 0x34;
    static constexpr uint8_t kScsPositionVertical = 0xC4;
    static constexpr uint8_t kScsPositionHorizontal = 0xC8;
    static constexpr uint8_t kScsShiftOut = 0x0E;
    static constexpr uint8_t kScsShiftIn = 0x0F;
    static constexpr uint8_t kScsBlank = 0x40;
    static constexpr uint8_t kScsGraphicError = 0xFF;
    static constexpr int kBlankRunThreshold = 3;
    // The first six bytes of the print buffer are the room the leading
    // control codes are inserted into (SA21-9436 3-114's worked example).
    static constexpr int kPrintBufferDataOffset = 6;
    // The SSP Ideographic feature indicator: guest 0x808 bit 0x80.
    static constexpr int kScaDsspf = 0x0808;
    static constexpr uint8_t kScaMkkkf = 0x80;

    // The work area the print-buffer scan and its helpers share.
    struct PrintWork {
        machine::MachineState& m;
        int buffer;
        int read, write;
        int forms, line, blankRun = 0, formFeeds = 0;
        uint8_t lastCode = 0;

        PrintWork(machine::MachineState& state, int bufferAt, int formsLength, int currentLine)
            : m(state), buffer(bufferAt), read(bufferAt), write(bufferAt), forms(formsLength), line(currentLine) {}

        void emit(uint8_t b) { m.writeByte(write++, b); }
        void emitCarriageReturn()
        {
            lastCode = kScsCarriageReturn;
            emit(kScsCarriageReturn);
        }
        void insertSkip()
        {
            lastCode = kScsPresentationPosition;
            emit(kScsPresentationPosition);
            emit(kScsPositionVertical);
            emit(static_cast<uint8_t>(line));
        }
        // A run of more than three blanks has already been COPIED to the
        // output by the loop, so the flush rewinds over them before writing
        // the three-byte position code.
        void flushBlankRun()
        {
            if (blankRun > kBlankRunThreshold) {
                write -= blankRun;
                emit(kScsPresentationPosition);
                emit(kScsPositionHorizontal);
                emit(static_cast<uint8_t>(blankRun));
            }
            blankRun = 0;
        }
        void skipSpace(int skip, int space);
    };
    bool preparePrintBuffer(SvcRequest& req);
    bool scanData(int iob, PrintWork& work, int length, bool ideographic, bool spooled, uint8_t& control);
    bool refuseDirectIdeographic(int iob, bool spooled);

    // ---- the control storage transient bodies ------------------------------------
    bool transientTransferM36(uint8_t function, uint8_t subFunction, int xr1, int requestBlock);
    std::vector<uint8_t> buildTfrm36GuestPayload(int unit, bool autoSignOn);
    bool transientIplControl(int requestBlock);
    bool transientStorageCleanup(int xr1, int xr2, int requestBlock);
    bool transientTimer(uint8_t inline2, int xr2, int taskBlock, int requestBlock);
    bool registerCptcTimer(int block, int taskBlock, int requestBlock);
    bool cancelCptcTimer(uint8_t inline2, int block, int taskBlock);
    bool transientTimeOfDay(int xr1);
    bool transientLoadControl(int xr2);
    bool loadControlChain(int block, uint8_t control);
    void resolveArea(int entry, uint8_t control);
    static bool loadControlArea(int key, int& sector, int& plus8);
    bool resolveThreeByteAddress(int value, int& real);
    void writeZonedTwoDigits(int address, int value);
    void writePackedTwoDigits(int address, int value);

    // Transient 03's block: bit 0x08 the block is a chain of 90-byte
    // entries, bit 0x10 resolve this entry.
    static constexpr uint8_t kNulcChained = 0x08, kNulcResolve = 0x10;
    static constexpr int kNulcNotFound = 0xFF0000;
    static constexpr int kNulcChainStride = 90;
    // A bound the machine does not have: the links are guest data.
    static constexpr int kNulcChainMaxEntries = 256;
    // Bit 0x800000 of a three-byte guest address: the low sixteen bits are
    // task-translated.
    static constexpr int kThreeByteTranslated = 0x800000;
    // The keyboard translate table family, FB10..FBEF, keyed by guest 0x097F.
    static constexpr int kKeyboardTableFirst = 0xFB10, kKeyboardTableLast = 0xFB10 + 223;
    static constexpr int kKeyboardTableSelector = 0x097F;

    // ---- native timer registrations (transient 0A) -----------------------------------
    // A native timer registration is control-storage state, not guest
    // work station data.  Only the proven registration and cancellation
    // subset is kept.
    struct NutixTimerRegistration {
        int taskBlock = 0, requestBlock = 0, trb = 0;
        uint8_t key = 0, control = 0;
        uint32_t interval = 0;
        // Host monotonic clock, in nanoseconds since the clock's epoch.
        long long registeredTimestamp = 0, dueTimestamp = 0;
        long long expirationCount = 0;
    };
    std::map<std::string, NutixTimerRegistration> nutixTimers_;
    // One calendar day in the machine timer's 8.192 ms units: exactly
    // 86400 seconds.
    static constexpr uint32_t kNutixMachineDayUnits = 0x00A0EEBB;
    static long long timerNow();
    static long long timerUnitsToTicks(uint32_t units);
    static uint32_t timerRemainingIntervalAt(const NutixTimerRegistration& timer, long long now);
    uint32_t timerRemainingInterval(const NutixTimerRegistration& registration);

public:
    // A stable, read-only view of one native timer entry.
    struct NativeTimerInfo {
        int taskBlock = 0, requestBlock = 0, trb = 0;
        uint8_t key = 0, control = 0, type = 0;
        uint32_t originalInterval = 0, remainingUnits = 0;
        int millisecondsUntilDue = 0;
        long long expirationCount = 0;
        std::string expiryDisposition;
    };
    // Milliseconds until the next native timer must be serviced: -1 when
    // none is registered, 0 when at least one is due.
    int millisecondsUntilNextNativeTimer() const;
    std::vector<NativeTimerInfo> nativeTimerState() const;
    // Run every due native timer through its expiry disposition; true when
    // a callback made an SSP task runnable.
    bool serviceDueNativeTimers(int& expired);

    // Set when the guest has asked for a pseudo IPL, with the source its
    // request named.  The machine is stopped at that point; taking the
    // restart is the operator's move.
    bool pseudoIplRequested() const { return pseudoIplRequested_; }
    const std::string& pseudoIplSource() const { return pseudoIplSource_; }

private:
    bool pseudoIplRequested_ = false;
    std::string pseudoIplSource_;
