---------------------------- MODULE Authority ----------------------------
EXTENDS Naturals, Integers, Sequences, FiniteSets, TLC

CONSTANTS MaxEpoch, MaxInflight, RequireDrain, RequirePermission, AckAfterInstall
Clients == {"A", "B"}
Configs == 0..6
Holders(c) == CASE c = 1 \/ c = 4 -> {"A"}
               [] c = 2 \/ c = 5 -> {"B"}
               [] c = 3 -> Clients
               [] OTHER -> {}
Exclusive(c) == c \in {4, 5}
NoCommand == [epoch |-> 0, config |-> 0]
Commands == [epoch: 1..MaxEpoch, config: Configs]
Tokens == [epoch: 1..MaxEpoch, config: Configs, client: Clients]
Operations == [epoch: 1..MaxEpoch, config: Configs, client: Clients,
               write: BOOLEAN, abandoned: BOOLEAN]

VARIABLES epoch, config, gate, dead, queue, executing, hostPending,
          directoryEpoch, directoryConfig, directoryPending, directoryWaiting,
          requests, acknowledgments, tokens, installed, connected
vars == <<epoch, config, gate, dead, queue, executing, hostPending,
          directoryEpoch, directoryConfig, directoryPending, directoryWaiting,
          requests, acknowledgments, tokens, installed, connected>>

Init == /\ epoch = 0 /\ config = 0 /\ gate = TRUE /\ dead = FALSE
        /\ queue = <<>> /\ executing = FALSE /\ hostPending = NoCommand
        /\ directoryEpoch = 0 /\ directoryConfig = 0
        /\ directoryPending = NoCommand /\ directoryWaiting = FALSE
        /\ requests = {} /\ acknowledgments = {} /\ tokens = {}
        /\ installed = [e \in {0} |-> 0] /\ connected = TRUE

\* One directory transaction at a time. Sent records remain available for
\* arbitrary delayed/duplicate delivery and idempotent retries. No fairness or
\* reliable-delivery assumption is imposed by this safety specification.
Request(c) ==
    /\ connected /\ ~directoryWaiting /\ directoryConfig # 6
    /\ directoryEpoch < MaxEpoch /\ c \in Configs
    /\ LET cmd == [epoch |-> directoryEpoch + 1, config |-> c] IN
        /\ directoryPending' = cmd /\ requests' = requests \cup {cmd}
    /\ directoryWaiting' = TRUE
    /\ UNCHANGED <<epoch, config, gate, dead, queue, executing, hostPending,
                   directoryEpoch, directoryConfig, acknowledgments, tokens,
                   installed, connected>>

\* Closing admission and accepting a transition share the Host decision point.
Close(cmd) ==
    /\ connected /\ cmd \in requests /\ ~dead
    /\ cmd.epoch = epoch + 1 /\ hostPending = NoCommand
    /\ hostPending' = cmd /\ gate' = FALSE
    /\ acknowledgments' = IF AckAfterInstall THEN acknowledgments
                          ELSE acknowledgments \cup {cmd}
    /\ UNCHANGED <<epoch, config, dead, queue, executing,
                   directoryEpoch, directoryConfig, directoryPending,
                   directoryWaiting, requests, tokens, installed, connected>>

Install ==
    /\ hostPending # NoCommand
    /\ (~RequireDrain \/ Len(queue) = 0)
    /\ epoch' = hostPending.epoch /\ config' = hostPending.config
    /\ dead' = (hostPending.config = 6)
    /\ gate' = (hostPending.config # 6)
    /\ installed' = [e \in (DOMAIN installed) \cup {hostPending.epoch} |->
                        IF e = hostPending.epoch THEN hostPending.config ELSE installed[e]]
    /\ acknowledgments' = acknowledgments \cup {hostPending}
    /\ hostPending' = NoCommand
    /\ UNCHANGED <<queue, executing, directoryEpoch, directoryConfig,
                   directoryPending, directoryWaiting, requests, tokens, connected>>

\* Only an ACK matching the full pending transaction publishes capabilities.
\* Older ACKs and control messages cannot change the installed epoch.
Publish(cmd) ==
    /\ connected /\ directoryWaiting
    /\ cmd \in acknowledgments /\ cmd = directoryPending
    /\ directoryEpoch' = cmd.epoch /\ directoryConfig' = cmd.config
    /\ directoryWaiting' = FALSE /\ directoryPending' = NoCommand
    /\ tokens' = tokens \cup {[epoch |-> cmd.epoch, config |-> cmd.config, client |-> c]:
                              c \in Holders(cmd.config)}
    /\ UNCHANGED <<epoch, config, gate, dead, queue, executing, hostPending,
                   requests, acknowledgments, installed, connected>>

Admit(token, write) ==
    /\ gate /\ ~dead /\ Len(queue) < MaxInflight
    /\ token \in tokens /\ token.epoch = epoch /\ token.config = config
    /\ token.client \in Holders(config)
    /\ (~write \/ Exclusive(config) \/ ~RequirePermission)
    /\ queue' = Append(queue, [epoch |-> epoch, config |-> config,
                    client |-> token.client, write |-> write, abandoned |-> FALSE])
    /\ UNCHANGED <<epoch, config, gate, dead, executing, hostPending,
                   directoryEpoch, directoryConfig, directoryPending,
                   directoryWaiting, requests, acknowledgments, tokens, installed, connected>>

\* Accepted, queued and executing operations all occupy queue slots. One head
\* executes at a time; client abandonment changes observation, never Host pins.
Start == /\ Len(queue) > 0 /\ ~executing /\ executing' = TRUE
         /\ UNCHANGED <<epoch, config, gate, dead, queue, hostPending,
                        directoryEpoch, directoryConfig, directoryPending,
                        directoryWaiting, requests, acknowledgments, tokens, installed, connected>>
Complete == /\ executing /\ Len(queue) > 0
            /\ queue' = Tail(queue) /\ executing' = FALSE
            /\ UNCHANGED <<epoch, config, gate, dead, hostPending,
                           directoryEpoch, directoryConfig, directoryPending,
                           directoryWaiting, requests, acknowledgments, tokens, installed, connected>>
Abandon(i) == /\ i \in 1..Len(queue) /\ ~queue[i].abandoned
             /\ queue' = [queue EXCEPT ![i].abandoned = TRUE]
             /\ UNCHANGED <<epoch, config, gate, dead, executing, hostPending,
                            directoryEpoch, directoryConfig, directoryPending,
                            directoryWaiting, requests, acknowledgments, tokens, installed, connected>>
Disconnect == /\ connected /\ connected' = FALSE
              /\ UNCHANGED <<epoch, config, gate, dead, queue, executing, hostPending,
                             directoryEpoch, directoryConfig, directoryPending,
                             directoryWaiting, requests, acknowledgments, tokens, installed>>

\* Stale/duplicate messages are explicit no-ops; a same-epoch different payload
\* is rejected. Unsent transactions are outside this trusted-controller model.
Replay(cmd) == /\ cmd \in requests
              /\ (cmd.epoch <= epoch \/ hostPending # NoCommand)
              /\ UNCHANGED vars
ReplayAck(cmd) == /\ cmd \in acknowledgments /\ cmd # directoryPending
                 /\ UNCHANGED vars

Next == (\E c \in Configs: Request(c))
     \/ (\E cmd \in requests: Close(cmd) \/ Replay(cmd))
     \/ Install
     \/ (\E cmd \in acknowledgments: Publish(cmd) \/ ReplayAck(cmd))
     \/ (\E token \in tokens, write \in BOOLEAN: Admit(token, write))
     \/ Start \/ Complete \/ (\E i \in 1..Len(queue): Abandon(i)) \/ Disconnect
Spec == Init /\ [][Next]_vars

TypeOK == /\ epoch \in 0..MaxEpoch /\ config \in Configs
          /\ gate \in BOOLEAN /\ dead \in BOOLEAN /\ connected \in BOOLEAN
          /\ queue \in Seq(Operations) /\ Len(queue) <= MaxInflight
          /\ executing \in BOOLEAN /\ hostPending \in Commands \cup {NoCommand}
          /\ directoryEpoch \in 0..MaxEpoch /\ directoryConfig \in Configs
          /\ directoryPending \in Commands \cup {NoCommand} /\ directoryWaiting \in BOOLEAN
          /\ requests \subseteq Commands /\ acknowledgments \subseteq Commands
          /\ tokens \subseteq Tokens /\ installed \in [0..epoch -> Configs]
ActiveEpoch == \A i \in 1..Len(queue): queue[i].epoch = epoch /\ queue[i].config = config
AuthorizedOperations == \A i \in 1..Len(queue):
    queue[i].client \in Holders(config) /\ (queue[i].write => Exclusive(config))
DrainBeforeDestruction == dead => Len(queue) = 0 /\ ~executing /\ ~gate
SingleExecutionLane == executing => Len(queue) > 0
ClosedDuringTransition == hostPending # NoCommand => ~gate
AckIsInstalled == \A ack \in acknowledgments:
    ack.epoch \in DOMAIN installed /\ installed[ack.epoch] = ack.config
GrantAfterAck == /\ directoryEpoch <= epoch
                /\ directoryEpoch \in DOMAIN installed
                /\ installed[directoryEpoch] = directoryConfig
                /\ \A token \in tokens: token.epoch \in DOMAIN installed
                       /\ installed[token.epoch] = token.config
                       /\ token.client \in Holders(token.config)
=============================================================================
