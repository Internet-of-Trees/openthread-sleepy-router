# Sleepy Router — Roadmap e decisioni di design

Documento vivo. Aggiornato ad ogni tappa completata (data + cosa è cambiato). Obiettivo: dare a chiunque (io in una sessione futura, un collaboratore, un potenziale reviewer OpenThread) il quadro completo senza dover rileggere tutta la cronologia di lavoro.

Vedi anche: `CSL_Sleepy_Router_Riassunto.pdf` (1 pagina, per spiegare rapidamente) e `CSL_Sleepy_Router_Dettagliato.pdf` (report tecnico esteso) — questo file è la versione *aggiornabile*, i PDF sono istantanee.

**Possibile destinazione:** questo lavoro potrebbe diventare una PR ufficiale verso OpenThread upstream, se va a buon fine. Le scelte di design vanno quindi valutate anche pensando a un reviewer esterno: preferire lo stile e gli idiomi già presenti nella codebase (niente RTTI/virtual dispatch, controlli di appartenenza a tabella tipo `ChildTable::Contains()`, macro `#if OPENTHREAD_FTD` / `#if OPENTHREAD_CONFIG_MAC_CSL_TRANSMITTER_ENABLE` coerenti con quelle già in uso) piuttosto che soluzioni rapide ma isolate.

---

## 1. Obiettivo

Due traguardi, in sequenza:

1. **Un Router può dormire via CSL restando Router** — pubblicizza un periodo CSL come farebbe un Parent verso un child sleepy, senza perdere il ruolo/eleggibilità di router nel protocollo.
2. **Chi è agganciato a un Router sleepy può parlargli usando CSL** invece di trasmissione diretta — inizialmente solo per la coppia Parent↔Child, ora in fase di generalizzazione verso **Router-Router (peer)**, con l'idea in più di valutare una sincronizzazione globale delle finestre di veglia per mitigare la latenza cumulativa multi-hop (vedi §6).

---

## 2. Terminologia rapida

- **CSL Receiver** (`OPENTHREAD_CONFIG_MAC_CSL_RECEIVER_ENABLE`) — il dispositivo dorme e si sveglia a finestre periodiche. Di serie: i child sleepy.
- **CSL Transmitter** (`OPENTHREAD_CONFIG_MAC_CSL_TRANSMITTER_ENABLE`) — il dispositivo accoda messaggi e li trasmette nella finestra di ascolto altrui. Di serie: il Parent verso i suoi child sleepy. In questo fork: anche un Child/Router verso un Parent/Router-peer sleepy.
- **`CslNeighbor`** (`neighbor.hpp`) — `class CslNeighbor : public Neighbor, public IndirectSender::NeighborInfo, public DataPollHandler::NeighborInfo, public CslTxScheduler::NeighborInfo` — rappresenta "un vicino con cui faccio CSL". Di serie solo `Child` ne eredita.
- **`IndirectSender`** — gestisce la coda dei messaggi indiretti e la loro preparazione/finalizzazione.
- **`CslTxScheduler`** — decide *quando* trasmettere CSL a chi, tra tutti i candidati CSL-sincronizzati.

---

## 3. Stato attuale (aggiornato 2026-09-24)

### Fatto
- [x] Router (FTD) può entrare in modalità Sleepy Router (`sleepyrouter enable/disable`, `Mle::SetSleepyRouterMode`)
- [x] Pipeline Child→Parent CSL funzionante end-to-end (verificato su OTNS, messaggi passano davvero)
- [x] **Tappa A** — `Router : public CslNeighbor` (invece di `: public Neighbor` diretto); rimossi i campi CSL duplicati storici su `Parent` (`mCslPeriod`/`mCslPhase`/`mCslSynchronized` + accessor) — ora forniti transitivamente da `Router→CslNeighbor`. Vedi §5.1. Nota: primo giro aveva tolto solo la lista di ereditarietà multipla (risolveva l'errore di compilazione) ma lasciato i vecchi accessor/campi locali dentro `Parent` — stesso bug di hiding riaperto silenziosamente (nessun errore di compilazione stavolta). Rimossi anche quelli il 2026-09-24 (per ora commentati, non cancellati — da ripulire).

### Fatto (continua)
- [x] **Tappa B** — `PrepareFrameForChild`/`PrepareFrameForParent` fuse in `PrepareFrameForCslNeighbor` generica; `PrepareFrameForParent` rimossa, `PrepareFrameForChild` mantenuta (serve ancora a `DataPollHandler`, percorso classico non-CSL — vedi nota sotto)
- [x] **Tappa C** — `HandleSentFrameToCslNeighbor` discrimina Child (`ChildTable::Contains`) vs Parent invece di castare ciecamente a `Child&`; branch MTD (`#else`) ora chiama `HandleSentFrameToCslParent` invece di non fare nulla
- [x] **Tappa D** — `RescheduleCslTx()` include il Parent nello stesso confronto dei Child (`bestNeighbor = &parent`); `HandleSentFrame()` usa un solo percorso generico via `mCslTxNeighbor`; `mSchedulingParent` e `CslTxScheduler::HandleSentFrameToParent` rimossi del tutto (verificato: zero riferimenti in tutto `src/core`)

### Fatto (continua, 2)
- [x] Verificato su OTNS: 13/13 ping riusciti, zero "CSL tx failed", zero loop di frame vuoti — pipeline unificata B+C+D funzionante end-to-end

### Fatto (continua, 3) — 2026-09-28
- [x] **Tappa E** — Router-peer CSL end-to-end funzionante e verificato su OTNS (0% packet loss, due nodi `router` forzati con `state router`, non più Leader+Child). Tre bug trovati e corretti lungo il percorso, tutti della stessa famiglia "conteggio/stato non aggiornato coerentemente":
  1. `AddMessageForSleepyRouter` non incrementava mai `mQueuedMessageCount` del Router (il Child lo fa transitivamente via `SourceMatchController::IncrementMessageCount`, che per Router non esiste — serve farlo a mano). Senza l'incremento, `RescheduleCslTx()` non selezionava mai il Router come `bestNeighbor` (`GetIndirectMessageCount() == 0` sempre vero) → messaggio "preparato" ma mai davvero trasmesso via CSL.
  2. `HandleSentFrameToCslRouter` cancellava `IsPendingForRouter()` direttamente sul messaggio invece di passare da `RemoveMessageFromSleepyRouter`, quindi non decrementava mai il contatore — bloccato per sempre a >0 dopo il primo invio riuscito → `RescheduleCslTx()` continuava a selezionare il Router anche a coda vuota → loop infinito a tempo zero di `TransmitDataCsl` (nodo si disconnette/si pianta in OTNS).
  3. `MeshForwarder::RemoveMessageIfNoPendingTx()` non controllava affatto `IsPendingForRouter` (guardava solo diretto/child-mask/parent) — un messaggio appena accodato per un Router veniva liberato/deallocato immediatamente dopo, lasciando un puntatore pendente in `aRouter.mIndirectMessage` (use-after-free, poi assert-crash al secondo tentativo di dequeue). Aggiunto `Message::IsPendingForAnyRouter()` e incluso nel controllo.
- [x] Gate minimale per il sonno fisico della radio (inizio Tappa F, vedi sotto): `Mle::mCslCapable` ora si attiva anche con `IsSleepyRouterMode()`, non solo `!IsRxOnWhenIdle()`.

### Fatto (continua, 4) — 2026-09-29, Tappa F completata e verificata con radio realmente addormentata
- [x] Catena completa dei quattro punti che bloccavano il sonno fisico risolta (`IsCslSupported()`, gate `SetCslCapable`/`SetCslPeriod` pilotato direttamente da `SetSleepyRouterMode()`, `Mac::UpdateCslParameters()` non più incondizionatamente Child-oriented, `Mac::UpdateIdleMode()` che ora considera anche `IsCslEnabled()`) — vedi `DESIGN_LOG.md` §3 Tappa F e §4 bug #6-#9 per il dettaglio completo di ciascuno.
- [x] Gap multicast-to-sleepy-router (bug #10, AddrQuery mai in coda indiretta per `RouterTable`) — risolto estendendo il ramo multicast di `SendMessage()` anche ai Router-peer CSL-sincronizzati.
- [x] Bug #11, trovato durante la verifica finale: un messaggio la cui destinazione richiede Address Resolution viene marcato diretto **una sola volta**, alla primissima chiamata di `SendMessage()` (quando il vicino non è ancora risolvibile) — `HandleResolved()` (stock) si limita a far ripartire il tentativo senza mai rivalutare se ora il vicino è un Router-peer CSL-sincronizzato, quindi ogni traffico che passa da Address Resolution (praticamente ogni primo contatto Router-Router) saltava sistematicamente il percorso CSL e falliva con 6 tentativi diretti NoAck. Risolto in `UpdateIp6RouteFtd()` (non in `SendMessage()`/`HandleResolved()`), stesso punto e stesso pattern già usato da OpenThread stock per il caso "ALOC che risulta essere un child sleepy" (righe 438-454) — questa funzione gira sia al primo tentativo sia a ogni retry dopo risoluzione, a differenza di `HandleResolved()`.
- [x] **Verificato su OTNS con radio fisicamente addormentata (non solo annuncio CSL-IE):** 20/20 ping riusciti, **packet loss = 0.0%**, RTT 151-711ms (min/avg/max = 151/235.000/711 ms), zero `NoAck`, zero `Dropping`, zero crash/assert. Transizioni di stato radio confermate nei log (`RadioState: Receive -> RadioSample`, `RadioSample -> CslTransmit`, cicli `CSL window start`/`CSL sleep` periodici reali) — non solo assenza di crash, prova diretta che il radio duty-cicla davvero.

### Fatto (continua, 5) — 2026-09-29
- [x] **§7.1 (packet loss Child→Parent) confermato risolto**: ripetuto il test originale (`add router` + `add med`, `sleepyrouter enable` sul router, 20 ping), risultato `20 packets transmitted, 20 packets received. Packet loss = 0.0%, RTT 17/78.300/139 ms`. Non è chiaro quale fix specifico l'abbia risolto per riflesso (probabile candidato: uno dei bug #3/#4/#5 sul contatore condiviso, o il #11 sulla Address Resolution) — non essendo mai stato isolato un fix dedicato, resta una nota per il paper più che una lezione tecnica precisa.
- [x] **§7.3/scenario "purge" risolto e coperto da test di regressione** (bug #12): `Mle::RemoveNeighbor()` non aveva, per un Router rimosso, l'equivalente di `ClearAllMessagesForSleepyChild()` — qualunque messaggio ancora in coda indiretta per quel Router (`Message::IsPendingForRouter()`) restava bloccato per sempre in `mSendQueue`, con rischio aggiuntivo di misconsegna se il suo `RouterId` veniva riassegnato a un Router diverso. Riprodotto con timing deterministico via script Python OTNS (`pylibs/unittests/test_sleepy_router_purge.py`: `ping` + `delete` del peer nello stesso istante simulato, poi `go(180)` oltre `kMaxNeighborAge`) — confermato sia via conteggio `bufferinfo` (43→40→42, un buffer mai liberato) sia via log (`Router 0xb000 - Link Accept timeout expired`, nessuna pulizia del messaggio accodato). Risolto aggiungendo `IndirectSender::ClearAllMessagesForSleepyRouter(Router&)` (mirror di `ClearAllMessagesForSleepyChild`) e richiamandolo dal ramo Router di `RemoveNeighbor()`. Test rilanciato dopo il fix: `43 == 43`, passa.
- [x] **Primo test Nexus scritto e passante**: `tests/nexus/test_sleepy_router_peer.cpp` — due Router (Leader + peer sleepy), verifica white-box: sync CSL imparato dal Leader, radio del peer che duty-cicla davvero, consegna end-to-end in entrambe le direzioni. Ha scoperto un tredicesimo bug (#13): `Mac::UpdateCslParameters()` passava `kShortAddrInvalid` a `otPlatRadioEnableCsl()` come "nessun filtro" — accettato silenziosamente da `ot-rfsim` (mai validato), ma **rifiutato** dalla piattaforma Nexus (che valida davvero i parametri), lasciando un disallineamento core/platform mai emerso su OTNS: `Mac` credeva il CSL abilitato con periodo reale, la piattaforma restava a periodo 0 → `SIGFPE` al primo frame trasmesso. Risolto passando il proprio RLOC16 come placeholder sintatticamente valido. `All tests passed`, exit code 0.
- [x] **Matrice Nexus ampliata a 3 test, tutti passanti**: aggiunti `test_sleepy_parent.cpp` (lo scenario originale Child→Parent-sleepy, Tappe A-D, mai testato automaticamente prima) e `test_sleepy_router_purge.cpp` (porting whitebox del test Python OTNS del bug #12). Nessun nuovo bug di prodotto trovato in questo giro, solo due sottigliezze di Nexus (tasklet asincrono per l'invio ICMP, `MessagePool::GetFreeBufferCount()` inutilizzabile su Nexus per via dell'heap esterno) — documentate nei commenti del codice dei test.
- [x] **Bug #14 trovato e risolto da `script/check-simulation-build`** (checklist di verifica finale, non un nuovo test): la variante Thread 1.1 (niente Header IE, niente CSL) non compilava — famiglia di guardie `#if` incoerenti, codice che chiama funzioni CSL gated correttamente ma i chiamanti gated solo da `#if OPENTHREAD_FTD`, mai esercitati finché nessuna build usata finora (ot-rfsim/OTNS, Nexus) provava mai Thread <1.2. Corretto in `mac.cpp`, `mac_frame.cpp`, `mesh_forwarder_ftd.cpp`, `mesh_forwarder_mtd.cpp`, `message.hpp` — dettaglio completo in design log §4. **Run finale completo confermato pulito**: tutte le 20 varianti di build dello script (Thread 1.1/1.4, NCP, multi-radio TREL, ecc.) passano senza errori.

### Non risolto / bloccante
(nessun item bloccante aperto al momento — vedi §6 del design log per il lavoro futuro non bloccante)

---

## 4. Roadmap a tappe

### Tappa A — Fondamenta strutturali ✅ fatta (2026-09-24)
`router.hpp`: `Router` eredita `CslNeighbor` invece di `Neighbor` direttamente. `Parent` (che eredita `Router`) riceve così i tre mixin (`CslTxScheduler::NeighborInfo`, `DataPollHandler::NeighborInfo`, `IndirectSender::NeighborInfo`) per transitività, senza doverli più dichiarare esplicitamente — rimossa la vecchia dichiarazione multipla su `Parent` e i campi CSL duplicati. Vedi §5.1 per il ragionamento e il bug che questo ha risolto.

### Tappa B — Unificare la preparazione del frame 🔄 in corso
File: `indirect_sender.hpp`/`.cpp`.
`PrepareFrameForChild` e `PrepareFrameForParent` sono risultate strutturalmente identiche (stessi accessor generici del mixin: `GetMacAddress`, `GetIndirectMessage`, `GetIndirectFragmentOffset`, `GetIndirectMessageCount` — nessun riferimento a `ChildTable`/`ChildSupervisor`). Fusa in una `PrepareFrameForCslNeighbor(Mac::TxFrame&, FrameContext&, CslNeighbor&)` senza cast (fatto, 2026-09-24).

**Attenzione, chiarito il 2026-09-24 dopo un errore mio (avevo cercato solo dentro `thread/`, non in tutto `src/core`):** `IndirectSender` è usata da **due** sottosistemi paralleli e diversi, non solo da `CslTxScheduler`:
- `mac/data_poll_handler.cpp` (`DataPollHandler::HandleFrameRequest`) — il meccanismo **classico** 802.15.4 di data-poll (senza CSL), che esiste solo nella relazione Parent-sempre-acceso → Child sleepy. `mIndirectTxChild` è tipizzato `Child*`, non `CslNeighbor*`. **Non generalizzabile e non va toccato** — un Parent o un Router-peer non "fanno mai il poll" in questo design, usano sempre e solo CSL.
- `thread/csl_tx_scheduler.cpp` — il nostro percorso CSL, quello che stiamo generalizzando.

Quindi: `PrepareFrameForChild` **resta** (serve a `DataPollHandler`, non è morta). Solo `PrepareFrameForParent` diventa superflua una volta che `CslTxScheduler::HandleFrameRequest` chiama `PrepareFrameForCslNeighbor` al posto suo — quella sì va rimossa. **Lezione generale:** prima di dichiarare una funzione "non più chiamata da nessuno", cercare in tutto `src/core`, mai in una sola sottocartella.

### Tappa C — Discriminare solo dove serve davvero
File: `indirect_sender.cpp`.
`HandleSentFrameToChild` **non** è pura (usa `ChildSupervisor`, `SourceMatchController`, bitmask multi-child) — resta separata da quella per Parent/Router-peer. Ma il suo dispatcher `HandleSentFrameToCslNeighbor` va corretto: invece del cast cieco a `Child&`, usare `Get<ChildTable>().Contains(aCslNeighbor)` (stesso idioma già usato in `mesh_forwarder_ftd.cpp::SendMessage()`) per instradare correttamente.

### Tappa D — Ritirare `mSchedulingParent`
File: `csl_tx_scheduler.hpp`/`.cpp`.
Con B e C fatte: `RescheduleCslTx()` include il Parent nello stesso loop/confronto dei Child (`bestNeighbor = &parent` è legale ora); `HandleFrameRequest()` perde il ramo `if (mSchedulingParent)`, un solo percorso generico; `HandleSentFrame()` usa solo la versione a 3 parametri già generica (di serie, non nostra). Si rimuovono: il campo `mSchedulingParent`, la funzione `HandleSentFrameToParent` (ridondante).

### Tappa E — Da "il mio Parent" a "un Router-peer qualsiasi"
File: `router_table.hpp`/`.cpp`, `mesh_forwarder_ftd.cpp`, `mesh_forwarder_mtd.cpp`, `indirect_sender.cpp`.
- `RescheduleCslTx()`: loop sui vicini di `RouterTable` CSL-sincronizzati (mirror del loop `ChildTable`), non più un solo controllo sul Parent singolo.
- `SendMessage()`: riconoscere "il prossimo salto è un Router-peer sleepy", non solo "sono Child del mio Parent".
- `AddMessageForSleepyParent`/`FindQueuedMessageForSleepyParent`/`RequestMessageUpdate`/`UpdateIndirectMessage`: generalizzare da `Parent&` esplicito. **Decisione aperta:** il bit singolo `Message::IsPendingForParent()` bastava con un solo Parent possibile; con più Router-peer serve qualcosa più vicino al bitmask dei Child (`GetIndirectTxChildMask()`) che a un bit singolo.

**Tentativo anticipato e rimandato (2026-09-24):** durante la Tappa D è stato scritto un primo abbozzo del loop su `RouterTable` dentro `RescheduleCslTx()`, poi rimosso deliberatamente per non mischiarlo con la verifica della Tappa D. Note utili per quando si riprende sul serio:
- **`RouterTable` non ha un `Iterate()` con filtro di stato** come `ChildTable` — espone solo `begin()`/`end()` grezzi su tutto l'array (`Router *begin(void)`, `Router *end(void)`, router_table.hpp:428-431). Il filtro sullo stato (es. escludere `kStateInvalid`) va fatto a mano dentro il loop, non c'è un equivalente diretto di `Get<ChildTable>().Iterate(Child::kInStateAnyExceptInvalid)`.
- Il loop, anche scritto correttamente, resterà **inerte** (nessun candidato trovato, `GetIndirectMessageCount()` sempre 0) finché non esiste un meccanismo di accodamento messaggi per i Router-peer (l'equivalente di `AddMessageForSleepyChild`/`AddMessageForSleepyParent`, ancora da progettare — vedi il punto sopra sul bit singolo vs bitmask).
- Decisione presa: costruire questa parte **insieme** al meccanismo di accodamento, non prima — per evitare codice "in attesa" senza copertura di test possibile, e per non mischiare debug di più pezzi contemporaneamente.

### Tappa F — Prerequisito fuori da `thread/` (indagine approfondita 2026-09-28)
File: `src/core/mac/sub_mac.cpp`, `src/core/mac/mac.cpp`, `src/core/thread/mle.cpp`.

**Correzione rispetto a quanto scritto sopra:** `OPENTHREAD_CONFIG_MAC_CSL_RECEIVER_ENABLE` **è già attivo** nel nostro build (`ot-rfsim/src/openthread-core-rfsim-config.h:112`, legato a `OPENTHREAD_CONFIG_THREAD_VERSION >= 1.2`, non a FTD/MTD) — non è "mai attivato per FTD", il meccanismo esiste ed è compilato. Il vero blocco è più a monte, in un solo punto:
```cpp
// mle.cpp:341 (prima della modifica di oggi)
Get<Mac::Mac>().SetCslCapable(IsCslSupported() && !IsRxOnWhenIdle());
```
`mIsCslCapable` è il flag che attiva davvero lo scheduling fisico di sleep/wake in `sub_mac.cpp`. Si accendeva solo con `!IsRxOnWhenIdle()` — e il nostro Sleepy Router **non tocca mai** `IsRxOnWhenIdle()` (resta sempre `true`), deliberatamente, per non compromettere l'eleggibilità/comportamento da Router.

**Perché non conviene flippare `IsRxOnWhenIdle()`/`DeviceMode` per farlo diventare `false` su un Router** (valutato e scartato il 2026-09-28):
1. Vincolo di spec: `DeviceMode::IsValid()` (`mle_types.hpp:481`) richiede `RxOnWhenIdle() == true` per ogni FTD — un FTD sleepy è formalmente non valido. `Mle::SetDeviceMode()` ha già un bypass pronto per `IsSleepyRouterMode()` ma **non viene mai usato** (nessuno chiama `SetDeviceMode()` da `SetSleepyRouterMode()`).
2. Il protocollo Router↔Router non ha comunque un canale per negoziarlo: in `mle_ftd.cpp:1055` e `:1365` (Link Request/Accept), il `DeviceMode` del Router-peer viene **hardcoded** a `FullThreadDevice | RxOnWhenIdle | FullNetworkData`, mai letto dal messaggio — quindi anche flippando il nostro bit, i peer non lo saprebbero mai da lì (per questo esiste il canale laterale via CSL-IE di Tappa E, ed è la scelta giusta).
3. `IsRxOnWhenIdle()` è usato ~20 volte in `mle.cpp` come proxy implicito di "sono un Child sleepy che parla col suo Parent" (es. `AppendSupervisionIntervalTlvIfSleepyChild()`, registrazione indirizzi multicast col parent) — flipparlo per un Router farebbe scattare comportamenti pensati solo per la relazione Child→Parent.

**Scelta presa:** non toccare `DeviceMode`/`IsRxOnWhenIdle()` per niente. Gate indipendente, aggiunto oggi:
```cpp
Get<Mac::Mac>().SetCslCapable(IsCslSupported() && (!IsRxOnWhenIdle() || IsSleepyRouterMode()));
```
(nota: la prima stesura aveva un bug di precedenza operatori — `&&` lega più stretto di `||`, quindi `A && B || C` andava letto `(A && B) || C`, non quello che si voleva; corretto con le parentesi esplicite).

**Non ancora verificato:** questo gate abilita solo la condizione — bisogna ancora testare su OTNS se il Router duty-cicla davvero la radio (non solo annuncia CSL-IE) e se il resto di `sub_mac.cpp` (pensato per un MTD child che non fa altro) regge quando applicato a un dispositivo che deve anche instradare traffico normale da altri vicini mentre "dorme".

**Idea valutata e scartata per ora, da tenere a mente:** durante l'indagine si è considerato se estendere anche ai Router-peer il meccanismo di accodamento indiretto per messaggi multicast (`MeshForwarder::SendMessage()`, ramo multicast, oggi solo per `ChildTable`) — per garantire la consegna di Advertisement/Link Request/Address Query a un Router che dorme davvero. Scartata dopo analisi (2026-09-28): il traffico realm-local (es. Address Query, ff03::2) passa per MPL, che ha già una propria ridondanza via retrasmissione Trickle, indipendente da `IndirectSender`; il traffico link-local (Advertisement, primo tentativo di Link Request) è periodico o ha già un fallback unicast (quello di Tappa E). Perdere una singola trasmissione multicast non è quindi chiaramente fatale. Inoltre l'implementazione non è banale: `Router` non ha un equivalente di `Child::HasIp6Address()` (i Router non registrano indirizzi come i child), quindi la condizione andrebbe ripensata da zero, non solo copiata. **Da riprendere solo se il test con radio davvero addormentata mostra perdita concreta di Advertisement/aggiornamenti di routing** — non implementare preventivamente senza evidenza.

---

## 5. Decisioni di design prese, e perché

### 5.1 — `Router : public CslNeighbor` invece di mixin diretti su `Parent` (2026-09-24)
**Alternativa scartata:** dare i tre mixin direttamente a `Parent` (mantenendo `Router : public Neighbor`) — soluzione iniziale, funzionante ma duplicava manualmente quello che `CslNeighbor` già offre a `Child`.
**Scelta:** cambiare l'ereditarietà di `Router` da `Neighbor` a `CslNeighbor`. Un solo cambio, una sola catena di ereditarietà (`Router → CslNeighbor → Neighbor`), nessun diamond — verificato che `Parent` è l'unica classe che eredita da `Router` in tutta la codebase.
**Bonus:** ha risolto un bug latente reale, non solo estetico. `ProcessCsl()` (mac.cpp) scriveva periodo/fase su `Parent` tramite accesso non qualificato (`parent->SetCslPeriod(...)`, tipo statico `Parent*`) — su un build FTD, dove la vecchia copia locale di `Parent` esisteva, questo risolveva per *hiding* C++ sulla copia locale. Ma `GetNextCslTransmissionDelay(const CslTxScheduler::NeighborInfo&, ...)` prende il parametro già tipizzato come classe base — l'hiding non si applica, quindi leggeva la copia *ereditata*, mai scritta da nessuno (sempre 0) → `periodInUs = 0` → divisione per zero in `radioNow % periodInUs`. Mai emerso perché i test sono sempre stati fatti con un Child **MTD** (`OPENTHREAD_FTD=0`, la copia locale duplicata di `Parent` non esisteva nemmeno). Sarebbe esploso alla prima vera schedulazione FTD — esattamente lo scenario Router-Router.

### 5.2 — Parent non eredita `CslNeighbor` direttamente (rivalutato, ora superato da §5.1)
Decisione originale (prime tappe): evitare `Parent : public Router, public CslNeighbor` perché avrebbe duplicato `Neighbor` (due strade, diamond, richiederebbe eredità virtuale — mai usata in questa codebase). Superata dalla soluzione di §5.1, che ottiene lo stesso risultato (Parent è-a CslNeighbor) senza il diamond, modificando `Router` invece di aggiungere una seconda strada su `Parent`.

### 5.3 — `mesh_forwarder_mtd.cpp::SendMessage()` — struttura a cascata
Per un messaggio IP6 unicast: se Child con parent CSL-sincronizzato → indiretto (`AddMessageForSleepyParent`); se Child ma parent non sincronizzato → diretto (fallback esplicito, altrimenti il messaggio resta senza marcatura e viene scartato silenziosamente da `RemoveMessageIfNoPendingTx`); se non Child affatto (Detached/Disabled) → diretto. Tre casi, tutti necessari — il secondo e il terzo sono stati bug reali trovati durante la review.

### 5.4 — Ordine delle operazioni in `SendMessage()` (mtd)
`ApplyDirectTxQueueLimit()` deve girare *dopo* aver deciso diretto/indiretto (controlla `IsDirectTransmission()` al suo interno, che se chiamata prima non è ancora impostata) e *dopo* `RemoveMessageIfNoPendingTx()` (non ha senso applicare un limite di coda a un messaggio che si sta per rimuovere).

### 5.5 — Bug trovato dopo la fusione B: `PrepareFrameForCslNeighbor` inizializzava `error` a `kErrorNotFound` invece di `kErrorNone` (2026-09-24)
La vecchia versione "dispatcher" di questa funzione (quella con lo `static_cast<Child&>` cieco, pre-Tappa B) partiva legittimamente da `kErrorNotFound`, perché su build senza `OPENTHREAD_FTD` non delegava a nessuno e doveva restituire comunque un errore sensato. Durante la fusione (Tappa B) il corpo è diventato quello di un'implementazione vera e propria (come `PrepareFrameForChild`/`PrepareFrameForParent` originali, che partivano da `kErrorNone`), ma l'inizializzazione non è stata aggiornata di conseguenza. Risultato: anche un frame preparato con successo tornava `kErrorNotFound`, `HandleFrameRequest` lo interpretava come fallimento (`VerifyOrExit(... == kErrorNone, frame = nullptr)`), e si ripeteva lo stesso loop di frame vuoti diagnosticato settimane prima per una causa diversa (allora era `mEnabled`, qui `error`). **Lezione:** quando si fonde una funzione-wrapper in un'implementazione vera, ricontrollare anche i valori di inizializzazione/default, non solo la logica del corpo.

**Risultato dopo il fix:** verificato su OTNS, 13/13 ping riusciti, zero retry CSL falliti, zero loop di frame vuoti — vedi §3, stato Tappa B/C/D.

---

## 6. Idea in sospeso: sincronizzazione globale delle finestre (stile TSCH)

Per mitigare la latenza cumulativa multi-hop (ogni hop verso un vicino sleepy può costare fino a un periodo CSL intero), si è discusso se far "dormire e svegliare insieme" tutti i dispositivi su una griglia temporale condivisa — pattern noto in letteratura come **TSCH / 6TiSCH**.

Requisiti reali, non banali:
- Serve una base temporale condivisa — OpenThread ha già `OPENTHREAD_CONFIG_TIME_SYNC_ENABLE` (default spento, mai usato in questo fork) come possibile building block.
- Serve un periodo uniforme nel gruppo sincronizzato (oggi ogni Router sceglie `kDefaultSleepyRouterCslPeriod` indipendentemente).
- Contesa nel canale: "tutti svegli insieme" senza turni assegnati causa collisioni — TSCH risolve con una matrice di slot temporali dedicati, non solo una finestra condivisa libera-per-tutti.

Nota interessante non ancora verificata: OTNS è un simulatore a eventi discreti, probabilmente `otPlatRadioGetNow()` è già un orologio unico condiviso senza drift reale — il che potrebbe already-rendere l'allineamento presente in simulazione anche senza costruire nulla apposta. Da verificare quando si torna su questo tema.

**Stato:** solo discusso, non affrontato — è una policy (come scegliamo periodo/fase) ortogonale al meccanismo delle Tappe A-F (come schedulo, dato un periodo/fase qualsiasi). Da riprendere dopo la Tappa E.

---

## 7. Problemi noti / tech debt

### 7.1 — Packet loss residua Child→Parent (NON RISOLTO)
Test OTNS: 20 ping, 3 ricevuti. Fallimenti: sempre 4 tentativi CSL consecutivi senza ack, poi abbandono. **Ipotesi testate ed escluse:** canale sbagliato (verificato via log diagnostico, canale sempre = PAN channel). **Ipotesi indebolita:** drift di sincronizzazione nel tempo (il Router calcola la fase su una griglia assoluta fissa dal tempo radio zero, dovrebbe restare valida indipendentemente da quanto tempo è passato). **Sospetto attuale, non verificato:** dato che il Router probabilmente non spegne mai la radio (vedi 7.5), il problema potrebbe essere nel modo in cui `ot-rfsim` gestisce una trasmissione con `TxDelay` impostato (schedulazione radio ritardata), più che nella logica di protocollo.

### 7.2 — Detach automatico disabilitato (rischioso)
`Mle::RetxTracker::RetryInfo::DetachIfMaxAttemptsReached()` (mle.cpp) — la chiamata a `BecomeDetached()` è commentata, sostituita con un log ("HACK: Max attempts reached, detach blocked"). Il device non si stacca più mai automaticamente sui retry MLE esauriti. Da riabilitare o sostituire con qualcosa di più mirato prima di un uso reale.

### 7.3 — Scenario "purge" non gestito
In `RequestMessageUpdate(Parent&)` non è gestito il caso in cui il cursore (`GetIndirectMessage()`) punti a un messaggio il cui `IsPendingForParent()` è nel frattempo diventato falso (equivalente al "Block A" della versione Child). Deciso di rimandarlo esplicitamente.

### 7.4 — Fallback pragmatico ancora presente
`MeshForwarder::HandleFrameRequest()` (mesh_forwarder.cpp, non quella di `CslTxScheduler`) contiene ancora il fix "storico" con cui è iniziato questo lavoro — `TxDelay` calcolato al volo per un frame diretto verso il parent sincronizzato, bypassando `CslTxScheduler`/`IndirectSender`. Tenuto come rete di sicurezza; da rimuovere solo dopo aver isolato e confermato che la pipeline nuova regge da sola (compreso risolvere 7.1).

### 7.5 — Router non implementa davvero il duty-cycling radio
Vedi Tappa F. Nessuna modifica di questo fork tocca `sub_mac.cpp`. `CSL_RECEIVER_ENABLE` è spento di default e mai attivato per FTD in questa build.

### 7.6 — `data_poll_sender.cpp`, valore fisso
`CalculatePollPeriod()`, ramo `mRetxMode`: periodo di poll sovrascritto con un valore fisso di 25000ms, indipendentemente dal calcolo precedente. Tech debt noto, non bloccante.

### 7.7 — Nessuna copertura di test automatica
Nessun test Nexus/unit per `CslTxScheduler`/`IndirectSender` lato Parent o Router-peer. Tutta la validazione finora è manuale via OTNS.

---

## 8. Come testare (promemoria workflow)

1. `cd /mnt/hdd/ot-ns/ot-rfsim && ./script/build_latest` — **obbligatorio dopo ogni modifica**, `cmake --preset simulation` nella repo `openthread` da solo non aggiorna i binari usati da OTNS (build separata, vedi `ot-rfsim/script/build_latest`, punta comunque allo stesso codice sorgente tramite `OT_DIR`).
2. `cd /mnt/hdd/ot-ns && otns` — va lanciato da questa directory (root del repo OTNS), altrimenti non trova i binari in `ot-rfsim/ot-versions/`.
3. `log debug`, `add router x 100 y 100`, `add med x 130 y 100`, `watch 1 2`, `go 10`, `node 1 "sleepyrouter enable"`, `go 10`, `ping 2 1 count 20 interval 1`.
4. Log completi (non troncati dalla CLI) su disco: `/mnt/hdd/ot-ns/tmp/0_<id>.log`.
