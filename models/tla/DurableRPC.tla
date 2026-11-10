----------------------------- MODULE DurableRPC -----------------------------
EXTENDS Naturals, Integers, Sequences, FiniteSets, TLC
CONSTANTS MaxRequests, MaxCrashes, RequireDurableAck, KeepReceipt,
          CheckDurableDedup, BindPayload, PersistIntent, RequireDrain,
          RecoverClosed, DurablePublication
ASSUME MaxRequests \in {1,2}
Nodes == {"Host","Table"}
H == "Host"
T == "Table"
Ids == 1..MaxRequests
Bodies == {1,2}
NoReceipt == [body |-> 0, result |-> 0]
Initial == [valid |-> TRUE, value |-> 0, receipts |-> [r \in Ids |-> NoReceipt],
            epoch |-> 1, gate |-> "Open", target |-> 0, tx |-> 0,
            decision |-> "None"]
NoBuffer == [Initial EXCEPT !.valid=FALSE]
VARIABLES disk, buffered, running, host, table, commands, replies, acknowledged,
          installAck, published, crashes, lostWrite, recoveredInstall
store == INSTANCE DurableStore WITH Nodes <- Nodes,
          InitialImage <- [n \in Nodes |-> Initial], EmptyBuffer <- NoBuffer
vars == <<disk,buffered,running,host,table,commands,replies,acknowledged,
          installAck,published,crashes,lostWrite,recoveredInstall>>
EmptyRequest == [id |-> 0, body |-> 0]
EmptyHost == [mode |-> "Open", queue |-> <<>>, job |-> "Idle",
              request |-> EmptyRequest, cache |-> {}, writeReady |-> FALSE, result |-> 0]
Init == /\ store!Init /\ host=EmptyHost /\ table="Idle"
        /\ commands=FALSE /\ replies={} /\ acknowledged={} /\ installAck=FALSE
        /\ published=FALSE /\ crashes=0 /\ lostWrite=FALSE /\ recoveredInstall=FALSE
QueuedIds == {host.queue[i].id : i \in 1..Len(host.queue)}
HasReceipt(r) == disk[H].receipts[r].body#0
ReceiptMatches(r,b) == disk[H].receipts[r].body=b
ReplyMatches(reply) == ReceiptMatches(reply[1],reply[2]) /\ disk[H].receipts[reply[1]].result=reply[3]
BufferedReceipt(r,b) == IF buffered[H]=NoBuffer THEN FALSE ELSE buffered[H].receipts[r].body=b
HostFree == running[H] /\ host.job="Idle" /\ buffered[H]=NoBuffer
TableFree == running[T] /\ buffered[T]=NoBuffer

(* Requests use a stable authenticated identity; two bodies deliberately share
   each ID to exercise conflicting-payload rejection, including after restart. *)
Admit(r,b) ==
    /\ running[H] /\ host.mode="Open" /\ disk[H].epoch=1
    /\ r \notin QueuedIds /\ Len(host.queue)<MaxRequests
    /\ IF CheckDurableDedup THEN ~HasReceipt(r) ELSE r \notin host.cache
    /\ host'=[host EXCEPT !.queue=Append(@,[id |-> r, body |-> b])]
    /\ UNCHANGED <<disk,buffered,running,table,commands,replies,acknowledged,
                   installAck,published,crashes,lostWrite,recoveredInstall>>
PrepareWrite ==
    /\ HostFree /\ ~host.writeReady /\ host.mode \in {"Open","Closing"} /\ Len(host.queue)>0
    /\ LET req == Head(host.queue)
           receipt == [body |-> req.body,result |-> Cardinality({r \in Ids: HasReceipt(r)})+1]
           image == [disk[H] EXCEPT !.value=@+req.body,
                     !.receipts[req.id]=IF KeepReceipt THEN receipt ELSE @]
       IN /\ store!Stage(H,image)
          /\ host'=[host EXCEPT !.job="Write", !.request=req]
    /\ UNCHANGED <<table,commands,replies,acknowledged,installAck,published,
                   crashes,lostWrite,recoveredInstall>>
ObserveWriteCommit ==
    /\ running[H] /\ host.job="Write"
    /\ LET req == host.request
       IN /\ IF RequireDurableAck THEN ReceiptMatches(req.id,req.body)
              ELSE ReceiptMatches(req.id,req.body) \/ BufferedReceipt(req.id,req.body)
          /\ host'=[host EXCEPT !.job="Idle", !.writeReady=TRUE,
                    !.result=IF ReceiptMatches(req.id,req.body) THEN disk[H].receipts[req.id].result ELSE buffered[H].receipts[req.id].result]
    /\ UNCHANGED <<disk,buffered,running,table,commands,replies,acknowledged,installAck,
                   published,crashes,lostWrite,recoveredInstall>>
(* Backend resource completion precedes the posted owner-loop pin retirement.
   A durable Close can interleave here; Install must still wait for the pin. *)
CompleteWrite ==
    /\ running[H] /\ host.writeReady /\ Len(host.queue)>0
    /\ LET req == host.request
       IN /\ replies'=replies \cup {<<req.id,req.body,host.result>>}
          /\ host'=[host EXCEPT !.queue=Tail(@), !.writeReady=FALSE,
                     !.request=EmptyRequest, !.result=0, !.cache=@ \cup {req.id}]
    /\ UNCHANGED <<disk,buffered,running,table,commands,acknowledged,installAck,
                   published,crashes,lostWrite,recoveredInstall>>
Replay(r,b) ==
    /\ running[H] /\ host.mode \in {"Open","Closing"} /\ HasReceipt(r)
    /\ ~BindPayload \/ ReceiptMatches(r,b)
    /\ replies'=replies \cup {<<r,b,disk[H].receipts[r].result>>}
    /\ UNCHANGED <<disk,buffered,running,host,table,commands,acknowledged,
                   installAck,published,crashes,lostWrite,recoveredInstall>>
DeliverReply(reply) ==
    /\ reply \in replies /\ reply \notin acknowledged
    /\ acknowledged'=acknowledged \cup {reply}
    /\ UNCHANGED <<disk,buffered,running,host,table,commands,replies,
                   installAck,published,crashes,lostWrite,recoveredInstall>>

(* One immutable directory transaction changes epoch 1 to epoch 2. Table is
   outside the data RPC path. Its intent precedes any outgoing configure RPC. *)
PrepareIntent ==
    /\ TableFree /\ table="Idle" /\ disk[T].decision="None"
    /\ store!Stage(T,[disk[T] EXCEPT !.target=2, !.tx=1, !.decision="Intent"])
    /\ table'="IntentPending"
    /\ UNCHANGED <<host,commands,replies,acknowledged,installAck,published,
                   crashes,lostWrite,recoveredInstall>>
SendConfigure ==
    /\ running[T] /\ table \in {"IntentPending","Waiting"}
    /\ IF PersistIntent THEN disk[T].decision \in {"Intent","Committed"}
       ELSE disk[T].decision \in {"Intent","Committed"} \/ buffered[T]#NoBuffer
    /\ commands'=TRUE /\ table'="Waiting"
    /\ UNCHANGED <<disk,buffered,running,host,replies,acknowledged,installAck,
                   published,crashes,lostWrite,recoveredInstall>>
ReceiveConfigure ==
    /\ commands /\ HostFree /\ host.mode="Open" /\ disk[H].epoch=1
    /\ store!Stage(H,[disk[H] EXCEPT !.gate="Closing", !.target=2, !.tx=1])
    /\ host'=[host EXCEPT !.mode="Closing", !.job="Close"]
    /\ UNCHANGED <<table,commands,replies,acknowledged,installAck,published,
                   crashes,lostWrite,recoveredInstall>>
ObserveCloseCommit ==
    /\ running[H] /\ host.job="Close" /\ buffered[H]=NoBuffer
    /\ disk[H].gate="Closing" /\ disk[H].target=2
    /\ host'=[host EXCEPT !.job="Idle"]
    /\ UNCHANGED <<disk,buffered,running,table,commands,replies,acknowledged,
                   installAck,published,crashes,lostWrite,recoveredInstall>>
PrepareInstall ==
    /\ HostFree /\ host.mode="Closing" /\ disk[H].gate="Closing"
    /\ disk[H].target=2 /\ (~RequireDrain \/ Len(host.queue)=0)
    /\ store!Stage(H,[disk[H] EXCEPT !.epoch=2, !.gate="Open"])
    /\ host'=[host EXCEPT !.job="Install"]
    /\ UNCHANGED <<table,commands,replies,acknowledged,installAck,published,
                   crashes,lostWrite,recoveredInstall>>
ObserveInstallCommit ==
    /\ running[H] /\ host.job="Install"
    /\ IF DurablePublication THEN disk[H].epoch=2 /\ buffered[H]=NoBuffer
       ELSE disk[H].epoch=2 \/ buffered[H]#NoBuffer
    /\ host'=[host EXCEPT !.mode="Open", !.job="Idle"] /\ installAck'=TRUE
    /\ UNCHANGED <<disk,buffered,running,table,commands,replies,acknowledged,
                   published,crashes,lostWrite,recoveredInstall>>
RepeatInstallAck ==
    /\ commands /\ HostFree /\ host.mode="Open" /\ disk[H].epoch=2
    /\ installAck'=TRUE
    /\ UNCHANGED <<disk,buffered,running,host,table,commands,replies,acknowledged,
                   published,crashes,lostWrite,recoveredInstall>>
PrepareTableConfirmation ==
    /\ TableFree /\ table="Waiting" /\ installAck /\ disk[T].decision="Intent"
    /\ store!Stage(T,[disk[T] EXCEPT !.decision="Committed", !.epoch=2])
    /\ table'="ConfirmPending"
    /\ UNCHANGED <<host,commands,replies,acknowledged,installAck,published,
                   crashes,lostWrite,recoveredInstall>>
PublishGrant ==
    /\ running[T] /\ table \in {"ConfirmPending","Ready"}
    /\ disk[T].decision="Committed" /\ buffered[T]=NoBuffer
    /\ published'=TRUE /\ table'="Ready"
    /\ UNCHANGED <<disk,buffered,running,host,commands,replies,acknowledged,
                   installAck,crashes,lostWrite,recoveredInstall>>

(* Network messages and externally observed replies survive process death.
   A timeout never changes either node's durable decision. *)
Crash(victims) ==
    /\ crashes<MaxCrashes /\ victims \subseteq Nodes /\ victims#{}
    /\ \E n \in victims: running[n]
    /\ store!Crash(victims)
    /\ host'=IF H \in victims THEN [EmptyHost EXCEPT !.mode="Offline"] ELSE host
    /\ table'=IF T \in victims THEN "Offline" ELSE table
    /\ crashes'=crashes+1
    /\ lostWrite'=(lostWrite \/ (H \in victims /\ \E r \in Ids: HasReceipt(r) /\ <<r,disk[H].receipts[r].body,disk[H].receipts[r].result>> \notin replies))
    /\ recoveredInstall'=(recoveredInstall \/ (H \in victims /\ disk[H].epoch=2))
    /\ UNCHANGED <<commands,replies,acknowledged,installAck,published>>
RestartHost ==
    /\ ~running[H] /\ store!Restart(H)
    /\ host'=[host EXCEPT !.mode="Recovering"]
    /\ UNCHANGED <<table,commands,replies,acknowledged,installAck,published,
                   crashes,lostWrite,recoveredInstall>>
RecoverHost ==
    /\ running[H] /\ host.mode="Recovering"
    /\ host'=[EmptyHost EXCEPT !.mode=IF RecoverClosed THEN disk[H].gate ELSE "Open"]
    /\ UNCHANGED <<disk,buffered,running,table,commands,replies,acknowledged,
                   installAck,published,crashes,lostWrite,recoveredInstall>>
RestartTable ==
    /\ ~running[T] /\ store!Restart(T) /\ table'="Recovering"
    /\ UNCHANGED <<host,commands,replies,acknowledged,installAck,published,
                   crashes,lostWrite,recoveredInstall>>
RecoverTable ==
    /\ running[T] /\ table="Recovering"
    /\ table'=CASE disk[T].decision="None" -> "Idle"
                  [] disk[T].decision="Intent" -> "Waiting"
                  [] OTHER -> "Ready"
    /\ UNCHANGED <<disk,buffered,running,host,commands,replies,acknowledged,
                   installAck,published,crashes,lostWrite,recoveredInstall>>
Flush(n) == /\ store!Sync(n)
            /\ UNCHANGED <<host,table,commands,replies,acknowledged,installAck,
                           published,crashes,lostWrite,recoveredInstall>>
Next == (\E r \in Ids,b \in Bodies: Admit(r,b) \/ Replay(r,b))
        \/ PrepareWrite \/ ObserveWriteCommit \/ CompleteWrite \/ (\E reply \in replies: DeliverReply(reply))
        \/ PrepareIntent \/ SendConfigure \/ ReceiveConfigure \/ ObserveCloseCommit
        \/ PrepareInstall \/ ObserveInstallCommit \/ RepeatInstallAck
        \/ PrepareTableConfirmation \/ PublishGrant
        \/ (\E n \in Nodes: Flush(n)) \/ (\E victims \in SUBSET Nodes: Crash(victims))
        \/ RestartHost \/ RecoverHost \/ RestartTable \/ RecoverTable
Spec == Init /\ [][Next]_vars

ReceiptSum == IF MaxRequests=1 THEN disk[H].receipts[1].body
              ELSE disk[H].receipts[1].body+disk[H].receipts[2].body
TypeOK == /\ disk[H].value \in 0..(MaxRequests*2)
          /\ disk[H].epoch \in {1,2} /\ disk[T].decision \in {"None","Intent","Committed"}
          /\ host.mode \in {"Open","Closing","Recovering","Offline"}
          /\ host.job \in {"Idle","Write","Close","Install"}
          /\ Len(host.queue)<=MaxRequests /\ crashes \in 0..MaxCrashes
AtomicBusinessCommit == disk[H].value=ReceiptSum
AckSurvivesCrash == \A reply \in replies: ReplyMatches(reply)
ObservedAckSurvivesCrash == \A reply \in acknowledged: ReplyMatches(reply)
IntentBeforeSideEffect == disk[H].tx#0 => disk[T].decision#"None"
InstallAckIsDurable == installAck => disk[H].epoch=2 /\ disk[H].gate="Open"
PublishedGrantIsDurable == published => disk[T].decision="Committed" /\ disk[H].epoch=2
ClosedRecovery == host.mode="Open" => disk[H].gate="Open"
DrainBeforeInstall == disk[H].epoch=2 => Len(host.queue)=0
NoLostAckRecovery == ~(lostWrite /\ running[H] /\ host.mode="Open" /\ acknowledged#{})
NoGrantRecovery == ~(recoveredInstall /\ running[H] /\ host.mode="Open" /\ published)
=============================================================================
