# Sleepy Router — riepilogo delle modifiche rispetto a OpenThread stock

Data: 2026-10-06. Base di confronto: commit `a49c1f5c9` (ultimo commit stock prima del fork). Il confronto è stato fatto leggendo `git diff a49c1f5c9` sul working tree, quindi comprende anche le modifiche non ancora committate e i file nuovi non tracciati.

Questo documento è una **mappa**: dice cosa è cambiato, dove e perché. Il ragionamento completo, la cronologia e le diagnosi dei bug stanno in `DESIGN_LOG.md` (§3 tappe, §4 catalogo bug, §6 limiti); il tracker di lavoro è `ROADMAP.md`.

---

## 1. L'idea in breve

In Thread stock un Router (FTD) tiene sempre la radio accesa, e solo un *Child* può dormire (Sleepy End Device). Questo fork introduce lo **Sleepy Router**: un Router che resta tale nella rete (instrada, ha Child, partecipa a MLE) ma **duty-cicla la radio** usando il meccanismo **CSL** (Coordinated Sampling Listening, Thread 1.2) invece di tenerla accesa.

Il meccanismo, in quattro pezzi:
1. **Annuncio dello schedule.** Lo Sleepy Router mette un *CSL IE* (periodo e fase della propria finestra di ascolto) nei frame che trasmette. La fase si calcola all'ultimo istante, in `Mac::BeginTransmit()`.
2. **Apprendimento.** Chi riceve un frame con CSL IE da uno Sleepy Router ne memorizza periodo e fase (`Mac::ProcessCsl()`) e lo marca come *CSL-sincronizzato*.
3. **Trasmissione indiretta.** I messaggi destinati a un vicino CSL-sincronizzato non vanno in trasmissione diretta (cadrebbero nel vuoto, il vicino dorme): vengono accodati e trasmessi nella finestra del vicino dal `CslTxScheduler`.
4. **Sonno fisico.** Il Router, a sua volta, entra davvero in `Sleep()` e si sveglia solo per le proprie finestre CSL.

Nello stock i punti 2-3 esistono solo per la relazione *Parent sempre acceso → Child sleepy*. Il lavoro è consistito nel **generalizzare** quel percorso da "Child" a "qualsiasi vicino CSL" (Child, Parent, Router-peer), senza cambiare il comportamento stock dei Child.

---

## 2. Le scelte di design principali, con la motivazione

| Scelta | Perché |
|---|---|
| **`Router` eredita `CslNeighbor`** invece di `Neighbor` (`router.hpp`). | Un Router-peer ha bisogno degli stessi campi CSL (periodo, fase, ultimo rx, coda indiretta) di un Child. Ereditarli dalla stessa base permette di riusare codice generico invece di duplicare i campi. Risolve anche il bug #1 (un `Parent` aveva una copia locale dei campi CSL che il codice generico non leggeva). |
| **Non toccare `IsRxOnWhenIdle()` / `DeviceMode`.** Un Sleepy Router resta formalmente `RxOnWhenIdle`. Gate indipendente: `Mle::mIsSleepyRouter`. | (1) `DeviceMode::IsValid()` richiede `RxOnWhenIdle` per ogni FTD: un FTD sleepy sarebbe non valido. (2) Nel Link Request/Accept il `DeviceMode` del Router-peer è hardcoded, quindi i peer non potrebbero comunque saperlo da lì. (3) `IsRxOnWhenIdle()` è usato ~20 volte in `mle.cpp` come proxy di "sono un Child sleepy che parla col suo Parent": flipparlo farebbe scattare comportamenti pensati solo per Child→Parent. |
| **Lo schedule si annuncia nel CSL IE dei frame**, non in un campo MLE. | Il Link Request/Accept non ha un canale per negoziarlo. Il CSL IE è già il meccanismo standard di Thread 1.2 per dichiarare uno schedule. |
| **Fase CSL calcolata in `BeginTransmit()`**, non quando si prepara il frame. | La fase è "quanto manca alla prossima finestra": cambia col tempo, si può calcolare solo al momento reale della trasmissione. Conseguenza: la cifratura dei frame con CSL IE va rimandata a dopo il patch (vedi §3.3 e bug #19). |
| **Un solo periodo fisso** (`kDefaultSleepyRouterCslPeriod = 1000`, unità da 10 simboli = 160 ms). | Semplificazione per la sperimentazione. Il periodo configurabile non è stato affrontato. |
| **`RouterMask` (bitmask) al posto di un bit/ID singolo** per "messaggio in attesa per un Router-peer". | Un messaggio multicast può essere pendente per più Router-peer insieme. Un singolo slot si sovrascriveva (bug #15, crash). Indicizzato per posizione in `RouterTable`, come `ChildMask` per `ChildTable`. |
| **Gate di compilazione larghi quanto il chiamante più permissivo.** | Bug #14: codice gated più stretto del suo chiamante non compilava su Thread 1.1. Regola applicata a `RouterMask`. |
| **Il Parent usa ancora un bit singolo** (`mPendingForParent`), non una maschera. | Un Child ha un solo Parent, quindi un bit basta. Non è stato generalizzato. |
| **CSL IE escluso dai frame cifrati non Mode 1**, a monte nel framer. | Vedi bug #19 parte B: alternative scartate, tra cui estendere `SubMac` a Mode 2. |

---

## 3. Modifiche file per file

### 3.1 API pubblica e CLI
- `include/openthread/thread.h`, `src/core/api/thread_api.cpp`: nuove `otThreadSetSleepyRouterMode(aInstance, bool)` e `otThreadIsSleepyRouterMode(aInstance)`, adapter sottili su `Mle`.
- `src/cli/cli.cpp`: comando `sleepyrouter [enable|disable]` (solo FTD).

### 3.2 MLE (`mle.hpp`, `mle.cpp`, `mle_ftd.cpp`)
- `Mle::SetSleepyRouterMode()` / `IsSleepyRouterMode()`, campo `mIsSleepyRouter`. Abilitare la modalità imposta il periodo CSL (`Mac::SetSleepyRouterCslPeriod`), la capacità CSL (`SetCslCapable`) e il periodo del ricevitore (`SetCslPeriod`). Va fatto qui perché `SetRole()` ricalcola la capacità CSL solo a un cambio di ruolo, che non avviene attivando la modalità su un Router già tale (bug #7).
- `IsCslSupported()` ora è vero anche per uno Sleepy Router (prima: solo `IsChild()` con Parent ≥ 1.2) — bug #6. `SetRole()` usa la nuova condizione `(!IsRxOnWhenIdle() || IsSleepyRouterMode())`.
- `mle_ftd.cpp::RemoveNeighbor()`: per un Router rimosso chiama `IndirectSender::ClearAllMessagesForSleepyRouter()`, mirror di quanto già fa il ramo Child — bug #12 (altrimenti il messaggio in coda per quel Router resta bloccato per sempre, e il suo `RouterId` può essere riassegnato a un altro Router).
- `SetDeviceMode()`: bypass di `IsValid()` e del re-attach per `IsSleepyRouterMode()`. **Residuo di un approccio scartato** (flippare il `DeviceMode`), oggi non usato: candidato alla rimozione prima di una PR.

### 3.3 MAC (`mac.hpp`, `mac.cpp`, `mac_frame.hpp/.cpp`)
- `Mac::SetSleepyRouterCslPeriod()/GetSleepyRouterCslPeriod()`: il periodo che uno Sleepy Router annuncia (0 = disattivato).
- `Mac::BeginTransmit()`: per un Sleepy Router, sui frame dati con CSL IE, scrive periodo e fase reali. La fase è `(periodo − (ora mod periodo)) / 160 µs`.
- `Mac::ProcessTransmitSecurity()`: la cifratura dei frame con CSL IE è rimandata (condizione estesa a `|| OPENTHREAD_FTD`), perché il CSL IE viene scritto dopo.
- `Mac::ProcessCsl()`: oltre ai Child, impara lo schedule dal **Parent** e dai **Router-peer** (cerca il mittente nella `RouterTable` via `NeighborTable::FindNeighbor`).
- `Mac::UpdateCslParameters()`: separa il caso Child (comportamento stock: filtro sul Parent, ricalcolo del poll, Child Update Request) dal caso Router. Per un Router non c'è un Parent a cui scrivere: il Child Update Request inviato comunque faceva staccare un Leader (bug #8). Passa il proprio RLOC16 come filtro segnaposto invece di un indirizzo invalido, perché la piattaforma Nexus lo rifiuta e lasciava `mCslPeriod` a zero (SIGFPE, bug #13).
- `Mac::UpdateIdleMode()`: la radio va in `Sleep()` anche con `RxOnWhenIdle` vero se il CSL è abilitato. Altrimenti il duty-cycling CSL di `SubMac` non parte mai (bug #9).
- `mac_frame.*`: gli accessor del CSL IE (`SetCslIe`, `GetCslIe`, `HasCslIe`, il flag `mAppendCslIe`) sono resi disponibili anche con `|| OPENTHREAD_FTD`, e gated su `OPENTHREAD_CONFIG_MAC_HEADER_IE_SUPPORT` (bug #14). C'è un `// TODO: REENABLE OPENTHREAD_FTD MACRO` da chiarire.
- Non si modifica `sub_mac.cpp`: il fork non tocca il sonno fisico a quel livello.

### 3.4 Framer (`message_framer.cpp`)
`PrepareMacHeaders()` decide se riservare spazio per il CSL IE e forzare la versione 2015:
- **broadcast** da uno Sleepy Router, solo se il frame non è cifrato oppure è Key ID Mode 1;
- **unicast verso un Child**, anche quando lo Sleepy Router non ha motivo stock di farlo, così un Router con Child e Router-peer annuncia lo schedule su ogni frame;
- Child CSL-sincronizzato: logica stock, riorganizzata.

### 3.5 Scheduler CSL (`csl_tx_scheduler.hpp/.cpp`)
- `RescheduleCslTx()`: oltre ai Child considera i Router-peer CSL-sincronizzati con messaggi in coda, e (per un Child) il proprio Parent.
- `GetNextCslTransmissionDelay()` prende `CslTxScheduler::NeighborInfo` invece di `CslNeighbor` (stessi campi, dipendenza più stretta).
- `HandleFrameRequest()`: aggiunto un `LogInfo` di debug per ogni richiesta, da togliere. La logica per il Parent passa dallo stesso percorso generico dei Child (nel fork esisteva un ramo dedicato con `mSchedulingParent`, poi ritirato: nel confronto con lo stock non compare).
- Non modificata: la logica di `HandleSentFrame()` (vedi limite noto #20).

### 3.6 `IndirectSender` (`indirect_sender.hpp/.cpp`) — la modifica più grande
- **`PrepareFrameForCslNeighbor()`**: nello stock chiamava `PrepareFrameForChild` con un cast cieco a `Child` ("può essere solo un Child per ora"). Ora costruisce il frame con gli accessor generici del mixin, senza cast, quindi serve Child, Parent e Router. `PrepareFrameForChild` resta perché serve a `DataPollHandler` (percorso data-poll stock).
  - Ramo `Message::kType6lowpan` (messaggio già incapsulato in mesh header da un hop precedente): senza, il frame non veniva costruito e usciva corrotto (bug #16). Imposta anche l'indirizzo MAC sorgente, senza il quale il ricevente non identificava il mittente (bug #17).
- **Percorso Router-peer**: `AddMessageForSleepyRouter`, `RemoveMessageFromSleepyRouter`, `HandleSentFrameToCslRouter`, `FindQueuedMessageForSleepyRouter`, `UpdateIndirectMessage(Router&)`, `RequestMessageUpdate(Router&)`, `ClearAllMessagesForSleepyRouter` — tutti mirror dei corrispondenti per Child, con `RouterMask`. `AddMessageForSleepyRouter` incrementa esplicitamente il contatore di messaggi del Router (bug #3); la gestione del completamento lo decrementa (bug #4).
- **Percorso Parent**: `AddMessageForSleepyParent`, `RemoveMessageFromSleepyParent`, `FindQueuedMessageForSleepyParent`, `HandleSentFrameToCslParent` e relativi update.
- **`HandleSentFrameToCslNeighbor()`** smista per tipo (Child, Router, Parent) con `Contains()` sulle tabelle, invece del cast cieco a `Child`.
- `AcceptAnyMessage` reso pubblico/spostato fuori dal blocco FTD, perché serve anche al percorso Parent.

### 3.7 `MeshForwarder` (`mesh_forwarder*.cpp/.hpp`)
- **`mesh_forwarder_ftd.cpp::SendMessage()`**:
  - multicast: oltre ai Child, accoda ogni Router-peer CSL-sincronizzato (bug #10), **escludendo i messaggi MLE fuori canale** (Announce, Discovery Request — bug #19 parte A);
  - unicast: cascata a rami (Child sleepy / Child di un Parent sleepy / Router-peer sleepy / diretto).
- **`UpdateIp6RouteFtd()` e `UpdateMeshRoute()`**: convertono a CSL-indiretto quando il prossimo hop è uno Sleepy Router CSL-sincronizzato, anche se è solo un hop intermedio (bug #11 e #16). Azzerano `mDelayNextTx` alla conversione (bug #18: il flag veniva ripulito solo dal completamento della trasmissione *diretta*).
- **`mesh_forwarder_mtd.cpp::SendMessage()`**: cascata a due rami (Child di Parent sleepy / diretto).
- **`mesh_forwarder.cpp`**: `FinalizeMessageIndirectTxsToParent()`, `RemoveMessageIfNoPendingTx()` considera anche `IsPendingForParent` e `GetIndirectTxRouterMask()`. Contiene un blocco `#if 0` (il fix "storico" con cui è iniziato il lavoro, `TxDelay` calcolato a mano per il Parent) e un commento "NOTE" che oggi descrive un comportamento non più attivo.

### 3.8 Strutture dati
- `router.hpp`: `class Router : public CslNeighbor`. Contiene un blocco di codice commentato (i vecchi campi CSL) da rimuovere.
- `router_table.hpp`: `GetRouterIndex()`.
- `router_mask.hpp` (nuovo): `RouterMask = BitSet<OPENTHREAD_CONFIG_MLE_MAX_ROUTERS>`, gated FTD.
- `common/message.hpp`: `mRouterMask` nei metadati (sostituisce il vecchio slot singolo) con accessor `GetIndirectTxRouterMask()`; bit `mPendingForParent` con i tre metodi.

### 3.9 Configurazione e varie
- `config/mac.h`: `MAC_DEFAULT_MAX_FRAME_RETRIES_DIRECT` 15 → 5. **Tuning sperimentale, non parte della feature.**
- `config/mle.h`: `MLE_CHILD_TIMEOUT_DEFAULT` 240 → 20. **Tuning sperimentale, non parte della feature.**
- `mac/data_poll_sender.cpp`: nel ramo `mRetxMode`, `period = 25000` fisso. **Hack preesistente**, non tocca il Sleepy Router.
- `mle.cpp::RetxTracker::RetryInfo::DetachIfMaxAttemptsReached()`: `BecomeDetached()` commentato, sostituito da un log "HACK". **Hack preesistente.**
- `script/check-simulation-build-cmake`: variante permanente Thread 1.4 con `CSL_TRANSMITTER_ENABLE=0`.
- `.gitignore` (6 righe aggiunte, non riviste in dettaglio), `doxygen/Doxyfile*`: strumenti di lavoro locali, non parte della feature.

### 3.10 Test (`tests/nexus/`)
Registrati in `CMakeLists.txt` con `ot_nexus_test(...)`:
- `test_sleepy_router_peer.cpp` — due Router: schedule imparato, radio che duty-cicla davvero, consegna nei due versi.
- `test_sleepy_parent.cpp` — lo scenario originale Child → Parent sleepy.
- `test_sleepy_router_purge.cpp` — regressione del bug #12. Verificato in entrambe le direzioni: fallisce senza il fix, passa con il fix.
- `test_sleepy_router_desync.cpp` — de-sync del peer e misura del tempo di trattenimento del messaggio (limite noto #20).
- `test_sleepy_router_chain.cpp` — catena Leader → relay → 3 Sleepy Router consecutivi.

---

## 4. Bug trovati lungo il percorso (catalogo sintetico)

Dettaglio e causa radice di ciascuno in `DESIGN_LOG.md` §4.

| # | Sintomo | Causa radice in una riga |
|---|---|---|
| 1 | Divisione per zero in `GetNextCslTransmissionDelay` | Un `Parent` aveva una copia locale dei campi CSL che il codice generico non leggeva. |
| 2 | Loop infinito di frame vuoti dopo la fusione dei `PrepareFrameFor*` | `error` inizializzato a `kErrorNotFound` invece di `kErrorNone`. |
| 3 | Messaggi per un Router "preparati" ma mai trasmessi | Il contatore di messaggi indiretti del Router non veniva mai incrementato. |
| 4 | Nodo bloccato in un loop di trasmissioni CSL | Lo stesso contatore non veniva mai decrementato. |
| 5 | Assert/use-after-free dopo il primo invio | `RemoveMessageIfNoPendingTx` ignorava i messaggi pendenti per un Router. |
| 6 | Un Router non risulta CSL-capable | `IsCslSupported()` accettava solo un Child con Parent ≥ 1.2. |
| 7 | La capacità CSL restava ferma dopo `sleepyrouter enable` | `SetCslCapable` era dentro `SetRole()`, mai richiamato senza cambio di ruolo. |
| 8 | Il Leader si stacca ~150 ms dopo l'abilitazione | `UpdateCslParameters` leggeva il Parent e mandava un Child Update Request senza un Parent valido. |
| 9 | La radio non entra mai in sleep | `UpdateIdleMode` guardava solo `RxOnWhenIdle`, non il CSL. |
| 10 | 100% di perdita con radio che dorme davvero (Address Query) | Il multicast accodava solo i Child, mai i Router. |
| 11 | La destinazione risolta ma il ping resta diretto e fallisce | La decisione diretto/indiretto era presa una sola volta, prima della risoluzione. |
| 12 | Leak di un messaggio quando un Router sparisce | `RemoveNeighbor` non aveva l'equivalente di `ClearAllMessagesForSleepyChild`. |
| 13 | SIGFPE su Nexus | Indirizzo invalido passato come "nessun filtro", rifiutato dalla piattaforma che lo valida. |
| 14 | Non compila su Thread 1.1 | Guardie `#if` più strette sulle funzioni che sui chiamanti. |
| 15 | Crash in una catena di 3 Sleepy Router | Slot singolo per "pendente per un Router" sovrascritto dal multicast. |
| 16 | Echo end-to-end su 4 hop non arriva | Nessuna conversione a CSL-indiretto per un hop *intermedio*; frame di puro inoltro mesh non costruiti. |
| 17 | Fallimento di sicurezza tra due Sleepy Router | Indirizzo MAC sorgente non impostato nel nuovo ramo. |
| 18 | Coda diretta bloccata su un Router che inoltra | `mDelayNextTx` non ripulito alla conversione a indiretto. |
| 19 | Fallimento di sicurezza intermittente | Un MLE Announce (Key ID Mode 2) con CSL IE non veniva cifrato da nessuno. Risolto in due parti. |
| 20 | Messaggio trattenuto per un Router irraggiungibile | **Limite noto**, non un bug: vedi §5. |

Una costante: molti di questi sono **assunzioni implicite di ruolo** ("chi arriva qui è sempre un Child sempre acceso con un solo Parent") che il codice stock dava per scontate.

---

## 5. Limiti noti

- **Bug #20.** Un messaggio in coda per un Router-peer irraggiungibile ma ancora vicino valido resta trattenuto fino all'aging del vicino (~100 s), poi viene pulito dal fix #12. Misurato con `desync`: de-sync a 560 ms, in coda a 30 s, vuota a 120 s. Deciso di non scrivere codice. Riserve: pressione sui buffer a scala non misurata; il callback di fine invio è ritardato; non verificato il comportamento dello stock con un Child che sparisce.
- **Filtro radio CSL a singolo peer.** Per un Router il filtro è un segnaposto: evita il crash ma non risolve il limite, rilevante solo su hardware reale.
- **Scala.** Verificata una catena di 3 Sleepy Router. Molti nodi e traffico concorrente non sono misurati.
- **Periodo CSL fisso**, nessun parametro configurabile.
- **Cifratura dell'Announce diretto** non osservata con una cattura dedicata: evidenza indiretta (12/12 e nessuna riga `security`/`MIC` nei log del test di catena, ma il livello di log Nexus potrebbe non mostrarle).

---

## 6. Da ripulire prima di una PR upstream

Non sono bug ma scorie di lavoro, individuate leggendo il diff:

- **Tuning non pertinente**: `MAC_DEFAULT_MAX_FRAME_RETRIES_DIRECT` (15→5), `MLE_CHILD_TIMEOUT_DEFAULT` (240→20).
- **Hack preesistenti** nello stesso diff: `period = 25000` in `data_poll_sender.cpp`, `BecomeDetached()` disabilitato in `mle.cpp`.
- **Codice morto o scartato**: blocco `#if 0` in `mesh_forwarder.cpp` (e il suo commento "NOTE" ormai falso), campi CSL commentati in `router.hpp`, bypass `IsSleepyRouterMode()` in `Mle::SetDeviceMode()`, `LogInfo` di debug in `csl_tx_scheduler.cpp`, e i commenti di lavoro nel codice di `RescheduleCslTx()` ("maybe this path needs to stay...").
- **Commenti**: marcatori `GAMA`, commenti in italiano, typo ("Wheter", "if for a csl-sync parent"), `// TODO: REENABLE OPENTHREAD_FTD MACRO`.
- **CLI**: la doc del comando punta a `#otThreadSetSleepyEndDeviceMode` (errore di copia), `#endif // OPENTHREAD` incompleto, e `cli.cpp` ha modifiche di sola formattazione (`region`, macro `CmdEntry`) probabilmente frutto di un `clang-format` di versione diversa da quella richiesta (19.1.7); la causa non è stata verificata.
- **File non pertinenti**: `doxygen/Doxyfile*`, `.gitignore`, i `.json` prodotti dai test.
- **Un solo commit grande più molto lavoro non committato**: da dividere in commit idiomatici.
- **`script/make-pretty check`** non è stato eseguito (richiede `clang-format` 19.1.7, non disponibile), così come gli altri `script/check-*` (POSIX, ARM, GN).

---

## 7. Stato della verifica (2026-10-06)

- `script/check-simulation-build`: `EXIT=0`, 21 configurazioni, zero errori (su una copia dei sorgenti: lo script va lanciato da una directory di build vuota e non funziona se la radice del repo ha già un `CMakeCache.txt` in-source).
- Test Nexus: `chain` 12/12; `peer`, `parent`, `purge`, `desync` passano.
- Verifica su OTNS (radio realmente addormentata): 20/20 ping, 0% packet loss, per Router↔Router e per Child→Parent (risultati del 2026-09-29, ripresi dal design log, non rieseguiti in questa sessione).

## 8. Cosa questo documento non garantisce

Il contenuto sul *codice* (§3) viene dalla lettura diretta del diff. Le *cause radice* dei bug (§4) e i risultati su OTNS (§7) vengono da `DESIGN_LOG.md` e dalle diagnosi fatte nelle sessioni precedenti: li ho riassunti, non rieseguiti. I numeri della verifica 2026-10-06 (build e Nexus) sono stati ottenuti in questa sessione.
