-------------------------- MODULE DataTransfer --------------------------
EXTENDS Naturals, FiniteSets, TLC
CONSTANTS MaxChunks, RequireComplete, KeepTombstone
VARIABLES phase, received, valid
vars == <<phase,received,valid>>
Init == /\ phase="Empty" /\ received={} /\ valid=FALSE
Begin == /\ phase="Empty" /\ phase'="Receiving"
         /\ UNCHANGED <<received,valid>>
Chunk == /\ phase="Receiving"
         /\ \E c \in 1..MaxChunks: received'=received \cup {c}
         /\ UNCHANGED <<phase,valid>>
Validate == /\ phase="Receiving" /\ received=1..MaxChunks
            /\ valid'=TRUE /\ UNCHANGED <<phase,received>>
Seal == /\ phase="Receiving"
        /\ (~RequireComplete \/ (received=1..MaxChunks /\ valid))
        /\ phase'="Prepared" /\ UNCHANGED <<received,valid>>
Abort == /\ phase#"Aborted" /\ phase'="Aborted" /\ UNCHANGED <<received,valid>>
LateBegin == /\ phase="Aborted" /\ ~KeepTombstone /\ phase'="Prepared"
             /\ UNCHANGED <<received,valid>>
Next == Begin \/ Chunk \/ Validate \/ Seal \/ Abort \/ LateBegin
TypeOK == /\ phase \in {"Empty","Receiving","Prepared","Aborted"}
          /\ received \subseteq 1..MaxChunks /\ valid \in BOOLEAN
PreparedComplete == phase="Prepared" => (received=1..MaxChunks /\ valid)
AbortStable == phase="Aborted" => phase'="Aborted"
Spec == Init /\ [][Next]_vars
AbortNeverReopens == [][AbortStable]_vars
=============================================================================
