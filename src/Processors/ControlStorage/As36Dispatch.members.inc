    // ---- Dispatch family: the task dispatcher, waits, posts, events, ----------
    // ---- resources and the action controller (As36Dispatch.cpp) ----------------

    // tb+4, the task state byte, and the bits the dispatcher reads: 0x80
    // waiting, 0x10 long wait, 0x08 suspended inside a supervisor call, 0x04
    // tested beside 0x08, 0x02 routes to the error-control-storage arm.
    static constexpr uint8_t kTbStateWaiting = 0x80, kTbStateLongWait = 0x10;
    static constexpr uint8_t kTbStateInSvc = 0x08, kTbStateSvcComplete = 0x04, kTbStateEcs = 0x02;
    // TB_STAT2 (tb+5) wait codes this corpus can name: event wait, general
    // wait, suspend wait.  SA21-9436 3-102 lists six and prints no values.
    static constexpr uint8_t kWaitEvent = 0x80, kWaitGeneral = 0x20, kWaitSuspend = 0x02;
    // TB_STAT2 wait code 0x10, resource enqueue (SVC 21's wait).
    static constexpr uint8_t kWaitResourceEnqueue = 0x10;
    // TB_STAT2 bit 0x40: an asynchronous child waiting for its program to be
    // made ready.
    static constexpr uint8_t kWaitProgramNotReady = 0x40;
    // System queue header 37: the task that is currently dispatched.
    static constexpr int kDispatchedTaskQueue = 37;
    // rb+47: cleared when a request block is built, stored 0xFF when the
    // task is readied.  Its meaning is not established; the store is.
    static constexpr int kRbTransientMark = 47;
    // The console terminal unit block's job/JCB pointer, +0x63..0x65.
    static constexpr int kConsoleTubJobPointerOffset = 0x63;
    // EBCDIC "SM", at measurement block + 3.
    static constexpr uint16_t kMeasurementBlockEyecatcher = 0xE2D4;
    // System queue 30 holds the elements a general post also completes;
    // system queue 4 the machine actions inline 1 bit 2 would run.
    static constexpr uint8_t kGeneralWaitCondition = 0x20;
    static constexpr uint8_t kGeneralPostElementQueue = 30;
    static constexpr uint8_t kMachineActionQueue = 4;
    // 3 bytes ending at 213: the job control block's AQE queue head; 3 bytes
    // ending at 79: its current task block.
    static constexpr int kJcbAqeQueue = 211;
    static constexpr int kJcbAqeQueueLast = kJcbAqeQueue + 2;
    static constexpr int kJcbCurrentTaskBlock = 77;
    // The ceiling a critical-resource holder is raised to.
    static constexpr uint8_t kCriticalResourceCeiling = 240;
    // One timer unit, 8.192 ms, and 86 400 s / 8.192 ms exactly.
    static constexpr double kTimerUnitMilliseconds = 8.192;
    static constexpr int kTimerUnitsPerDay = 10546875;
    // The work-station controller lock byte and the queues its scan walks.
    static constexpr int kWsControllerLockByte = 0x08B2;
    static constexpr uint8_t kWsControllerLockBit = 0x02;
    static constexpr int kWsIoRequestQueue = 2;
    static constexpr int kWsIobQueueSelectorBase = 0x0AFD;
    // SSP R3 posts work-station device status as mask 0x29 where 7.5 posts
    // 0x2D; dispatched as 0x2D by emulator decision.
    static constexpr uint8_t kR3WorkStationDeviceStatusMask = 0x29;

    // The pending redispatch request lives in the header (redispatch_).
    // Async children built by a transfer but not yet readied.
    std::vector<int> pendingAsyncChildren_;
    // True when the MSP last stopped because every task is waiting and none
    // is ready: idle for an external event, not faulted.
    bool idleEventWait_ = false;
    // Console sign-on phase ordering: the phase-2 job task captured when an
    // asynchronous transfer enters the sign-on job entry member, and whether
    // it has been dispatched at least once.
    int phase2SvatJobTask_ = 0;
    bool phase2SvatDispatched_ = false;

    // ---- the action controller as SVC 0B sees it ------------------------------
    struct PendingSetAction {
        uint8_t mask;
        uint16_t iar;
        int taskBlock;
        std::string member;
    };
    struct ActionMaskCoverage {
        uint8_t mask = 0;
        int seen = 0, coalesced = 0, discharged = 0, undischarged = 0;
        uint16_t lastIar = 0;
        int lastTaskBlock = 0;
        std::string lastMember;
    };
    // The two status words: masks below 0x40 and at or above it.
    uint64_t actionStatusLow_ = 0;
    uint64_t actionStatusHigh_ = 0;
    std::deque<PendingSetAction> actionQueue_;
    std::map<uint8_t, ActionMaskCoverage> actionCoverage_;
    static bool isMaskAboveWordSplit(uint8_t mask) { return mask >= 0x40; }
    static uint64_t actionStatusBit(uint8_t mask);
    ActionMaskCoverage& coverage(uint8_t mask);
    void queueSetAction(uint8_t mask, SvcRequest& req);
    void drainActionScheduler(const std::string& call);
    void executeSetAction(const PendingSetAction& a, const std::string& call);
    static std::string actionSchedulerRoutineName(uint8_t mask);
    static std::string actionSchedulerDisposition(uint8_t mask);
    bool hasUndischargedActions() const;
    std::string undischargedActionsSuffix() const;
    void actionControllerReport(const std::function<void(const std::string&)>& sink) const;
    void traceActionControllerAtNoTaskExit(const std::string& call);

    // ---- the dispatcher ----------------------------------------------------
    void taskWaitReturn(int tb, uint8_t waitCode, uint8_t rByte, uint8_t q, const std::string& call);
    void readyQueueRemove(int tb);
    void readyTask(int tb, const std::string& call);
    int selectNextTask(const std::string& call);
    int phaseOrderJobTaskSelection(int head, const std::string& call);
    int consoleUnitBlock();
    int consoleTubJobPointer();
    int firstReadyTaskOtherThan(int skip);
    bool dispatch(const std::string& call);
    bool resumeSupervisorCall(int tb, int rb, const std::string& call);
    void readyPendingAsyncChildren(const std::string& call);
    bool waitAndDispatch(int tb, const std::string& call);
    bool idleEventWait() const { return idleEventWait_; }
    void signalNewWork(const std::string& call);
    void enableDispatching(const std::string& call, const std::string& site);

    // ---- waits ---------------------------------------------------------------
    bool generalWait(SvcRequest& req);
    bool supervisorGeneralWait(int tb, int mask, const std::string& call);
    bool taskWait(SvcRequest& req);
    bool asynchronousTaskWait(SvcRequest& req);
    bool canSuspendAsynchronously(int tb, const std::string& call);
    bool refuseField(const std::string& call, int tb, int field, bool refused, const std::string& why,
                     const std::string& site);
    bool asynchronousTaskReadyCheck(SvcRequest& req);
    bool postTaskCheckDispatch(int tb, const std::string& call, bool returnXr2OnPost = true,
                               bool deferDispatch = false);
    bool completedEventForTaskBlock(int tb, const std::string& call, bool returnXr2OnPost = true);
    bool quickLock(SvcRequest& req);

    // ---- queues, posts and events -----------------------------------------------
    bool scanSystemQueue(SvcRequest& req);
    bool scanForBlock(int headerField, int block, int chainLastByte);
    int scanForField(int headerField, int argument, int argumentLastByte, int length, int chainLastByte);
    bool testAndSet(SvcRequest& req);
    bool taskBlockPriorityQueue(SvcRequest& req);
    void setDispatchingPriority(int tb, uint8_t requested);
    void requeueByPriority(int tb, int chainLastByte, uint8_t headerNumber);
    bool postActionControlElement(SvcRequest& req);
    bool eventPost(SvcRequest& req);
    bool postEvent(int ecm, uint8_t headerNumber, uint8_t indicators, const std::string& call);
    bool completeToTask(int ace, uint8_t indicators, const std::string& call, bool returnXr2OnPost = true,
                        bool deferDispatch = false);
    bool postTaskCheck(int tb, const std::string& call, bool returnXr2OnPost = true, bool deferDispatch = false);
    bool postTaskByTaskId(SvcRequest& req);
    bool eventWait(SvcRequest& req);
    static bool waitEventTypeMatches(uint16_t wr6, uint16_t type);
    bool completedEventForTask(SvcRequest& req);
    bool completedEvent(int tb, int rb, uint8_t q, bool returnXr2, const std::string& call);
    void longWaitPriority(int tb);
    bool taskPost(SvcRequest& req);
    bool postTaskBlock(int tb, uint8_t conditions, const std::string& call);
    bool postTaskConditionsDirect(int tb, uint8_t conditions, const std::string& call);
    bool postTaskConditionsFromTransient(int tb, uint8_t conditions, const std::string& call);
    bool generalPost(SvcRequest& req);
    bool postActionControllerStatusWord(SvcRequest& req);
    bool incrementSystemEventCounters(SvcRequest& req);
    void incrementEventCounter(int taskBlock, int counter);
    bool logTraceInformation(SvcRequest& req);
    bool timeOfDay(SvcRequest& req);

    // ---- the work-station entry scan survey (read-only) ------------------------
    struct WsScanEntry {
        int dub = 0;
        uint16_t eyecatcher = 0;
        uint8_t at06 = 0, at07 = 0, cls = 0, unit = 0, at27 = 0;
        uint16_t queueSel = 0;
        uint8_t at2A = 0, at7C = 0;
        int iob = 0;
        std::string verdict;
    };
    bool wsSurveyReadable(int addr, int len) const;
    int wsIobGet(int dub, int& queueHead);
    std::string wsSearchVerdict(const WsScanEntry& r);
    WsScanEntry wsScanRead(int a);
    std::vector<WsScanEntry> wsScanQueue(int queue, int linkOffset);
    void wsEntryScanSurvey(const std::string& call, const std::function<void(const std::string&)>& sink);
    static void wsScanPrint(const std::function<void(const std::string&)>& sink, const WsScanEntry& r);

    // ---- SVC 20 and 21: resource allocation ----------------------------------
    static constexpr uint8_t kRenqCriticalSystemResource = 0x20;
    static constexpr uint8_t kRenqJcbInXr1 = 0x10;
    static constexpr uint8_t kRenqReturnAqeInXr2 = 0x04;
    static constexpr uint8_t kRenqWait = 0x01;
    static constexpr uint8_t kRenqEnqueue = 0x80;
    static constexpr uint8_t kRenqDeviceResource = 0x04;
    static constexpr uint8_t kQueueFifo = 0x00, kQueueLifoFlag = 0x10, kQueueDequeueFlag = 0x20;
    bool specificResourceDequeue(SvcRequest& req);
    static uint8_t resourceShareCombine(uint8_t a, uint8_t b);
    static bool resourceShareConflicts(uint8_t a, uint8_t b);
    int resourceOwnerTaskBlock(int aqe);
    void raiseHolderPriority(int tb);
    void restoreHolderPriority(int tb);
    void resourceShareAccumulate(uint8_t& all, uint8_t& blocking, int aqe, int tb);
    bool resourceEnqueueDequeue(SvcRequest& req);
    bool resourceEnqueue(int resq, int headerField, int ownerQueue, int owner, uint8_t request, bool nested, int mine,
                         int& aqe);
    void resourceDequeueElement(int aqe, int ownerQueue, uint8_t request);
    int releaseTaskResources(int tb, const std::string& call);
    bool resourceEvaluateQueue(int headerField, uint8_t& levelAll, uint8_t& levelBlocking, bool wait);
