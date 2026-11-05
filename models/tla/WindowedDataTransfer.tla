----------------------- MODULE WindowedDataTransfer -----------------------
EXTENDS Naturals, FiniteSets, TLC
CONSTANTS MaxChunks, Window, BoundWindow, CheckIdentity, CheckDuplicates,
          RequireComplete, KeepTombstone, OrderedHash
VARIABLES current, phase, received, origins, prefix, corrupt, issued, pending, aborted
Tx == 1..2
Chunks == 1..MaxChunks
vars == <<current,phase,received,origins,prefix,corrupt,issued,pending,aborted>>
Init == /\ current=1 /\ phase=[t \in Tx |-> "Empty"]
        /\ received=[t \in Tx |-> {}] /\ origins=[t \in Tx |-> [c \in Chunks |-> 0]]
        /\ prefix=[t \in Tx |-> 0] /\ corrupt=[t \in Tx |-> {}]
        /\ issued={} /\ pending={} /\ aborted={}
Begin == /\ phase[current]="Empty"
         /\ phase'=[phase EXCEPT ![current]="Receiving"]
         /\ UNCHANGED <<current,received,origins,prefix,corrupt,issued,pending,aborted>>
Issue(c) == /\ phase[current]="Receiving" /\ <<current,c>> \notin issued
            /\ (~BoundWindow \/ Cardinality(pending)<Window)
            /\ issued'=issued \cup {<<current,c>>} /\ pending'=pending \cup {<<current,c>>}
            /\ UNCHANGED <<current,phase,received,origins,prefix,corrupt,aborted>>
Deliver(t,c) ==
    LET destination == IF CheckIdentity THEN t ELSE current IN
    /\ <<t,c>> \in issued
    /\ pending'=pending \ {<<t,c>>}
    /\ IF phase[destination]="Receiving" /\ c \notin received[destination]
          THEN /\ received'=[received EXCEPT ![destination]=@ \cup {c}]
               /\ origins'=[origins EXCEPT ![destination][c]=t]
          ELSE UNCHANGED <<received,origins>>
    /\ UNCHANGED <<current,phase,prefix,corrupt,issued,aborted>>
ConflictingDuplicate(t,c) ==
    /\ phase[t]="Receiving" /\ c \in received[t]
    /\ corrupt'=IF CheckDuplicates THEN corrupt ELSE [corrupt EXCEPT ![t]=@ \cup {c}]
    /\ UNCHANGED <<current,phase,received,origins,prefix,issued,pending,aborted>>
Hash(t) == /\ phase[t]="Receiving" /\ prefix[t]<MaxChunks
           /\ (~OrderedHash \/ prefix[t]+1 \in received[t])
           /\ prefix'=[prefix EXCEPT ![t]=@+1]
           /\ UNCHANGED <<current,phase,received,origins,corrupt,issued,pending,aborted>>
Seal(t) == /\ phase[t]="Receiving"
           /\ (~RequireComplete \/ (received[t]=Chunks /\ prefix[t]=MaxChunks /\ corrupt[t]={}))
           /\ phase'=[phase EXCEPT ![t]="DataReady"]
           /\ UNCHANGED <<current,received,origins,prefix,corrupt,issued,pending,aborted>>
Abort(t) == /\ phase[t]#"Aborted" /\ phase'=[phase EXCEPT ![t]="Aborted"]
            /\ aborted'=aborted \cup {t}
            /\ UNCHANGED <<current,received,origins,prefix,corrupt,issued,pending>>
NextBinding == /\ current=1 /\ phase[1] \in {"Aborted","DataReady"} /\ current'=2
               /\ UNCHANGED <<phase,received,origins,prefix,corrupt,issued,pending,aborted>>
LateBegin(t) == /\ t \in aborted /\ ~KeepTombstone
                /\ phase'=[phase EXCEPT ![t]="Receiving"]
                /\ UNCHANGED <<current,received,origins,prefix,corrupt,issued,pending,aborted>>
Next == Begin \/ NextBinding \/ (\E c \in Chunks: Issue(c)) \/
        (\E t \in Tx: Hash(t) \/ Seal(t) \/ Abort(t) \/ LateBegin(t) \/
          (\E c \in Chunks: Deliver(t,c) \/ ConflictingDuplicate(t,c)))
TypeOK == /\ current \in Tx /\ phase \in [Tx -> {"Empty","Receiving","DataReady","Aborted"}]
          /\ received \in [Tx -> SUBSET Chunks] /\ prefix \in [Tx -> 0..MaxChunks]
          /\ pending \subseteq issued /\ issued \subseteq Tx \X Chunks /\ aborted \subseteq Tx
WindowBound == Cardinality(pending)<=Window
BindingIsolation == \A t \in Tx: \A c \in received[t]: origins[t][c]=t
DuplicatesPreserved == \A t \in Tx: corrupt[t]={}
HashedPrefix == \A t \in Tx: 1..prefix[t] \subseteq received[t]
ReadyComplete == \A t \in Tx: phase[t]="DataReady" => (received[t]=Chunks /\ prefix[t]=MaxChunks /\ corrupt[t]={})
AbortTerminal == \A t \in aborted: phase[t]="Aborted"
NoReordering == \A t \in Tx: received[t]#{} => 1 \in received[t]
Spec == Init /\ [][Next]_vars
=============================================================================
