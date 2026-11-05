-------------------------- MODULE ObjectMigration --------------------------
EXTENDS Naturals, TLC
(* This backend/decision projection retains capture pins. LeaseCopyCut adds the
   local RO/RW authority lineage, facade sealing and explicit ACK observations. *)
CONSTANTS MaxPins, RequireDrain, RequireFence, AllowRollback, TimeoutReopens, KeepDedup, RequireCaptureRelease
VARIABLES source, target, pins, decision, history, imported, executed, saved, unknown, capturing
vars == <<source,target,pins,decision,history,imported,executed,saved,unknown,capturing>>
Init == /\ source="Open" /\ target="Empty" /\ pins=MaxPins
        /\ decision="Undecided" /\ history=FALSE /\ imported=FALSE
        /\ executed=FALSE /\ saved=FALSE /\ unknown=FALSE /\ capturing=FALSE
Close == /\ source="Open" /\ decision="Undecided" /\ source'="Closing"
         /\ UNCHANGED <<target,pins,decision,history,imported,executed,saved,unknown,capturing>>
Finish == /\ pins>0 /\ pins'=pins-1 /\ history'=TRUE /\ executed'=TRUE
          /\ UNCHANGED <<source,target,decision,imported,saved,unknown,capturing>>
Freeze == /\ source="Closing" /\ (~RequireDrain \/ pins=0) /\ source'="Frozen"
          /\ saved'=executed
          /\ UNCHANGED <<target,pins,decision,history,imported,executed,unknown,capturing>>
Prepare == /\ source="Frozen" /\ target="Empty" /\ decision="Undecided"
           /\ target'="Prepared" /\ imported'=IF KeepDedup THEN history ELSE FALSE
           /\ UNCHANGED <<source,pins,decision,history,executed,saved,unknown,capturing>>
Commit == /\ decision="Undecided" /\ target="Prepared" /\ ~capturing /\ decision'="Commit"
          /\ UNCHANGED <<source,target,pins,history,imported,executed,saved,unknown,capturing>>
Fence == /\ decision="Commit" /\ source="Frozen" /\ ~capturing /\ source'="Fenced"
         /\ UNCHANGED <<target,pins,decision,history,imported,executed,saved,unknown,capturing>>
Activate == /\ decision="Commit" /\ target="Prepared"
            /\ (~RequireFence \/ source="Fenced") /\ target'="Open"
            /\ UNCHANGED <<source,pins,decision,history,imported,executed,saved,unknown,capturing>>
Abort == /\ (decision="Undecided" \/ (AllowRollback /\ decision="Commit"))
         /\ decision'="Abort"
         /\ UNCHANGED <<source,target,pins,history,imported,executed,saved,unknown,capturing>>
AbortTarget == /\ decision="Abort" /\ target#"Open" /\ target'="Aborted"
               /\ UNCHANGED <<source,pins,decision,history,imported,executed,saved,unknown,capturing>>
Restore == /\ decision="Abort" /\ target="Aborted" /\ pins=0 /\ (~RequireCaptureRelease \/ ~capturing) /\ source'="Open"
           /\ UNCHANGED <<target,pins,decision,history,imported,executed,saved,unknown,capturing>>
Timeout == /\ source \in {"Frozen","Fenced"} /\ unknown'=TRUE
           /\ source'=IF TimeoutReopens THEN "Open" ELSE source
           /\ UNCHANGED <<target,pins,decision,history,imported,executed,saved,capturing>>
Capture == /\ source="Frozen" /\ decision="Undecided" /\ ~capturing
           /\ capturing'=TRUE
           /\ UNCHANGED <<source,target,pins,decision,history,imported,executed,saved,unknown>>
CaptureDone == /\ capturing /\ capturing'=FALSE
               /\ UNCHANGED <<source,target,pins,decision,history,imported,executed,saved,unknown>>
Next == Capture \/ CaptureDone \/ Close \/ Finish \/ Freeze \/ Prepare \/ Commit \/ Fence \/ Activate \/ Abort \/ AbortTarget \/ Restore \/ Timeout
TypeOK == /\ source \in {"Open","Closing","Frozen","Fenced"}
          /\ target \in {"Empty","Prepared","Open","Aborted"}
          /\ capturing \in BOOLEAN /\ pins \in 0..MaxPins /\ decision \in {"Undecided","Commit","Abort"}
DrainBeforeFreeze == source \in {"Frozen","Fenced"} => pins=0
ActivationFenced == target="Open" => source="Fenced"
ActivationCommitted == target="Open" => decision="Commit"
DedupPreserved == target="Open" => (saved => imported)
CommittedSourceClosed == decision="Commit" => source#"Open"
CaptureProtected == capturing => source#"Open"
Spec == Init /\ [][Next]_vars
=============================================================================
