---------------------------- MODULE LeaseCopyCut ----------------------------
EXTENDS Naturals, FiniteSets, LeasePermission
CONSTANTS MaxReaders, MaxPins, MaxVersion,
          RequireReadExclusion, KeepBorrowOnDisconnect, RequireExclusiveCut,
          RequireCutNoReaders, RequireDrain, RequirePreparedAck, RequireFenceAck,
          KeepCommitOnDisconnect, FreshAbortLease
VARIABLES lease, host, copy, cut, net, version
vars == <<lease,host,copy,cut,net,version>>

(* One source permission lineage, one Copy and one Cut. Snapshot clone identity
   is checked separately by Clipboard. Host pins are real previously accepted
   backend uses, NOT participants in this process-local Lease counter. Ending a
   lineage here means ending its usable authority, not necessarily freeing its
   C++ control block. Target/Abort grants create NEW lineages. *)
Init == \E mode \in {"rw","ro"}, initialPins \in 0..MaxPins:
    /\ lease=[alive |-> TRUE, mode |-> mode, initialMode |-> mode,
               readers |-> 0, count |-> 1, writing |-> FALSE,
               custody |-> "Client", sealed |-> FALSE]
    /\ host=[source |-> "Open", target |-> "Empty", pins |-> initialPins,
             sourceData |-> TRUE, sourceEpoch |-> 1, restoredRW |-> FALSE,
             targetRW |-> FALSE, targetVersion |-> 0]
    /\ copy=[phase |-> "Idle", borrow |-> FALSE, beginVersion |-> 0,
             saved |-> MaxVersion+1]
    /\ cut=[phase |-> "Idle", decision |-> "Undecided", committedOnce |-> FALSE,
            preparedAck |-> FALSE, fenceAck |-> FALSE, activationAck |-> FALSE,
            abortAck |-> FALSE]
    /\ net=[connected |-> TRUE, unknown |-> FALSE]
    /\ version=0

BorrowReader ==
    /\ lease.custody="Client" /\ host.source="Open"
    /\ MayRead(lease.alive,lease.mode,lease.writing)
    /\ lease.readers<MaxReaders
    /\ lease'=[lease EXCEPT !.readers=@+1, !.count=@+1]
    /\ UNCHANGED <<host,copy,cut,net,version>>
ReleaseReader ==
    /\ ~lease.writing /\ lease.readers>BoolToNat(copy.borrow)
    /\ lease'=[lease EXCEPT !.readers=@-1, !.count=@-1]
    /\ UNCHANGED <<host,copy,cut,net,version>>
BeginWrite ==
    /\ lease.custody="Client" /\ host.source="Open" /\ host.pins=0
    /\ version<MaxVersion
    /\ IF RequireReadExclusion
          THEN MayWrite(lease.alive,lease.mode,lease.readers,lease.writing)
          ELSE lease.alive /\ lease.mode="rw" /\ ~lease.writing
    /\ lease'=[lease EXCEPT !.writing=TRUE]
    /\ UNCHANGED <<host,copy,cut,net,version>>
EndWrite ==
    /\ lease.writing /\ version'=version+1
    /\ lease'=[lease EXCEPT !.writing=FALSE]
    /\ UNCHANGED <<host,copy,cut,net>>
CompletePin ==
    /\ host.pins>0
    /\ host'=[host EXCEPT !.pins=@-1]
    /\ version'=IF lease.initialMode="rw" /\ version<MaxVersion THEN version+1 ELSE version
    /\ UNCHANGED <<lease,copy,cut,net>>

(* A Copy borrows RO from either an RW root or a shared RO root. Actual capture
   starts only when earlier backend work has left the source lane. The borrow
   remains live until the capture backend stops, even when its observer leaves. *)
RequestCopy ==
    /\ net.connected /\ copy.phase="Idle" /\ lease.custody="Client"
    /\ host.source="Open" /\ MayRead(lease.alive,lease.mode,lease.writing)
    /\ lease.readers<MaxReaders
    /\ lease'=[lease EXCEPT !.readers=@+1, !.count=@+1]
    /\ copy'=[copy EXCEPT !.phase="Queued", !.borrow=TRUE]
    /\ UNCHANGED <<host,cut,net,version>>
StartCapture ==
    /\ copy.phase="Queued" /\ host.pins=0 /\ host.source="Open"
    /\ copy'=[copy EXCEPT !.phase="Capturing", !.beginVersion=version]
    /\ UNCHANGED <<lease,host,cut,net,version>>
FinishCapture ==
    /\ copy.phase="Capturing"
    /\ lease'=[lease EXCEPT !.readers=@-BoolToNat(copy.borrow), !.count=@-BoolToNat(copy.borrow)]
    /\ copy'=[copy EXCEPT !.phase="Done", !.saved=version, !.borrow=FALSE]
    /\ UNCHANGED <<host,cut,net,version>>

(* Selecting Cut is inert. Submission seals the original client facade and
   transfers custody of its unique authority without duplicating the root.
   Closing the local facade does not finish remote backend pins. *)
SelectCut ==
    /\ cut.phase="Idle" /\ lease.custody="Client"
    /\ lease.mode="rw" \/ ~RequireExclusiveCut
    /\ cut'=[cut EXCEPT !.phase="Selected"]
    /\ UNCHANGED <<lease,host,copy,net,version>>
SubmitCut ==
    /\ net.connected /\ cut.phase="Selected" /\ host.source="Open"
    /\ ~lease.writing /\ lease.custody="Client"
    /\ lease.mode="rw" \/ ~RequireExclusiveCut
    /\ lease.readers=0 \/ ~RequireCutNoReaders
    /\ lease'=[lease EXCEPT !.custody="Escrow", !.sealed=TRUE]
    /\ host'=[host EXCEPT !.source="Closing"]
    /\ cut'=[cut EXCEPT !.phase="Started"]
    /\ UNCHANGED <<copy,net,version>>
Freeze ==
    /\ host.source="Closing" /\ cut.decision="Undecided"
    /\ host.pins=0 \/ ~RequireDrain
    /\ host'=[host EXCEPT !.source="Frozen"]
    /\ UNCHANGED <<lease,copy,cut,net,version>>
PrepareTarget ==
    /\ net.connected /\ host.source="Frozen" /\ host.target="Empty"
    /\ cut.decision="Undecided"
    /\ host'=[host EXCEPT !.target="Prepared", !.targetVersion=version]
    /\ UNCHANGED <<lease,copy,cut,net,version>>
ObservePrepared ==
    /\ net.connected /\ host.target="Prepared" /\ cut.decision="Undecided"
    /\ cut'=[cut EXCEPT !.preparedAck=TRUE]
    /\ UNCHANGED <<lease,host,copy,net,version>>
Commit ==
    /\ cut.decision="Undecided" /\ host.source="Frozen"
    /\ cut.preparedAck \/ ~RequirePreparedAck
    /\ cut'=[cut EXCEPT !.decision="Commit", !.committedOnce=TRUE]
    /\ UNCHANGED <<lease,host,copy,net,version>>
Fence ==
    /\ net.connected /\ cut.decision="Commit" /\ host.source="Frozen"
    /\ host.pins=0 /\ lease.readers=0 /\ ~lease.writing
    /\ lease'=[lease EXCEPT !.alive=FALSE, !.mode="none", !.count=@-1, !.custody="Spent"]
    /\ host'=[host EXCEPT !.source="Fenced"]
    /\ UNCHANGED <<copy,cut,net,version>>
ObserveFence ==
    /\ net.connected /\ host.source="Fenced" /\ cut.decision="Commit"
    /\ cut'=[cut EXCEPT !.fenceAck=TRUE]
    /\ UNCHANGED <<lease,host,copy,net,version>>
Activate ==
    /\ net.connected /\ cut.decision="Commit" /\ host.target="Prepared"
    /\ cut.fenceAck \/ ~RequireFenceAck
    /\ host'=[host EXCEPT !.target="Open", !.targetRW=TRUE]
    /\ UNCHANGED <<lease,copy,cut,net,version>>
ObserveActivation ==
    /\ net.connected /\ host.target="Open" /\ cut.decision="Commit"
    /\ cut'=[cut EXCEPT !.activationAck=TRUE]
    /\ net'=[net EXCEPT !.unknown=FALSE]
    /\ UNCHANGED <<lease,host,copy,version>>
RetireSource ==
    /\ cut.activationAck /\ cut.decision="Commit" /\ host.source="Fenced"
    /\ host'=[host EXCEPT !.source="Retired", !.sourceData=FALSE]
    /\ cut'=[cut EXCEPT !.phase="Done"]
    /\ UNCHANGED <<lease,copy,net,version>>

Abort ==
    /\ cut.phase="Started" /\ cut.decision="Undecided"
    /\ cut'=[cut EXCEPT !.decision="Abort"]
    /\ UNCHANGED <<lease,host,copy,net,version>>
AbortTarget ==
    /\ net.connected /\ cut.decision="Abort" /\ host.target \in {"Empty","Prepared"}
    /\ host'=[host EXCEPT !.target="Aborted"]
    /\ UNCHANGED <<lease,copy,cut,net,version>>
ObserveAbort ==
    /\ net.connected /\ cut.decision="Abort" /\ host.target="Aborted"
    /\ cut'=[cut EXCEPT !.abortAck=TRUE]
    /\ UNCHANGED <<lease,host,copy,net,version>>
RestoreSource ==
    /\ net.connected /\ cut.decision="Abort" /\ cut.abortAck
    /\ host.source \in {"Closing","Frozen"} /\ host.pins=0
    /\ lease.readers=0 /\ ~lease.writing
    /\ lease'=IF FreshAbortLease
                THEN [lease EXCEPT !.alive=FALSE, !.mode="none", !.count=@-1, !.custody="Spent"]
                ELSE [lease EXCEPT !.custody="Client"]
    /\ host'=[host EXCEPT !.source="Open", !.sourceEpoch=2, !.restoredRW=TRUE]
    /\ cut'=[cut EXCEPT !.phase="Done"]
    /\ UNCHANGED <<copy,net,version>>

Disconnect ==
    /\ net.connected
    /\ net'=[net EXCEPT !.connected=FALSE, !.unknown=TRUE]
    /\ lease'=IF ~KeepBorrowOnDisconnect /\ copy.borrow
                THEN [lease EXCEPT !.readers=@-1, !.count=@-1] ELSE lease
    /\ copy'=IF ~KeepBorrowOnDisconnect THEN [copy EXCEPT !.borrow=FALSE] ELSE copy
    /\ cut'=IF ~KeepCommitOnDisconnect /\ cut.decision="Commit"
              THEN [cut EXCEPT !.decision="Abort"] ELSE cut
    /\ UNCHANGED <<host,version>>
Reconnect ==
    /\ ~net.connected /\ net'=[net EXCEPT !.connected=TRUE]
    /\ UNCHANGED <<lease,host,copy,cut,version>>

Next == BorrowReader \/ ReleaseReader \/ BeginWrite \/ EndWrite \/ CompletePin
        \/ RequestCopy \/ StartCapture \/ FinishCapture \/ SelectCut \/ SubmitCut
        \/ Freeze \/ PrepareTarget \/ ObservePrepared \/ Commit \/ Fence \/ ObserveFence
        \/ Activate \/ ObserveActivation \/ RetireSource \/ Abort \/ AbortTarget
        \/ ObserveAbort \/ RestoreSource \/ Disconnect \/ Reconnect
Spec == Init /\ [][Next]_vars

TypeOK == /\ lease.mode \in Modes /\ lease.initialMode \in {"rw","ro"}
          /\ lease.custody \in {"Client","Escrow","Spent"}
          /\ lease.alive \in BOOLEAN /\ lease.sealed \in BOOLEAN /\ lease.writing \in BOOLEAN
          /\ lease.readers \in 0..MaxReaders /\ lease.count \in 0..(MaxReaders+1)
          /\ host.pins \in 0..MaxPins /\ host.sourceEpoch \in {1,2}
          /\ host.source \in {"Open","Closing","Frozen","Fenced","Retired"}
          /\ host.target \in {"Empty","Prepared","Open","Aborted"}
          /\ host.sourceData \in BOOLEAN /\ host.restoredRW \in BOOLEAN /\ host.targetRW \in BOOLEAN
          /\ copy.phase \in {"Idle","Queued","Capturing","Done"} /\ copy.borrow \in BOOLEAN
          /\ copy.beginVersion \in 0..MaxVersion /\ copy.saved \in 0..(MaxVersion+1)
          /\ cut.phase \in {"Idle","Selected","Started","Done"}
          /\ cut.decision \in {"Undecided","Commit","Abort"}
          /\ cut.committedOnce \in BOOLEAN /\ cut.preparedAck \in BOOLEAN
          /\ cut.fenceAck \in BOOLEAN /\ cut.activationAck \in BOOLEAN /\ cut.abortAck \in BOOLEAN
          /\ net.connected \in BOOLEAN /\ net.unknown \in BOOLEAN
          /\ version \in 0..MaxVersion /\ host.targetVersion \in 0..MaxVersion
CountRefinement == CountMatches(lease.alive,lease.readers,lease.count)
WriteExclusion == lease.writing => lease.mode="rw" /\ lease.count=1 /\ lease.readers=0
CopyBorrowHeld == copy.phase \in {"Queued","Capturing"} => copy.borrow /\ lease.readers>0
CopyStable == copy.phase="Capturing" => version=copy.beginVersion
CopyTimeValue == copy.phase="Done" => copy.saved=copy.beginVersion
ReadOnlyNeverPromotes == lease.initialMode="ro" => lease.mode#"rw" /\ ~host.restoredRW /\ ~host.targetRW
CutRequiresWriteAuthority == lease.sealed => lease.initialMode="rw"
CutHasNoReaders == lease.sealed => lease.readers=0
SealedNeverReopens == lease.sealed => lease.custody#"Client"
FrozenHasNoPins == host.source \in {"Frozen","Fenced","Retired"} => host.pins=0
SingleWriteAuthority == BoolToNat(lease.alive /\ lease.mode="rw") + BoolToNat(host.restoredRW) + BoolToNat(host.targetRW) <= 1
CommitHasPreparedAck == cut.decision="Commit" => cut.preparedAck /\ host.target \in {"Prepared","Open"}
CommitIsStable == cut.committedOnce => cut.decision="Commit"
TargetAuthorityCommitted == host.targetRW => cut.decision="Commit" /\ host.source \in {"Fenced","Retired"}
AbortRestoresFreshAuthority == host.restoredRW => host.sourceEpoch=2 /\ host.target="Aborted" /\ cut.abortAck /\ host.pins=0
RetireAfterActivationAck == ~host.sourceData => cut.activationAck /\ host.target="Open"

(* Intentional false invariants used ONLY as reachability witnesses. They keep
   the safe checks from passing vacuously by disabling the relevant paths. *)
NoSharedCopyCompletion == ~(lease.initialMode="ro" /\ copy.phase="Done")
NoCommittedLostAck == ~(host.target="Open" /\ cut.decision="Commit" /\ ~cut.activationAck /\ net.unknown /\ ~net.connected)
NoFreshAbortCompletion == ~host.restoredRW
=============================================================================
