# Sleepy Router — concetti da ripassare

Promemoria di teoria: le nozioni che sono servite per capire i bug di questo fork, raccolte per argomento e non in ordine cronologico. Ogni voce dice **cos'è**, **perché conta** e **dove l'abbiamo visto** (numero di bug in `DESIGN_LOG.md` §4, oppure file e funzione).

Documenti collegati: `ROADMAP.md` (cosa è fatto e cosa manca), `DESIGN_LOG.md` (diagnosi complete), `CHANGES_VS_UPSTREAM.md` (mappa delle modifiche).

Ultimo aggiornamento: 2026-10-07.

---

## 1. CSL in breve

- **CSL (Coordinated Sampling Listening, Thread 1.2).** Il ricevitore tiene la radio spenta e la riaccende per una breve finestra a ogni **periodo**, con una **fase** nota. Chi trasmette non manda subito: aspetta la finestra del ricevitore.
- **Receiver e transmitter.** CSL Receiver = chi dorme (di serie un Child sleepy). CSL Transmitter = chi accoda e trasmette nella finestra altrui (di serie il Parent). Nel fork un Router può essere entrambe le cose.
- **Come si impara la schedule di un vicino.** Dal **CSL IE** (Information Element nell'header 802.15.4) dei frame che il vicino trasmette. `Mac::ProcessCsl()` lo legge solo da frame cifrati con **Key ID Mode 1** e versione 2015: un CSL IE su un frame Mode 2 non sincronizza nessuno (bug #19).
- **Annunciare non è dormire.** Pubblicare un periodo CSL e spegnere davvero la radio sono due cose distinte. Il sonno fisico dipende da `IsCslSupported()`, `SetCslCapable()` e `Mac::UpdateIdleMode()` (bug #6-#9). Nei test si verifica campionando `mRadio.mState == kStateSleep`, non fidandosi dell'annuncio.
- **Un ricevitore che dorme non sente nulla.** In Nexus una radio in `kStateSleep` non riceve proprio il frame (`Radio::CanReceiveOnChannel()`). Un broadcast diretto one-shot verso un vicino che dorme è perso, a meno che non capiti nella sua finestra (bug #10).

## 2. Trasmissione diretta e indiretta

- **Diretta.** `MeshForwarder` sceglie il prossimo messaggio dalla send queue (`GetDirectTransmission()`), calcola la rotta (`UpdateIp6Route()`/`UpdateMeshRoute()`) e lo trasmette subito con CSMA e retry.
- **Indiretta.** Il messaggio resta nella send queue marcato "pendente per il vicino X". `IndirectSender` tiene per ogni vicino un **cursore** (`GetIndirectMessage()`) sul messaggio corrente; `CslTxScheduler` decide *quando* trasmettere e *a chi*, tra i vicini CSL-sincronizzati con messaggi in coda.
- **Un frame per finestra per vicino.** Ogni finestra CSL porta un solo frame verso quel vicino. Se la coda verso un vicino ha N messaggi, l'ultimo aspetta circa N finestre: la latenza cresce con la coda, non solo con il numero di hop.
- **Contatori coerenti.** `CslTxScheduler::RescheduleCslTx()` sceglie un vicino solo se `GetIndirectMessageCount() > 0`. Ogni accodamento deve incrementare il contatore e ogni rimozione decrementarlo; ogni messaggio pendente deve essere visibile a `RemoveMessageIfNoPendingTx()`. Ogni rottura di questa invariante ha prodotto un bug diverso: mai trasmesso (#3), loop infinito (#4), use-after-free (#5), leak (#12).
- **Un messaggio, molti destinatari indiretti.** Lo stesso multicast può essere pendente per più vicini insieme. Per questo lo stato è una **maschera di bit** (`ChildMask`, e nel fork `RouterMask`), non uno slot singolo (bug #15).

## 3. Routing mesh e Mesh Header

- **RLOC16.** L'indirizzo breve di instradamento di un nodo (Router ID + Child ID). `RouterTable::GetNextHop(rloc16)` dà il vicino a cui consegnare per raggiungere una destinazione mesh.
- **Mesh Header (6LoWPAN).** Quando il prossimo hop **non** è la destinazione finale, il frame porta un Mesh Header con sorgente e destinazione mesh (RLOC16). I nodi intermedi inoltrano leggendo solo quello, senza guardare l'IPv6 e senza rifare la risoluzione degli indirizzi.
- **Due forme dello stesso messaggio.** `Message::kTypeIp6` è un pacchetto IPv6 originato (o terminato) localmente; `Message::kType6lowpan` è un frame già compresso con Mesh Header, in puro transito. Il percorso CSL deve saper costruire entrambi (bug #16 per il transito, bug #21 per l'originato).
- **Stato transitorio contro stato del messaggio.** `mAddMeshHeader`, `mMeshSource`, `mMeshDest` sono campi di `MeshForwarder`: valgono solo per la trasmissione diretta *in corso*. Un messaggio che viene convertito a indiretto e trasmesso più tardi li ha persi. L'informazione che deve sopravvivere va **sul messaggio**: `Message::SetMeshDest()` (bug #21). Domanda da farsi sempre: "questa informazione vive abbastanza a lungo per chi la userà?"
- **Perché lo stock non ne aveva bisogno.** Di serie il destinatario indiretto è sempre un Child, che è sempre la destinazione finale: niente Mesh Header. Un Router-peer invece può essere un hop intermedio.
- **Metadati azzerati.** I metadati di un `Message` sono azzerati all'allocazione. Un campo mai scritto vale 0, e per un RLOC16 `0x0000` è un valore valido (Router ID 0). Un default "zero" non significa "non impostato": chi legge deve sapere che qualcuno l'ha scritto (bug #21, ramo unicast di `SendMessage()`).

## 4. Address Resolution

- **Cosa fa.** Traduce un indirizzo IPv6 (tipicamente l'ML-EID) nell'RLOC16 del nodo. Manca in cache → **AddrQuery** in multicast realm-local (`ff03::2`, via MPL) → il proprietario risponde con **AddrNotify** in unicast → voce in cache.
- **Costo su CSL.** Ogni passaggio aspetta una finestra CSL su ogni hop sleepy. Un ping a un solo hop che richiede la risoluzione ha misurato 639-1279 ms contro ~199 ms a cache popolata (catena a 6, 2026-10-07).
- **La decisione va rifatta dopo la risoluzione.** `SendMessage()` decide diretto/indiretto una sola volta, quando il vicino può non essere ancora noto; `UpdateIp6RouteFtd()` invece gira a ogni tentativo, anche dopo la risoluzione. Per questo la conversione a CSL sta lì (bug #11, #16).
- **Sintomo tipico di un Mesh Header mancante:** un nodo intermedio fa una AddrQuery per un indirizzo che non è suo e che non dovrebbe interessargli (bug #21).

## 5. Code, priorità e frame in volo

- **Send queue con priorità.** `net` (MLE, MPL, Address Resolution) passa davanti a `normal`. `FindQueuedMessageFor...()` prende il primo pendente, quindi il più prioritario.
- **Sostituzione del messaggio corrente.** Quando arriva un messaggio più prioritario, `RequestMessageUpdate()` sposta il cursore del vicino. Se il frame del messaggio precedente era già in volo, lo stock `CslTxScheduler::Update()` lo "abbandona" (`mCslTxNeighbor = nullptr`): l'esito viene ignorato e il messaggio vecchio viene ritrasmesso più tardi come frame **nuovo**, con un nuovo numero di sequenza. Il ricevitore non può riconoscerlo come ritrasmissione e lo consegna due volte. È comportamento stock, più frequente nel fork perché nelle code CSL c'è molto più multicast `net`.
- **Ritrasmissione contro frame nuovo.** Una vera ritrasmissione CSL riusa numero di sequenza, frame counter e key id (`SetIsARetransmission(true)`), così il ricevitore la scarta se l'aveva già. Un frame "nuovo" per lo stesso messaggio no.
- **Head-of-line blocking.** Se il messaggio in testa alla coda verso un vicino fallisce sempre, blocca tutti quelli dietro (visto con l'Announce del bug #19, analizzato nel #20).

## 6. Multicast e MPL

- **Link-local (`ff02::…`).** Un hop solo, broadcast diretto. Esempio: MLE Advertisement.
- **Realm-local (`ff03::…`).** Inoltrato da tutti i Router tramite **MPL**, con ritrasmissioni temporizzate da Trickle e soppressione dei duplicati per seed/sequenza. Esempio: AddrQuery.
- **Verso i Child sleepy, nello stock.** Solo la prima trasmissione MPL, e solo se il Child è interessato (`destinedForAll || child.HasIp6Address(destination)`) e non è lui la sorgente (`!child.HasIp6Address(source)`).
- **Verso i Router-peer, nel fork.** Ogni multicast non fuori canale viene accodato a ogni Router-peer CSL-sincronizzato. Non c'è l'esclusione della sorgente: il multicast torna in CSL anche al vicino da cui era arrivato (eco), e ogni eco costa una finestra. Domanda aperta: quale multicast deve davvero entrare nella coda CSL di un Router-peer.
- **MLE fuori canale.** Announce e Discovery Request vanno su un altro canale: una copia CSL partirebbe sul canale della PAN, quindi sono esclusi dalla coda CSL (bug #19 parte A).

## 7. Sicurezza MAC

- **Key ID Mode 1.** La chiave di rete corrente, per il traffico normale. È l'unico modo che `SubMac` sa cifrare nel percorso CSL.
- **Key ID Mode 2.** Una chiave fissa nota, usata da MLE Announce/Discovery. Materiale di chiave e frame counter stanno in `Mac`, non in `SubMac`.
- **Conseguenza (bug #19).** `Mac` rimanda a `SubMac` la cifratura dei frame che portano un CSL IE; un frame Mode 2 con CSL IE partiva quindi senza cifratura. Fix: niente CSL IE sui frame cifrati non Mode 1, tanto non sincronizzerebbero nessuno (§1).
- **Il ricevente deve riconoscere il mittente.** `ProcessReceiveSecurity()` cerca il vicino dall'indirizzo MAC sorgente: un frame con sorgente "None" viene rifiutato (bug #17).

## 8. Pattern ricorrenti nei bug del fork

- **Assunzione implicita di ruolo.** Molto codice stock assume "chi arriva qui è un Child con un solo Parent" senza dirlo. Estenderlo ai Router compila senza errori e si rompe a runtime, un caso alla volta (#3-#5, #8-#10, #21).
- **Un fix apre il caso successivo.** Il #16 ha prodotto direttamente #17 e #18: un percorso nuovo eredita tutte le assunzioni di quello vecchio (indirizzo sorgente, flag di collision avoidance). Dopo un fix, chiedersi cosa dava per scontato il codice a valle.
- **Guardie `#if` sbilanciate.** Funzione dietro una guardia stretta, chiamante dietro una larga: compila nella configurazione di tutti i giorni e si rompe in quella rara (bug #14). Si verifica compilando Thread 1.1 e `CSL_TRANSMITTER_ENABLE=0`.
- **Una scala più grande toglie le maschere.** Con 3 nodi la ri-risoluzione per hop arrivava in tempo e nascondeva il #21; con 6 no. Un test che passa a una scala piccola non prova che il meccanismo sia corretto.

## 9. Testare con Nexus: insidie

- **`SendAndVerifyEchoRequest()` avanza sempre l'intero timeout** (default 1 s), non si ferma alla risposta. Alzare il timeout sposta anche tutti i passi successivi del test.
- **Log silenziosi di default.** Il livello parte da `OT_LOG_LEVEL_CRIT`: per vedere `LogInfo`/`LogWarn` serve `otLoggingSetLevel(OT_LOG_LEVEL_INFO)` temporaneo in testa al test.
- **Spegnere un nodo non basta.** `SetNodeEnabled(false)` non ferma la radio, che continua a mandare ACK. Per renderlo irraggiungibile: `Node::SetPosition(2000, 0)` (RSSI sotto -100 dBm, frame scartato prima dell'ACK).
- **Invii asincroni.** `SendEchoRequest()` passa da un tasklet: `AdvanceTime(0)` lo esegue senza far avanzare il tempo.
- **Buffer.** `MessagePool::GetFreeBufferCount()` non è utilizzabile su Nexus (heap esterno); contare i messaggi in coda con `MeshForwarder::GetQueueInfo()`.
- **Le run non sono identiche.** Seed diversi danno tempi diversi: un risultato vale dopo molte run (12-20), non una.
- **Verificare nei due sensi.** Un test di regressione vale solo se **fallisce senza il fix** e passa con il fix (`purge` dopo la correzione del 2026-10-06, la catena per il #21).
- **Leggere il log hop per hop.** `Sent mesh frame` contro `Sent IPv6 ... msg`, `Prepping indir tx` (accodato in CSL), `Received ...` sul nodo successivo: la traccia mostra dove il messaggio cambia forma o si ferma.

## 10. Metodo

- **Cercare in tutto `src/core`, non in una sottocartella**, prima di dichiarare che una funzione non è usata (lezione di Tappa B).
- **Cercare il precedente nello stock prima di inventare.** `ChildMask` → `RouterMask`, `ClearAllMessagesForSleepyChild` → `...Router`, il caso ALOC→Child sleepy per la conversione in `UpdateIp6RouteFtd()`, `SetMeshDest()` per il #21.
- **Distinguere bug del fork da comportamento stock.** Prima di correggere, controllare se lo stock fa lo stesso (`git show <base>:file`). Se sì, è un limite da documentare, non un bug da correggere in silenzio (consegne duplicate).
- **Ipotesi, poi evidenza.** La prima diagnosi del #19 ("frame counter congelato") era sbagliata: si chiude un bug solo quando log e codice dicono la stessa cosa.
