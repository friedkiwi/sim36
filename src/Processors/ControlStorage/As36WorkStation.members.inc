    // ---- the work-station family: retained device actions, unit block ----
    // ---- resolution, the M36 display transfer, the console producers -------
    //
    // Declarations only; definitions are in As36WorkStation.cpp.  Included by
    // the class body's private section.
public:
    // A read-only tap for semantic work-station-controller boundaries that
    // are host-side and cannot be recovered from an instruction watch:
    // (event, unit, the selected/submitted unit block, the configured one).
    // Installing it changes no controller or guest state.
    std::function<void(const std::string&, int, int, int)> workStationDiagnosticObserver;

    // Observe a transport response behind a controller-owned action without
    // consuming it.
    bool observePendingDeviceInput();
    // Apply the status half of a real terminal response; posts no task event.
    bool deliverWorkStationInputStatus(int unitBlock);
    // Complete a retained work-station action through its own SVC-43 element.
    bool completePendingWorkStationInput();
    // Retained printer output completes when the guest gives the processor
    // away: every wait SVC passes through the wait-and-dispatch tail.
    bool completePendingPrinterOutput(const std::string& call);
    // The display behind a unit was switched off: fail one retained operation
    // against it and post the owner's element.
    bool failPendingWorkStationOperation(int unit, const std::string& call);
    bool deactivateWorkStationUnit(int unit);

    // The terminal unit block a work-station unit address belongs to, found
    // through the field the guest itself wrote (unit at +12).
    int resolveTubByUnit(int unitAddress);
    // The phase-2 configured unit block for a unit, excluding the bootstrap
    // console block: one with a W-id/OC association.
    int resolveConfiguredTubByUnit(int unitAddress);
    // The first eligible unit block on the shared queue whose +0x0C equals
    // the requested unit, in chain order.
    int resolveWsEntryTubByUnit(int unitAddress);

    // The most recently completed action-0 response consumed native
    // activation status and made SSP work runnable.
    bool lastM36WorkStationTransferPostedGuestWork() const { return lastM36WorkStationTransferPostedGuestWork_; }
    // The recovered configure success tail is normal controller behaviour;
    // the monitor may suppress or redirect it for differential experiments.
    bool cnfwsPowerOnAidExperiment = true;
    // Hold the power-on aid until the guest's next all-tasks-waiting boundary
    // (a deterministic stand-in for device latency).  Off by default.
    bool deferCnfwsPowerOnAid = false;
    bool cnfwsPowerOnAidConfiguredTargetExperiment = false;
    // Deliver a deferred power-on aid, if one is held; true when it posted
    // guest work.
    bool flushDeferredCnfwsPowerOnAid();

    // The control-plane state of the M36 display transfer: which configured
    // unit block's display is being lent to SSP, and prompt versus automatic
    // sign-on for this transfer.
    bool beginM36WorkStationTransfer(int tub, bool autoSignOn, bool controllerWorkPending, const std::string& call,
                                     bool& pending);
    bool reportWorkStationHriReady(int unit, const std::string& call);
    bool completePendingM36WorkStationTransfer(const std::string& call);
    bool endM36WorkStationTransfer(int tub, const std::string& call);
    bool isM36WorkStationTransferReady(int tub, bool autoSignOn) const;
    std::string m36WorkStationTransferState() const;

    // The guest-storage boundary of the power-on internal condition: an
    // element posted to the IPL/command task (id 9) carrying condition 0x14.
    bool postPowerOnInternalCondition(const std::string& call);
    // The guest-visible half of the controller's response post: copy the
    // response byte to the unit block's +0x8E, then optionally raise the
    // internal condition.
    bool deliverWorkStationControllerFunction(int tub, uint8_t function, bool storeByte, bool postInterrupt,
                                              const std::string& call, bool resumeFromMonitor = true,
                                              bool* postedGuestWork = nullptr);
    // Monitor-only control of the OC active bit a command task reads.
    bool setWorkStationOcActive(int tub, bool active, const std::string& call);

    // The generic I/O-attention producer: mark the terminal's unit block as
    // present and post the owning task.
    bool raiseDeviceAttention(int unitBlock, int taskBlock, const std::string& call);
    bool postDevicePresent(int taskBlock, int unitBlock, const std::string& call);
    // The inbound work-station completion typed 0x29 to the IPL/command task.
    bool postWorkStationPresent(int unitBlock, const std::string& call);
    // The console sign-on activation post: a router element with the
    // configured routing key.
    bool postConsoleSignOn(int unitBlock, const std::string& call);
    // Monitor experiment: synthesise the interactive job's control block the
    // way the sign-on module would, and link it into the console's unit
    // block.  Not on any default path.
    bool synthesizeJcb(int tubAddress, const std::string& call);
    static constexpr int kConsoleCommandStatementCode = 0x1B;
    bool postConsoleCommandStatement(int tubAddress, const std::string& call);
    bool postConsoleSignOnStatement(int tubAddress, const std::string& call);
    bool postConsoleSignOnRequest(int tubAddress, const std::string& call);
    bool postConsoleDeviceAttach(int tubAddress, const std::string& call);
    bool postConsoleDbElement(int tubAddress, const std::string& call);
    // A multiple-wait element on a task's complete queue with a 3-byte
    // routing key in ace+29, the field the command router loads on wake.
    bool postRouterElement(int taskId, int routingKey, int unitBlock, const std::string& call);
    // Monitor experiments reproducing the guest-visible half of a native
    // SSP call request and the console-message request that precedes one.
    bool postNativeSspCall(const std::string& program, int requestBlock, const std::string& call, int& cf);
    bool postNativeS36ConsoleCall(uint16_t messageId, uint8_t line410, uint8_t line408, uint8_t tag0, uint8_t tag1,
                                  const std::string& call, int& request, int& cf);
    bool wakeJobTaskSelfEvent(int taskId, const std::string& call);
    bool postTypedEventToTask(int taskBlock, uint16_t eventType, int unitBlock, const std::string& call);
    int installMessageSessionOc(int key, const std::string& call);
    void resetWorkStationDeviceStatus();
    // The console unit block: the pointer the IPL publishes at 0x092A, or
    // the console queue head as a fallback.
    int publishedConsoleUnitBlock();
    // Diagnostic message injector for the command router's type dispatch.
    bool postConsoleMessage(int taskBlock, int type, int p1, int p2, const std::string& call);

private:
    bool postRetainedWorkStationCompletion(int iob, const devices::DeviceSet::DeferredWorkStationInput* deferred,
                                           const std::string& call);
    void completeM36WorkStationTransfer(int tub, bool autoSignOn, const std::string& call);
    bool tryDeliverAction0ActivationStatus(const std::string& call, bool resumeFromMonitor);
    bool postPowerOnInternalCondition(const std::string& call, bool resumeFromMonitor);
    int transferredOrRequestedTub(int requested);
    void jcbCopyEndingAt(int jcb, int dst, int tub, int src, int len);
    void experimentLinkJcbIntoTasks(int jcb, const std::string& call);
    // Deliver a held Read Input Fields result onto the resident frame of the
    // work-space block a map has just named.
    void deliverDeferredWorkStationInput(int taskBlock, int block, uint8_t type);

    static constexpr int kConsoleOwnerTaskId = 0x0009;
    static constexpr uint16_t kWorkStationDeviceStatusEvent = 0x002D;
    static constexpr int kConsoleActiveSessionField = 0x8F;

    // The retained device elements: a Read Input Fields result held for
    // re-delivery onto the mapped block's own resident frame, keyed by the
    // read's owner task.
    std::map<int, devices::DeviceSet::DeferredWorkStationInput> deferredWsInput_;
    int transferredWorkStationTub_ = 0;
    bool transferredAutoSignOn_ = false;
    // Every unit that has been transferred, with its auto-sign-on choice: the
    // scalar above is only "most recently bound".
    std::map<int, bool> transferredWorkStations_;
    int pendingTransferredWorkStationTub_ = 0;
    bool pendingTransferAutoSignOn_ = false;
    bool lastM36WorkStationTransferPostedGuestWork_ = false;
    int pendingCnfwsPowerOnTub_ = 0;
    // Retained for checkpoint compatibility; new runs leave them zero.
    int wsDeviceStatusPending_ = 0;
    int wsDeviceStatusDelivered_ = 0;
    std::map<int, int> wsPresentByTask_;
