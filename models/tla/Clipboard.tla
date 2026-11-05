----------------------------- MODULE Clipboard -----------------------------
EXTENDS Naturals, FiniteSets, TLC
(* Copy is atomic in this value/clone projection. LeaseCopyCut separately checks
   the RO borrow and multi-step capture against writes and observer disconnect. *)
CONSTANTS FreshIdentity, UseSavedSnapshot, KeepCloneAbort
VARIABLES sourceVersion, saved, phase, values, identities, aborted
Tx == {1,2}
vars == <<sourceVersion,saved,phase,values,identities,aborted>>
Init == /\ sourceVersion=0 /\ saved=3
        /\ phase=[t \in Tx |-> "Empty"] /\ values=[t \in Tx |-> 3]
        /\ identities=[t \in Tx |-> 0] /\ aborted={}
WriteSource == /\ sourceVersion<2 /\ sourceVersion'=sourceVersion+1
               /\ UNCHANGED <<saved,phase,values,identities,aborted>>
Copy == /\ saved=3 /\ saved'=sourceVersion
        /\ UNCHANGED <<sourceVersion,phase,values,identities,aborted>>
Paste(t) == /\ saved#3 /\ (phase[t]="Empty" \/ (~KeepCloneAbort /\ phase[t]="Aborted"))
            /\ phase'=[phase EXCEPT ![t]="Installed"]
            /\ values'=[values EXCEPT ![t]=IF UseSavedSnapshot THEN saved ELSE sourceVersion]
            /\ identities'=[identities EXCEPT ![t]=IF FreshIdentity THEN t ELSE 0]
            /\ UNCHANGED <<sourceVersion,saved,aborted>>
Abort(t) == /\ phase[t]#"Installed" /\ phase'=[phase EXCEPT ![t]="Aborted"]
            /\ aborted'=aborted \cup {t}
            /\ UNCHANGED <<sourceVersion,saved,values,identities>>
Next == Copy \/ WriteSource \/ (\E t \in Tx: Paste(t) \/ Abort(t))
CopyTimeValue == \A t \in Tx: phase[t]="Installed" => values[t]=saved
CloneIdentity == \A t \in Tx: phase[t]="Installed" => identities[t]=t
AbortTerminal == \A t \in aborted: phase[t]="Aborted"
Spec == Init /\ [][Next]_vars
=============================================================================
