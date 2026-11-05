------------------------ MODULE ImportReservation ------------------------
EXTENDS Naturals, TLC
CONSTANTS CloseOnReserve, KeepTombstone
VARIABLES target, gate, writer, unknown
vars == <<target,gate,writer,unknown>>
Init == /\ target="Empty" /\ gate="Open" /\ writer=FALSE /\ unknown=FALSE
Reserve == /\ target="Empty" /\ ~writer
           /\ target'="Receiving" /\ gate'=IF CloseOnReserve THEN "Closed" ELSE gate
           /\ UNCHANGED <<writer,unknown>>
Grant == /\ gate="Open" /\ target#"Active" /\ writer'=TRUE
         /\ UNCHANGED <<target,gate,unknown>>
Seal == /\ target="Receiving" /\ target'="Prepared"
        /\ UNCHANGED <<gate,writer,unknown>>
Activate == /\ target="Prepared" /\ target'="Active" /\ gate'="Open"
            /\ UNCHANGED <<writer,unknown>>
Abort == /\ target \in {"Empty","Receiving","Prepared"}
         /\ target'="Aborted" /\ gate'="Open"
         /\ UNCHANGED <<writer,unknown>>
LateBegin == /\ target="Aborted" /\ ~KeepTombstone /\ target'="Receiving" /\ gate'="Closed"
             /\ UNCHANGED <<writer,unknown>>
Timeout == /\ ~unknown /\ unknown'=TRUE /\ UNCHANGED <<target,gate,writer>>
Next == Reserve \/ Grant \/ Seal \/ Activate \/ Abort \/ LateBegin \/ Timeout
ReservedGate == target \in {"Receiving","Prepared"} => gate="Closed"
NoCompetingWriter == target \in {"Receiving","Prepared","Active"} => ~writer
AbortTerminal == [](target="Aborted" => [] (target="Aborted"))
Spec == Init /\ [][Next]_vars
=============================================================================
