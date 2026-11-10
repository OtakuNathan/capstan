---------------------------- MODULE DurableStore ----------------------------
EXTENDS Naturals
CONSTANTS Nodes, InitialImage, EmptyBuffer
VARIABLES disk, buffered, running
vars == <<disk, buffered, running>>
Init == /\ disk=InitialImage
        /\ buffered=[n \in Nodes |-> EmptyBuffer]
        /\ running=[n \in Nodes |-> TRUE]
(* Stage is process-local preparation, not a successful database commit. *)
Stage(n,image) == /\ running[n] /\ buffered[n]=EmptyBuffer
                  /\ buffered'=[buffered EXCEPT ![n]=image]
                  /\ UNCHANGED <<disk,running>>
(* Refinement boundary: a FULL-synchronous SQLite transaction commits the
   complete node image atomically. No ACK or gate publication occurs here. *)
Sync(n) == /\ running[n] /\ buffered[n]#EmptyBuffer
           /\ disk'=[disk EXCEPT ![n]=buffered[n]]
           /\ buffered'=[buffered EXCEPT ![n]=EmptyBuffer]
           /\ UNCHANGED running
(* A local process crash, or a machine-wide power loss when victims=Nodes.
   The fault model retains flushed storage and discards all unflushed state. *)
Crash(victims) == /\ victims \subseteq Nodes /\ victims#{}
                 /\ buffered'=[n \in Nodes |-> IF n \in victims THEN EmptyBuffer ELSE buffered[n]]
                 /\ running'=[n \in Nodes |-> IF n \in victims THEN FALSE ELSE running[n]]
                 /\ UNCHANGED disk
Restart(n) == /\ ~running[n]
              /\ running'=[running EXCEPT ![n]=TRUE]
              /\ UNCHANGED <<disk,buffered>>
=============================================================================
