    // ---- native state with no guest-storage representation -------------------
    //
    // Ordinary loader, heap and translation-file residency is rebuilt from
    // the guest image; these are the controller and dispatcher latches that
    // cannot be.  The struct is the checkpoint file's record, field for
    // field, so its shape does not change without the file version.
public:
    struct CheckpointState {
        std::vector<int> pendingDeviceAcePairs;
        std::vector<int> pendingAsyncChildren;
        std::vector<int> nativeTransferContinuations;
        bool redispatch = false;
        int phase2SvatJobTask = 0;
        bool phase2SvatDispatched = false;
        int taskIdCounter = 0, requestBlockBeforeAttach = 0;
        bool systemMeasurementEnabled = false;
        int transferredTub = 0, pendingTransferredTub = 0;
        bool transferredAutoSignOn = false, pendingTransferAutoSignOn = false;
        bool lastTransferPostedGuestWork = false;
        int wsDeviceStatusPending = 0, wsDeviceStatusDelivered = 0;
        // The SVC 0B action-controller status words and the per-mask
        // coverage counters, 7 ints per mask.
        uint64_t actionStatusLow = 0, actionStatusHigh = 0;
        std::vector<int> actionCoverage;
        // Experiment counters the reference records; SIM/36 carries no
        // experiment, so they are always zero here and keep the file shape.
        int sessionContentPopulated = 0, clssMapGateSet = 0, wddqWriteForced = 0;
        int expAutoMapped = 0, expGrounded = 0;
        std::vector<int> wsPresentPairs;
        std::vector<int> directAreaWords;
        std::vector<int> taskWorkAreaFree;
        std::vector<int> workSpaces;
        std::vector<int> heap;
        std::vector<int> ptt;
        int moduleStorageNext = 0;
        int currentTransientProgramBlock = 0;
        std::vector<int> moduleStoragePairs, workSpaceStoragePairs, workSpaceStoragePages;
        std::vector<int> moduleStorageSizePairs;
        std::vector<int> moduleStorageFree;
        std::vector<int> loadedMemberData;
        std::vector<std::string> loadedMemberNames;
        ActionControlElementQueue::CheckpointState aces;
    };

    // Capture the native state.  False with a reason when a checkpoint is
    // not possible here (inside the native dispatcher).
    bool captureCheckpoint(CheckpointState& out, std::string& failure);
    // Restore it.  False with a reason on a malformed record; the state is
    // then partially restored, as the reference's exception left it.
    bool restoreCheckpoint(const CheckpointState& s, std::string& failure);
    // Restore the volatile memory of a native checkpoint.  An early IPL
    // checkpoint can predate the guest's own task structures, so the native
    // execution owner is carried explicitly and no guest structure is
    // invented.
    bool restoreCheckpointMemory(const std::vector<uint8_t>& main, int currentTask, int currentRequest,
                                 std::string& failure);

private:
    std::vector<int> captureActionCoverage() const;
    void restoreActionCoverage(const std::vector<int>& v);
    static std::vector<int> capturePairs(const std::map<int, int>& from);
    static bool restorePairs(std::map<int, int>& into, const std::vector<int>& pairs, const std::string& what,
                             std::string& failure);
    static std::vector<int> capturePageMaps(const std::map<int, std::vector<int>>& from);
    static bool restorePageMaps(std::map<int, std::vector<int>>& into, const std::vector<int>& data,
                                const std::string& what, std::string& failure);
    static std::vector<int> captureRanges(const std::vector<std::pair<int, int>>& from);
