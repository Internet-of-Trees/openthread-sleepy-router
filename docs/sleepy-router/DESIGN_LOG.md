# Sleepy Router — Design Log e Technical Report

Documento tecnico "di sintesi": raccoglie, in forma organica e motivata, tutte le modifiche fatte a OpenThread in questo fork, il ragionamento dietro ogni scelta, i bug trovati (causa radice, non solo sintomo) e lo stato dei test. È il materiale grezzo da cui derivare sia il paper sia (eventualmente) la PR verso OpenThread upstream — a differenza di `ROADMAP.md`, che resta il tracker di lavoro informale/cronologico, qui l'obiettivo è la ricostruzione tecnica pulita, leggibile da chi non ha seguito il lavoro giorno per giorno.

Ultimo aggiornamento: 2026-09-29.

---

## 1. Motivazione e obiettivo

OpenThread implementa CSL (Coordinated Sampled Listening, Thread 1.2+) esclusivamente per la relazione **Parent (sempre acceso) → Child (sleepy)**: un Router genitore accoda messaggi e li trasmette nella finestra di ascolto periodica di un child a basso consumo. Un **Router** (dispositivo capace di instradare, eleggibile a inoltrare traffico per altri nodi) non può, di serie, entrare in modalità a basso consumo restando Router — l'assunzione strutturale del protocollo è che ogni Router sia sempre acceso.

Questo lavoro estende OpenThread in due passi:

1. **Un Router può dormire via CSL restando Router** — pubblicizza un periodo/fase CSL come farebbe un Parent verso un child, senza perdere il ruolo/eleggibilità di router nel protocollo Thread.
2. **Chi deve parlare a un Router sleepy lo fa via CSL** invece che con trasmissione diretta — generalizzando la stessa infrastruttura di accodamento/scheduling indiretto già usata per Parent→Child, prima al caso Child→Parent-sleepy, poi al caso generale **Router↔Router (peer)**.

Motivazione applicativa: reti Thread reali spesso hanno Router più della soglia ottimale (ridondanza fisica, copertura), molti dei quali instradano poco traffico ma restano comunque sempre accesi per requisito di protocollo — spreco energetico evitabile se un Router "di riserva" potesse dormire quando non serve, pur restando disponibile a fare da router quando richiesto.

**Possibile destinazione:** questo lavoro potrebbe diventare oggetto di un paper e/o di una PR verso OpenThread upstream. Le scelte di design sono quindi valutate pensando anche a un reviewer esterno: preferire lo stile e gli idiomi già presenti nella codebase (niente RTTI/virtual dispatch, controlli di appartenenza a tabella tipo `ChildTable::Contains()`, macro `#if OPENTHREAD_FTD` / `#if OPENTHREAD_CONFIG_MAC_CSL_TRANSMITTER_ENABLE` / `#if OPENTHREAD_CONFIG_MAC_CSL_RECEIVER_ENABLE` coerenti con quelle già in uso) piuttosto che soluzioni rapide ma isolate.

---

## 2. Terminologia e architettura di partenza

- **CSL Receiver** (`OPENTHREAD_CONFIG_MAC_CSL_RECEIVER_ENABLE`) — il dispositivo dorme e si sveglia a finestre periodiche. Di serie: solo i child sleepy.
- **CSL Transmitter** (`OPENTHREAD_CONFIG_MAC_CSL_TRANSMITTER_ENABLE`) — il dispositivo accoda messaggi e li trasmette nella finestra di ascolto altrui. Di serie: il Parent verso i suoi child sleepy. In questo fork: anche un Child/Router verso un Parent/Router-peer sleepy.
- **`CslNeighbor`** (`neighbor.hpp`) — `class CslNeighbor : public Neighbor, public IndirectSender::NeighborInfo, public DataPollHandler::NeighborInfo, public CslTxScheduler::NeighborInfo`, rappresenta "un vicino con cui faccio CSL". Di serie solo `Child` ne eredita.
- **`IndirectSender`** — gestisce la coda dei messaggi indiretti e la loro preparazione/finalizzazione.
- **`CslTxScheduler`** — decide *quando* trasmettere CSL a chi, tra tutti i candidati CSL-sincronizzati (lato trasmettitore).
- **`SubMac` / `sub_mac_csl_receiver.cpp`** — lato ricevitore: la macchina a stati che decide se il radio deve fisicamente dormire o campionare periodicamente una finestra CSL.
- **Layering rilevante per questo lavoro:** `Mle` (protocollo, ruoli, `DeviceMode`) → `Mac` (frame 802.15.4, IE CSL, stato CSL enable/disable) → `SubMac` (stato fisico del radio) → `Radio`/piattaforma (`otPlatRadioEnableCsl` ecc.). Molti dei bug descritti sotto nascono da assunzioni implicite che attraversano questi livelli senza essere mai state esplicitate come tali nel codice originale.

---

## 3. Le tappe (narrativa tecnica)

### Tappa A — `Router` eredita `CslNeighbor`

**Problema:** di serie `Router : public Neighbor` direttamente; `Parent : public Router` non aveva quindi accesso ai tre mixin CSL (`IndirectSender::NeighborInfo`, `DataPollHandler::NeighborInfo`, `CslTxScheduler::NeighborInfo`) che invece `Child` riceve tramite `CslNeighbor`.

**Alternativa scartata:** dare i tre mixin direttamente a `Parent` mantenendo `Router : public Neighbor` — soluzione iniziale, funzionante ma duplicava manualmente ciò che `CslNeighbor` già offre a `Child`, e non generalizzava a Router-non-Parent.

**Scelta:** `Router : public CslNeighbor` invece di `Router : public Neighbor`. Una sola catena di ereditarietà (`Router → CslNeighbor → Neighbor`), nessun diamante — verificato che `Parent` è l'unica classe che eredita da `Router` in tutta la codebase, quindi il cambio è sicuro.

**Bug reale scoperto come effetto collaterale, non solo estetico:** `Mac::ProcessCsl()` scriveva periodo/fase su `Parent` tramite accesso non qualificato (`parent->SetCslPeriod(...)`, tipo statico `Parent*`). Con la vecchia gerarchia, su un build FTD esisteva una copia locale duplicata dei campi CSL dentro `Parent` — l'assegnazione risolveva per *hiding* C++ su quella copia locale, "funzionando" per puro caso. Ma `GetNextCslTransmissionDelay(const CslTxScheduler::NeighborInfo&, ...)` prende il parametro già tipizzato come classe base: l'hiding non si applica lì, quindi leggeva la copia *ereditata* (mai scritta da nessuno, sempre 0) → `periodInUs = 0` → divisione per zero in `radioNow % periodInUs`. Il bug non era mai emerso perché tutti i test erano sempre stati fatti con un Child **MTD** (`OPENTHREAD_FTD=0`), dove la copia locale duplicata di `Parent` non esisteva nemmeno — sarebbe esploso alla prima vera schedulazione FTD, esattamente lo scenario Router-Router che stavamo per affrontare.

*Lezione generale, riproposta più volte in questo lavoro: un bug "silenzioso" che dipende da hiding/override non qualificato spesso resta invisibile finché non cambia la configurazione di build (qui: MTD→FTD) che lo rende osservabile.*

### Tappa B — Unificare la preparazione del frame

`PrepareFrameForChild` e `PrepareFrameForParent` erano strutturalmente identiche (stessi accessor generici del mixin: `GetMacAddress`, `GetIndirectMessage`, `GetIndirectFragmentOffset`, `GetIndirectMessageCount` — nessun riferimento a `ChildTable`/`ChildSupervisor`). Fuse in `PrepareFrameForCslNeighbor(Mac::TxFrame&, FrameContext&, CslNeighbor&)`, senza cast.

**Attenzione rilevante (errore mio corretto durante il lavoro, non dell'utente):** `IndirectSender` è usata da **due** sottosistemi paralleli e diversi:
- `mac/data_poll_handler.cpp` — il meccanismo **classico** 802.15.4 di data-poll (senza CSL), esiste solo nella relazione Parent-sempre-acceso → Child sleepy; `mIndirectTxChild` è tipizzato `Child*`, non generalizzabile (un Parent o un Router-peer non fanno mai il poll in questo design, usano sempre e solo CSL). `PrepareFrameForChild` **resta**, non è morta.
- `thread/csl_tx_scheduler.cpp` — il percorso CSL, quello generalizzato in questo lavoro.

**Bug trovato dopo la fusione:** `PrepareFrameForCslNeighbor` inizializzava `error` a `kErrorNotFound` invece di `kErrorNone`. La vecchia versione "dispatcher" (pre-fusione, con `static_cast<Child&>` cieco) partiva legittimamente da `kErrorNotFound` perché su build senza `OPENTHREAD_FTD` non delegava a nessuno. Durante la fusione il corpo è diventato un'implementazione vera (come gli originali `PrepareFrameForChild`/`PrepareFrameForParent`, che partivano da `kErrorNone`), ma l'inizializzazione non fu aggiornata di conseguenza — anche un frame preparato con successo tornava `kErrorNotFound`, interpretato come fallimento, con conseguente loop di frame vuoti. *Lezione: quando si fonde una funzione-wrapper in un'implementazione vera, ricontrollare anche i valori di default/inizializzazione, non solo la logica del corpo.*

### Tappa C — Discriminare solo dove serve davvero

`HandleSentFrameToChild` non è pura (usa `ChildSupervisor`, `SourceMatchController`, bitmask multi-child) — resta separata da quella per Parent/Router-peer. Il dispatcher `HandleSentFrameToCslNeighbor` è stato corretto per instradare correttamente invece di castare ciecamente a `Child&`, usando `Get<ChildTable>().Contains(aCslNeighbor)` (stesso idioma già usato altrove nella codebase). Il branch MTD (`#else`) chiama ora `HandleSentFrameToCslParent` invece di non fare nulla.

### Tappa D — Ritirare `mSchedulingParent`

Con B e C fatte, `RescheduleCslTx()` include il Parent nello stesso loop/confronto dei Child (`bestNeighbor = &parent` diventa legale); `HandleFrameRequest()` perde il ramo `if (mSchedulingParent)`, un solo percorso generico. Rimossi: il campo `mSchedulingParent`, la funzione ridondante `HandleSentFrameToParent`.

**Verifica intermedia:** 13/13 ping riusciti su OTNS, zero "CSL tx failed", zero loop di frame vuoti — pipeline Child↔Parent unificata funzionante end-to-end.

### Tappa E — Da "il mio Parent" a "un Router-peer qualsiasi"

Generalizzazione a Router-Router (peer), non più solo Child→Parent.

**Rappresentazione dello stato "pending" per Router:** con un solo Parent possibile, un bit singolo (`Message::mPendingForParent`) bastava. Con più Router-peer possibili serve poter dire "in attesa di consegna a **quale** router". Scelta: un campo a 6 bit (`mPendingRouterId`, `OT_NETWORK_MAX_ROUTER_ID` sta in 6 bit) con **codifica shift-by-uno** (`aRouterId + 1`, 0 = "nessuno") — permette al valore zero-inizializzato di default ("mai toccato") di restare distinguibile da "genuinamente in attesa per il router 0", esattamente come il default `false` di `mPendingForParent` funziona già gratuitamente. Alternativa scartata: un bitmask come `GetIndirectTxChildMask()` dei Child — avrebbe permesso a un messaggio di essere in coda per *più* Router-peer contemporaneamente, ma non è il caso d'uso reale (un messaggio unicast ha un solo prossimo salto) e avrebbe complicato la rappresentazione senza bisogno concreto.

**`RouterTable` non ha un `Iterate()` con filtro di stato** come `ChildTable` — espone solo `begin()`/`end()` grezzi su tutto l'array. Il filtro sullo stato (`Neighbor::kStateValid`) va fatto a mano dentro ogni loop che itera `RouterTable`, non c'è un equivalente diretto di `Get<ChildTable>().Iterate(Child::kInStateAnyExceptInvalid)`. Questo ha causato più di un bug (vedi §4).

**`SendMessage()` (mesh_forwarder_ftd.cpp), struttura a cascata per un messaggio IP6 unicast:**
1. Se sono Child e il mio Parent è CSL-sincronizzato → indiretto (`AddMessageForSleepyParent`).
2. Se sono Child ma il Parent non è sincronizzato → diretto (fallback esplicito).
3. Se non sono Child (sono Router/Leader) e il vicino è un Router CSL-sincronizzato → indiretto (`AddMessageForSleepyRouter`).
4. Altrimenti → diretto.

Ogni `else` è esplicito e obbligatorio: un `Message` non marcato né `SetDirectTransmission()` né aggiunto a una coda indiretta viene scartato silenziosamente da `RemoveMessageIfNoPendingTx` — questo "bug dei messaggi orfani" è ricorso più volte durante lo sviluppo (vedi §4) ed è la ragione per cui ogni ramo della cascata ha un `else` esplicito invece di un fallback implicito a fine funzione.

**`Mac::ProcessCsl()` — Router vs Parent, ordine critico:** un `Parent` è *anche* un `Router` (tramite `CslNeighbor`), quindi un lookup generico rischia di aggiornare l'oggetto sbagliato: un `Router` auto-popolato in `RouterTable` dal processing del TLV di rotta (che avviene anche mentre si è ancora in ruolo Child) invece del vero singleton `mParent`. Soluzione: il blocco che riconosce il Parent è reso esclusivo (termina con `ExitNow()`), e solo se quel blocco non matcha si prova il lookup generico su Router — tramite la `NeighborTable::FindNeighbor()` **pubblica** (non la `RouterTable::FindNeighbor()` privata, accessibile solo a `NeighborTable` come friend), che internamente sceglie `FindChildOrRouter` o `FindParent` in base al ruolo corrente, evitando l'ordinamento fragile di un lookup manuale.

**Risultato:** dopo la correzione dei bug in §4, 0% packet loss su un test reale con due nodi forzati in ruolo `router` (non il caso di default Leader+Child, che non esercita mai il percorso CSL — vedi §4 per la diagnosi di questo falso positivo).

### Tappa F — Il Router deve *davvero* addormentare il radio

Fino a questo punto, un "Sleepy Router" annunciava correttamente un periodo/fase CSL nel proprio IE in uscita (Tappa E) — sufficiente perché gli **altri** nodi instradassero correttamente il traffico verso di lui via CSL — ma non c'era prova che il **suo** radio smettesse mai davvero di ascoltare in modo continuo.

**Indagine su `IsRxOnWhenIdle()`/`DeviceMode` come possibile blocco strutturale** (poi scartata, con motivazione precisa — rilevante per un reviewer OpenThread):
1. **Vincolo di spec:** `DeviceMode::IsValid()` richiede `RxOnWhenIdle() == true` per ogni FTD — un FTD "sleepy" è formalmente non valido per lo schema del Mode TLV.
2. **Il protocollo Router↔Router non ha comunque un canale per negoziarlo:** in `mle_ftd.cpp` (Link Request/Accept), il `DeviceMode` del Router-peer viene hardcoded a `FullThreadDevice | RxOnWhenIdle | FullNetworkData` **senza mai leggerlo dal messaggio** — quindi anche flippando il proprio bit, i peer non lo saprebbero mai da lì.
3. **`IsRxOnWhenIdle()` è usato ~20 volte in `mle.cpp`** come proxy implicito di "sono un Child sleepy che parla col suo Parent" (es. `AppendSupervisionIntervalTlvIfSleepyChild()`, registrazione indirizzi multicast col parent) — flipparlo per un Router farebbe scattare comportamenti pensati solo per la relazione Child→Parent.

**Decisione:** non toccare mai `DeviceMode`/`IsRxOnWhenIdle()` per un Router. Tenerlo sempre `true` (soddisfa 1 e 2 automaticamente, evita 3) e costruire un canale di attivazione del sonno fisico **completamente indipendente**, agganciato solo a `IsSleepyRouterMode()`. Il CSL-IE side-channel di Tappa E era già, implicitamente, la stessa scelta architetturale applicata al lato "annuncio" — qui la si applica al lato "sonno fisico".

**Catena di quattro punti scoperti, ciascuno bloccava il successivo** (dettagli root-cause in §4):

1. `Mle::IsCslSupported()` era hardcoded a `IsChild()` — escludeva ogni Router a monte, prima ancora di arrivare al gate vero e proprio.
2. `Mac::SetCslCapable()`/`SetCslPeriod()` non venivano mai richiamati al momento giusto: vivono dentro `Mle::SetRole()`, che gira solo a un vero cambio di ruolo — non quando si abilita `sleepyrouter` su un nodo già Router.
3. `Mac::UpdateCslParameters()` assumeva incondizionatamente "se il CSL è abilitato, ho un Parent vero": leggeva un `Parent` mai popolato e programmava un `Child Update Request` verso di esso, che 100ms dopo, non trovando un parent valido, faceva scattare `BecomeDetached()` — un Leader/Router si staccava da solo pochi istanti dopo aver abilitato la modalità sleepy.
4. `Mac::UpdateIdleMode()` — il punto più profondo — decideva "vado a dormire o resto in ascolto" **solo** in base a `mRxOnWhenIdle`, ignorando completamente `mIsCslEnabled`. Anche con i tre punti sopra corretti, il radio non sarebbe mai entrato in stato `Sleep()`, perché la macchina di campionamento CSL (`SubMac::Sleep()` → `RadioSample()`) si attiva *solo* a partire da quello stato.

**Verifica fisica del sonno reale:** confermata leggendo le transizioni di stato del radio nei log (`RadioState: Receive -> RadioSample`, `CSL window start`/`CSL sleep` a intervalli periodici reali) — non solo l'assenza di crash, che da sola non prova nulla (un radio sempre acceso non si rompe mai, semplicemente non testa il vero meccanismo).

**Effetto collaterale reale, non ipotetico, del sonno effettivo:** l'Address Query (TMF, realm-local multicast ff03::2, veicolata via MPL/HopOpts) smette di arrivare in modo affidabile a un Router che dorme davvero, perché il percorso multicast di `SendMessage()` (`mesh_forwarder_ftd.cpp`) mette in coda indiretta solo per `ChildTable`, mai per `RouterTable` — un broadcast diretto one-shot ha una probabilità concreta di non coincidere con nessuna finestra di veglia del Router. Risolto estendendo lo stesso meccanismo di accodamento indiretto (già usato per il traffico unicast in Tappa E) anche al ramo multicast, per ogni Router-peer CSL-sincronizzato (esclusi i retx MPL, stesso filtro già usato per i Child) — vedi `mesh_forwarder_ftd.cpp`, ramo multicast.

**Secondo effetto collaterale reale, trovato subito dopo il primo:** anche con il fix del multicast, l'Address Query stessa ora arriva ed è risolta correttamente, ma il traffico ICMP che la triggerava continuava ad andare **diretto** e a fallire con 6 tentativi NoAck. Causa: `SendMessage()` decide diretto-vs-indiretto una sola volta, alla primissima chiamata — quando la destinazione richiede ancora Address Resolution, `Get<NeighborTable>().FindNeighbor(destination)` restituisce `nullptr` (il vicino non è ancora noto), quindi il messaggio cade nel fallback `SetDirectTransmission()`. Una volta risolto l'indirizzo, `MeshForwarder::HandleResolved()` (stock) si limita a cancellare `IsResolvingAddress()` e far ripartire il tentativo — **non rivaluta mai** se ora, con la destinazione nota, il vicino è un Router-peer CSL-sincronizzato. Per un Child questo non è mai stato un problema (gli indirizzi dei propri child sono noti a priori, via registrazione); per Router-Router la scoperta del vicino passa proprio dall'Address Resolution, quindi qualunque primo contatto saltava sistematicamente il percorso CSL. Risolto in `UpdateIp6RouteFtd()` (`mesh_forwarder_ftd.cpp`), non in `SendMessage()`/`HandleResolved()`: questa funzione gira sia al primo tentativo sia a ogni retry dopo la risoluzione (via `PrepareNextDirectTransmission()`), ed è esattamente lo stesso punto dove OpenThread stock già gestisce il caso analogo "la destinazione ALOC risulta essere un child sleepy di questo device" (righe 438-454 dello stesso file) — convertendo il messaggio da diretto a indiretto lì, con lo stesso pattern (`AddMessageForSleepyRouter` + `Message::ClearDirectTransmission()`), invece di duplicare la logica di decisione in un secondo punto.

**Nota sul filtro radio a livello hardware (rilevante solo per un porting a hardware reale/futuro upstream, non per i test in simulazione):** `otPlatRadioEnableCsl(period, shortAddr, extAddr)` — sia l'API di piattaforma sia `SubMac` (`mCslPeerShort`, campo singolare) sono progettati per **un solo peer** filtrato durante la finestra CSL — coerente con l'assunzione "Child ascolta solo il suo Parent". Un Router sleepy con più Router-peer non ha un singolo peer da filtrare. Verificato che il radio simulato di `ot-rfsim` (`src/radio.c::otPlatRadioEnableCsl`) ignora completamente `aShortAddr`/`aExtAddr` (`OT_UNUSED_VARIABLE`), quindi per i test in OTNS questo non è un problema pratico — lo diventerebbe solo con un chip radio reale che implementa davvero il filtro hardware. **Lasciato esplicitamente come lavoro futuro**, non affrontato in questo giro: `Mac::UpdateCslParameters()` passa `kShortAddrInvalid`/ext-address azzerato (nessun filtro) per il caso Router.

---

## 4. Catalogo dei bug trovati (causa radice, non solo sintomo)

Elencati nell'ordine in cui sono emersi. Per ciascuno: sintomo osservato, causa radice, perché non era mai emerso prima, fix.

| # | Tappa | Sintomo | Causa radice | Perché non emerso prima | Fix |
|---|-------|---------|---------------|--------------------------|-----|
| 1 | A | Divisione per zero in `GetNextCslTransmissionDelay` su build FTD | `ProcessCsl()` scriveva su `Parent` per accesso non qualificato, hiding C++ su una copia locale dei campi CSL mai letta dal codice generico | Test storici sempre su Child MTD, dove la copia locale non esisteva | `Router : public CslNeighbor`, rimossa la copia duplicata |
| 2 | B | Loop infinito di frame vuoti dopo la fusione di `PrepareFrameFor*` | `PrepareFrameForCslNeighbor` inizializzava `error = kErrorNotFound` invece di `kErrorNone`, ereditato dal vecchio dispatcher | La fusione ha cambiato la semantica della funzione (da wrapper a implementazione) senza aggiornare l'init | `error = kErrorNone` |
| 3 | E | Messaggi verso Router-peer "preparati" ma mai davvero trasmessi via CSL | `AddMessageForSleepyRouter` non incrementava mai `mQueuedMessageCount` del Router (per Child avviene transitivamente via `SourceMatchController::IncrementMessageCount`, inapplicabile a Router) — `RescheduleCslTx()` non selezionava mai il Router come `bestNeighbor` (`GetIndirectMessageCount() == 0` sempre vero) | Il meccanismo di conteggio non era mai stato esercitato per un Router prima | `aRouter.IncrementIndirectMessageCount()` esplicito in `AddMessageForSleepyRouter` |
| 4 | E | Nodo si pianta/disconnette in OTNS con loop infinito a tempo-zero di `TransmitDataCsl` dopo il primo invio riuscito | `HandleSentFrameToCslRouter` cancellava `IsPendingForRouter()` direttamente sul messaggio invece di passare da `RemoveMessageFromSleepyRouter`, quindi non decrementava mai il contatore — bloccato per sempre a >0, `RescheduleCslTx()` continuava a selezionare il Router anche a coda vuota | Stesso conteggio del bug #3, mai esercitato fino a due invii di seguito allo stesso Router | `aRouter.DecrementIndirectMessageCount()` esplicito in `HandleSentFrameToCslRouter` |
| 5 | E | Assert-crash (`OT_ASSERT(aMessage.IsInAPriorityQueue())`, use-after-free) subito dopo il primo invio riuscito | `MeshForwarder::RemoveMessageIfNoPendingTx()` non controllava affatto `IsPendingForRouter` (solo diretto/child-mask/parent) — un messaggio appena accodato per un Router veniva liberato/deallocato immediatamente dopo, lasciando un puntatore pendente in `aRouter.mIndirectMessage` | La funzione precede Tappa E, mai aggiornata per il nuovo stato "pending for router" introdotto insieme | Aggiunto `Message::IsPendingForAnyRouter()` e incluso nel controllo |
| 6 | F | Router non selezionabile come CSL-capable | `Mle::IsCslSupported()` hardcoded `IsChild() && GetParent().IsThreadVersion1p2OrHigher()` | Prerequisito mai raggiunto prima: nessun Router aveva mai provato ad abilitare il CSL receiver | `|| IsSleepyRouterMode()` nell'espressione |
| 7 | F | `mIsCslCapable` restava congelato al vecchio valore dopo `sleepyrouter enable` su un nodo già Router | `SetCslCapable()` vive dentro `Mle::SetRole()`, mai richiamato da un semplice cambio di modalità senza cambio di ruolo | Stesso motivo del #6 | `SetSleepyRouterMode()` richiama direttamente `SetCslCapable()`/`SetCslPeriod()` |
| 8 | F | Leader/Router si stacca da solo (`Role leader -> detached`) ~150ms dopo aver abilitato `sleepyrouter` | `Mac::UpdateCslParameters()` legge incondizionatamente `Get<Mle::Mle>().GetParent()` (mai popolato per un Leader) e schedula incondizionatamente un Child Update Request verso di esso; non trovando un parent valido, MLE reagisce staccandosi | Il codice presuppone "CSL abilitato ⇒ sono un Child", mai falsificato prima d'ora | Separato il caso `IsChild()` (comportamento originale) dal caso Router (nessun filtro, nessuna richiesta) |
| 9 | F | Radio non entra mai in `Sleep()`/`RadioSample()` nonostante `mIsCslEnabled == true` | `Mac::UpdateIdleMode()` decide `shouldSleep` solo da `mRxOnWhenIdle`, mai da `mIsCslEnabled` | Per ogni Child sleepy `mRxOnWhenIdle` è già `false`, quindi la distinzione non contava mai prima di un Router (che lo tiene `true` di proposito) | `shouldSleep = shouldSleep \|\| (IsCslEnabled() && !mPromiscuous)` |
| 10 | F | 100% packet loss (Address Query mai ricevuta) con radio davvero addormentata, nessun crash | Il ramo multicast di `SendMessage()` mette in coda indiretta solo `ChildTable`, mai `RouterTable` — un Router che dorme davvero perde il broadcast diretto one-shot se non coincide con una sua finestra di veglia | Finché il radio non dormiva mai davvero (bug #9 non ancora risolto), il broadcast diretto arrivava sempre, mascherando il problema | Esteso il loop di accodamento indiretto anche a `RouterTable`, stesso filtro MPL-retx dei Child |
| 11 | F | Address Query risolta correttamente, ma il ping continuava comunque ad andare diretto e a fallire con 6 tentativi NoAck | `SendMessage()` decide diretto/indiretto una sola volta, quando la destinazione non è ancora risolvibile (`FindNeighbor()` → `nullptr`) → fallback diretto permanente; `HandleResolved()` (stock) non rivaluta mai la decisione dopo la risoluzione | Per un Child l'indirizzo del destinatario è sempre noto a priori (registrazione); solo per Router-Router la scoperta passa da Address Resolution, quindi solo lì la finestra temporale "deciso prima di sapere chi è il vicino" diventa osservabile | Aggiunta la stessa conversione diretto→indiretto in `UpdateIp6RouteFtd()` (gira anche sui retry post-risoluzione), stesso pattern già usato da OT stock per il caso ALOC→child sleepy |
| 12 | Purge | Messaggio indiretto per un Router-peer mai liberato dopo che il Router scompare dalla rete (leak permanente di un buffer, confermato via `bufferinfo` e log — vedi test di regressione) | `Mle::RemoveNeighbor()` ha, per un Child rimosso, la chiamata a `ClearAllMessagesForSleepyChild()`; per un Router rimosso (ramo `else if (aNeighbor.IsStateValid())`, mle_ftd.cpp) non c'era alcun equivalente | Lo scenario "Router-peer che scompare mentre ha messaggi in coda" non era mai stato esercitato prima — nessun test, nessuna occasione empirica di osservarlo | Aggiunta `IndirectSender::ClearAllMessagesForSleepyRouter(Router&)` (mirror di `ClearAllMessagesForSleepyChild`), richiamata dallo stesso punto in `RemoveNeighbor()` |
| 13 | Nexus | `SIGFPE` (divisione per zero) in `ComputeCslPhase()` (`examples/platforms/utils/mac_frame.cpp`, codice stock) al primo frame trasmesso da un Sleepy Router nel test Nexus | `Mac::UpdateCslParameters()` passa `kShortAddrInvalid` a `otPlatRadioEnableCsl()` come "nessun filtro" per il caso Router (nessun singolo peer da filtrare) — la piattaforma simulata `ot-rfsim` ignora silenziosamente gli argomenti indirizzo, ma la piattaforma **Nexus** li valida davvero e rifiuta la chiamata, lasciando il proprio `mCslPeriod` a 0 mentre `Mac` crede che il CSL sia abilitato con periodo reale; nessun errore propagato indietro da `SetCslParams()` | Mai testato su una piattaforma (Nexus) che validasse davvero i parametri — `ot-rfsim`/OTNS avevano sempre mascherato il problema | Passato il proprio RLOC16 come indirizzo placeholder sintatticamente valido al posto di `kShortAddrInvalid` (non risolve la limitazione di fondo del filtro a singolo peer, §6, solo evita il crash) |
| 14 | check-simulation-build | Compilazione FTD fallisce (`script/check-simulation-build`, variante Thread 1.1): "no declaration matches" per `SetCslIe`/`HasCslIe`/`GetCslIe` in `mac_frame.cpp`; "has no member named" per `IsCslSynchronized`/`GetCslPeriod`/`AddMessageForSleepyRouter`/`AddMessageForSleepyParent` in `mesh_forwarder_ftd.cpp`/`_mtd.cpp`; e per `mPendingRouterId` in `message.hpp` | Famiglia di guardie `#if` incoerenti, tutte con la stessa forma: codice che chiama funzioni/campi CSL gated correttamente dietro `OPENTHREAD_CONFIG_MAC_HEADER_IE_SUPPORT` e/o `OPENTHREAD_CONFIG_MAC_CSL_TRANSMITTER_ENABLE`, ma i **chiamanti** erano gated solo da `#if OPENTHREAD_FTD` (o, per `message.hpp`, i 4 accessor di `mPendingRouterId` non erano gated affatto mentre il campo sì). CSL è strutturalmente impossibile senza Header IE (funzionalità 802.15.4-2015/Thread≥1.2) — `HEADER_IE_SUPPORT` si auto-deriva a 0 per un build Thread 1.1, portando con sé a cascata l'assenza di tutte le funzioni CSL, non solo quelle esplicitamente `CSL_*_ENABLE`-gated | Nessuna configurazione di build finora testata in questo progetto (ot-rfsim/OTNS, Nexus) usa mai Thread <1.2 — `script/check-simulation-build` è la prima volta che una configurazione Thread 1.1 (esplicitamente supportata e testata da OpenThread stock) viene esercitata contro il codice di questo fork | Aggiunto il gate mancante (`OPENTHREAD_CONFIG_MAC_HEADER_IE_SUPPORT` o `OPENTHREAD_CONFIG_MAC_CSL_TRANSMITTER_ENABLE`, a seconda di cosa richiede davvero la funzione chiamata) in `mac.cpp::BeginTransmit()`, `mac_frame.cpp` (2 punti), `mesh_forwarder_ftd.cpp` (3 punti, con fallback `else` a `SetDirectTransmission()`), `mesh_forwarder_mtd.cpp` (1 punto, stesso fallback); reso incondizionato il campo `mPendingRouterId` in `message.hpp`, allineandolo allo stile già usato dal suo omologo `mPendingForParent` (mai gated) invece di rincorrere ogni singolo chiamante |

**Pattern trasversale, utile per la sezione "lessons learned" di un paper:** la maggioranza di questi bug (3, 4, 5, 8, 9, 10) condivide la stessa forma — codice scritto assumendo implicitamente "l'unico dispositivo che può trovarsi in questo stato è un Child con un singolo Parent", mai reso esplicito come precondizione, mai controllato a runtime. Estendere lo stesso meccanismo a un ruolo diverso (Router) non causa errori di compilazione — le assunzioni erano tutte a runtime, non nel sistema di tipi — e quindi ciascuna è emersa solo empiricamente, una alla volta, ogni volta che il codice raggiungeva per la prima volta lo stato non prima raggiungibile.

---

## 5. Stato attuale (2026-09-29, aggiornato con verifica finale)

- [x] Tappe A-D: pipeline Child↔Parent CSL unificata e verificata (13/13 ping).
- [x] Tappa E: pipeline Router↔Router CSL end-to-end verificata.
- [x] Tappa F: il Router duty-cicla davvero il radio — verificato sia a livello di transizioni di stato (`Receive -> RadioSample`, `RadioSample -> CslTransmit`, cicli `CSL window start`/`CSL sleep` periodici) sia a livello applicativo, nessun detach spurio, nessun crash.
- [x] Gap multicast-to-sleepy-router risolto (bug #10) e verificato.
- [x] Bug della "seconda occasione" mancante su Address Resolution risolto (bug #11) e verificato.
- [x] **Test end-to-end con radio fisicamente addormentata, risultato confermato dal log (non aneddotico):**
  ```
  20 packets transmitted, 20 packets received. Packet loss = 0.0%.
  Round-trip min/avg/max = 151/235.000/711 ms
  ```
  Zero `NoAck`, zero `Dropping`, zero crash/assert, in un test da 20 ping a intervallo 1s tra due nodi entrambi in ruolo Router, entrambi con `sleepyrouter enable` e radio duty-ciclata verificata nei log.
- [x] **Il vecchio problema Child→Parent (~85% packet loss in un test precedente, mai risolto in modo isolato) risulta risolto**: ripetuto il test originale (Router + `med`, `sleepyrouter enable` sul router, 20 ping) — `20 packets transmitted, 20 packets received. Packet loss = 0.0%, RTT 17/78.300/139 ms`. Nessun fix di questa sessione è stato scritto pensando esplicitamente a questo problema, quindi è stato risolto per riflesso da uno degli altri fix (candidati più probabili: bug #3/#4/#5, che toccano lo stesso meccanismo di conteggio/scheduling condiviso da Child e Router; oppure #11). Non isolabile a posteriori senza rifare i test bisecando i commit — nota utile per il paper più che una lezione tecnica precisa da poter argomentare con certezza.
- [x] **Scenario "purge" (Router che lascia la rete mentre ha messaggi in coda) riprodotto, risolto e coperto da un test di regressione** (bug #12) — script Python OTNS deterministico (`pylibs/unittests/test_sleepy_router_purge.py`, non manuale/a tempo): ping accodato indirettamente per un Router-peer, quel Router cancellato nello stesso istante simulato (nessun `go()` in mezzo, quindi il messaggio è garantito ancora in coda), poi tempo avanzato oltre `kMaxNeighborAge` (100s) + retry Link Request + Link Accept timeout perché `Mle::RemoveNeighbor()` scatti davvero. Prima del fix: un buffer su 44 restava bloccato per sempre (`bufferinfo`: 43→40→42, mai tornato a 43), confermato anche nel log (`Router 0xb000 - Link Accept timeout expired`, nessuna pulizia successiva). Dopo il fix (`IndirectSender::ClearAllMessagesForSleepyRouter`, mirror di `ClearAllMessagesForSleepyChild`, richiamata dal ramo Router di `RemoveNeighbor()`): il test passa, `43 == 43`.
- [x] **Primo test Nexus del fork scritto, corretto e passante**: `tests/nexus/test_sleepy_router_peer.cpp` (`ot_nexus_test(sleepy_router_peer ...)` in `CMakeLists.txt`) — due Router (Leader + peer), il peer abilita Sleepy Router mode, verifica white-box che il Leader impari lo schedule CSL del peer (`IsCslSynchronized()`), che il radio del peer duty-cicli davvero (`mRadio.mState == kStateSleep` campionato su più finestre CSL), e consegna end-to-end in entrambe le direzioni (compreso Leader→peer, l'unica che richiede davvero la coda indiretta CSL). Nel farlo, ha scoperto un tredicesimo bug (#13, §4) — un `SIGFPE` mai visto su OTNS/ot-rfsim perché quella piattaforma non valida mai i parametri di `otPlatRadioEnableCsl()`, mentre Nexus sì — risolto. `All tests passed`, exit code 0.
- [x] **Matrice di test Nexus ampliata a tre scenari, tutti passanti:**
  - `test_sleepy_router_peer.cpp` — caso base Router-peer (§5 sopra).
  - `test_sleepy_parent.cpp` — lo scenario *originale* Child→Parent-sleepy (Tappe A-D), mai coperto da alcun test automatico prima d'ora (solo validazione manuale OTNS, incluso il mistero packet-loss §7.1 mai isolato). Verifica white-box: il Child impara lo schedule CSL del Parent, il Parent duty-cicla davvero pur avendo un Child sempre-acceso agganciato, consegna end-to-end in entrambe le direzioni.
  - `test_sleepy_router_purge.cpp` — porting Nexus del test Python OTNS del bug #12, con API whitebox invece di CLI/log scraping: `Core::SetNodeEnabled()` per far sparire il peer nello stesso istante simulato di un `SendEchoRequest()` fire-and-forget, `MeshForwarder::GetQueueInfo()` per il conteggio preciso dei messaggi in coda (non `MessagePool::GetFreeBufferCount()`, che su Nexus torna sempre il sentinel "illimitato" essendo configurato con heap esterno — scoperto scrivendo questo test). Conferma: 0→1 messaggi dopo l'accodamento, 1→0 dopo il timeout — il fix regge.
  - Lungo la scrittura di questi due test nuovi sono emerse due sottigliezze di Nexus non ovvie, documentate nei commenti del codice: (1) `Icmp6::SendEchoRequest()` accoda il messaggio in un tasklet, non sincronamente — serve un `AdvanceTime(0)` per "svuotare" i tasklet pendenti senza far avanzare il tempo simulato; (2) la sincronizzazione CSL tra due nodi va forzata esplicitamente con un ping in una direzione (mai aspettata passivamente via Trickle timer, per restare deterministici).
- [x] **`script/check-simulation-build` — 20/20 varianti passano, pulito.** Lanciato per la prima volta su questo fork, ha trovato bug #14 (§4): la compilazione FTD falliva del tutto sulla variante Thread 1.1 (mai testata prima, dato che ogni configurazione usata finora — ot-rfsim/OTNS, Nexus — usa Thread ≥1.2). Corretto in 5 file (`mac.cpp`, `mac_frame.cpp`, `mesh_forwarder_ftd.cpp`, `mesh_forwarder_mtd.cpp`, `message.hpp` — dettaglio in §8). Run finale completo confermato pulito: tutte le 20 combinazioni build/feature testate dallo script (Thread 1.1 in tutte le sue varianti, Thread 1.4 con backbone router/NAT64/CSL receiver/border routing/heap esterno, NCP, multi-radio TREL, ecc.) — zero `FAILED`, zero `error:`, zero `ninja: build stopped` su 26540 righe di log.
- [ ] **`script/make-pretty check` non verificabile in questo ambiente**: richiede `clang-format` 19.1.7 esatto, mentre è installato solo `clang-format 22.1.8` (`clang-format-19` assente dal sistema) — problema di toolchain dell'ambiente, non del codice, ma non c'è stato modo di confermare la conformità di formattazione dei file modificati. Da rifare in un ambiente con la versione corretta prima di una submission.

---

## 6. Limiti noti e lavoro futuro (materiale per la sezione "future work" del paper)

1. **Filtro radio a singolo peer per CSL receiver** (§3, Tappa F) — non affrontato, non necessario per la simulazione, rilevante solo per hardware reale.
2. **Scenario "purge" — parzialmente risolto.** Il caso principale (un Router lascia `RouterTable`, es. per link timeout, con messaggi ancora in coda) è **risolto e coperto da test di regressione** — bug #12, §4. Resta aperto un sotto-caso più stretto, mai testato: `RequestMessageUpdate(Router&)`/`RequestMessageUpdate(Parent&)` non gestiscono esplicitamente il caso in cui il cursore (`GetIndirectMessage()`) punti a un messaggio il cui stato "pending" sia diventato falso nel frattempo (senza che il Router lasci `RouterTable`) — né esiste un equivalente di `HandleChildModeChange` per un Router che cambia ruolo o perde la sincronizzazione CSL restando comunque un vicino valido. Non è chiaro se questo sia effettivamente raggiungibile nella pratica o solo teorico; da verificare con un test dedicato se si vuole chiudere anche questo residuo prima di una PR.
3. **Sincronizzazione globale delle finestre (stile TSCH/6TiSCH)** — idea discussa per mitigare la latenza cumulativa multi-hop, non affrontata; richiederebbe una base temporale condivisa (`OPENTHREAD_CONFIG_TIME_SYNC_ENABLE`, mai usato in questo fork) e una politica di contesa nel canale.
4. **Detach automatico disabilitato** (`Mle::RetxTracker::RetryInfo::DetachIfMaxAttemptsReached()`) — sostituito con un log ("HACK"), rischioso, da riabilitare o sostituire con qualcosa di più mirato prima di un uso reale/una PR.
5. **`MeshForwarder::HandleFrameRequest()` (non quella di `CslTxScheduler`)** contiene ancora il fix pragmatico storico (`TxDelay` calcolato al volo per un frame diretto verso il parent sincronizzato) con cui è iniziato questo lavoro — da rimuovere solo dopo aver isolato e confermato che la pipeline nuova regge da sola.
6. **Copertura di test automatica** — parzialmente risolta: 3 test Nexus (§5) + 1 test Python OTNS coprono i casi base Router-peer, Child→Parent-sleepy e purge. Restano scoperti: `script/check-simulation-build`/altre varianti di configurazione (es. Thread 1.1, vedi bug #14) non fanno parte di un ciclo di regressione automatico dedicato a questo fork — vanno rilanciate a mano dopo ogni modifica; scenari a più di 2 nodi; il sotto-caso residuo del punto 2 sopra.
7. **Guardie di compilazione (`#if`) per configurazioni non standard** — bug #14 (§4) ha mostrato che il codice di questo fork non era mai stato compilato per una configurazione Thread <1.2 (niente Header IE, niente CSL) prima d'ora. Il fix applicato copre i punti trovati da un singolo giro di `script/check-simulation-build`; non è escluso che esistano altri punti simili non ancora esercitati da nessuna build provata finora (es. build con `OPENTHREAD_CONFIG_MAC_CSL_TRANSMITTER_ENABLE=0` ma `HEADER_IE_SUPPORT=1`, o altre combinazioni non standard) — da tenere presente prima di una PR, dato che OpenThread CI testa molte più combinazioni di quelle esercitate finora in questo fork.

---

## 7. Note per la stesura del paper / della PR

- Il §4 (catalogo bug) è probabilmente la sezione con più valore per un paper — ogni bug è un caso di studio di "assunzione implicita di ruolo non verificata", un pattern generalizzabile oltre questo specifico fork. Il bug #14 estende questo pattern dal runtime al *momento della compilazione*: stesso tipo di assunzione implicita ("chi chiama questa funzione ha già verificato che CSL sia disponibile"), ma mai verificata nemmeno dal sistema di build — trovata solo alla prima build con una configurazione (Thread 1.1) mai esercitata prima.
- Il §3, Tappa F (perché non si è toccato `IsRxOnWhenIdle()`) è materiale diretto per la sezione "design rationale" di una eventuale PR — anticipa l'obiezione più ovvia che un reviewer OpenThread farebbe.
- Serve ancora, prima di una submission: numeri di test riproducibili (quanti nodi, quante run, percentuale di successo con intervalli di confidenza se possibile — non solo singole run aneddotiche come quelle raccolte finora), e la Tappa Nexus (§5) per una validazione automatizzata riproducibile da terzi.

---

## 8. Appendice: modifiche file per file rispetto a OpenThread stock

Elenco sistematico di ogni file toccato (`git diff --stat` contro `main` a monte del fork: 23 file, ~1000 righe aggiunte, ~90 rimosse), con **cosa cambia** e **perché**, per riferimento rapido in fase di stesura di paper/PR — non ripete il "perché" già motivato per esteso in §3/§4/§5, ci rimanda.

### `include/openthread/thread.h`
Due dichiarazioni pubbliche nuove: `otThreadSetSleepyRouterMode(otInstance*, bool)`, `otThreadIsSleepyRouterMode(otInstance*)`. Nessuna API esistente modificata — puramente additivo, superficie ABI-compatibile con stock.

### `src/core/api/thread_api.cpp`
Implementazione delle due funzioni sopra: thin wrapper che delega a `Mle::SetSleepyRouterMode()`/`Mle::IsSleepyRouterMode()`. Stesso pattern di ogni altra funzione `otThreadXxx` in questo file (nessuna logica propria a questo livello, solo `AsCoreType(aInstance).Get<Mle::Mle>()...`).

### `src/cli/cli.cpp`
Comando CLI `sleepyrouter enable|disable` (senza argomenti: stampa lo stato corrente), gated `#if OPENTHREAD_FTD`, registrato nella tabella `kCommands` in ordine alfabetico. Espone la API C sopra all'utente interattivo/agli script di test — è il comando usato in ogni test manuale OTNS di questo fork. Nota: la macro `CmdEntry` è stata riformattata (multi-riga) dallo strumento di formattazione, non è una modifica funzionale.

### `src/core/thread/mle.hpp` / `src/core/thread/mle.cpp`
Il cuore del controllo di modalità:
- `mle.hpp`: nuovo membro `bool mIsSleepyRouter` (nota: dichiarato come `bool` semplice, non bitfield `:1` come i suoi vicini — inconsistenza di stile minore, da sistemare se si punta a una PR) + `SetSleepyRouterMode()`/`IsSleepyRouterMode()`.
- `mle.cpp::SetSleepyRouterMode()` — pilota `Mac::SetSleepyRouterCslPeriod()` (annuncio CSL-IE in uscita) e, dalla correzione di Tappa F, anche `Mac::SetCslCapable()`/`Mac::SetCslPeriod()` direttamente (i quattro punti della catena Tappa F, §3/§4 bug #6-#9).
- `mle.cpp::SetDeviceMode()` — bypassa `DeviceMode::IsValid()` quando `IsSleepyRouterMode()` è vero (§3 Tappa F, punto 1) — predisposto ma di fatto **mai esercitato**, dato che `SetSleepyRouterMode()` non chiama mai `SetDeviceMode()` (scelta deliberata: `RxOnWhenIdle` resta sempre `true`).
- `mle.cpp::IsCslSupported()` — esteso da `IsChild() && ...` a `(IsChild() && ...) || IsSleepyRouterMode()` (bug #6).
- `mle.cpp::RetxTracker::RetryInfo::DetachIfMaxAttemptsReached()` — `BecomeDetached()` commentato, sostituito da un log ("HACK") — **non è una modifica Sleepy-Router**, è un tech-debt pre-esistente (§6 punto 4), documentato qui solo perché tocca lo stesso file.

### `src/core/thread/mle_ftd.cpp`
Un solo punto toccato, ma cruciale: `Mle::RemoveNeighbor()`, ramo Router (`else if (aNeighbor.IsStateValid())`) — aggiunta la chiamata a `IndirectSender::ClearAllMessagesForSleepyRouter()` prima di `RemoveRouterLink()` (bug #12, scenario "purge", §4/§5 — risolto e coperto da `pylibs/unittests/test_sleepy_router_purge.py`).

### `src/core/thread/router.hpp`
`class Router : public Neighbor` → `class Router : public CslNeighbor` (Tappa A). I vecchi campi CSL locali di `Router` (`mCslPeriod`/`mCslPhase`/`mCslSynchronized` + accessor) sono commentati, non cancellati — tech debt esplicito, da ripulire (§3 Tappa A lo nota già).

### `src/core/thread/csl_tx_scheduler.hpp` / `.cpp`
- `RescheduleCslTx()`: aggiunto un secondo loop su `Get<RouterTable>()` (mirror del loop `ChildTable` già esistente), filtrato a mano su `Neighbor::kStateValid` (nessun `Iterate()` con filtro per `RouterTable`, §3 Tappa E lo nota); mantenuto anche un blocco dedicato al Parent singolo (non generalizzato nello stesso loop dei Child/Router — commento nel codice stesso lo segnala come possibile ulteriore semplificazione futura, mai fatta).
- `GetNextCslTransmissionDelay()`: firma cambiata da `const CslNeighbor&` a `const CslTxScheduler::NeighborInfo&` — è il cambiamento che ha *risolto* (Tappa A) il bug di hiding C++ descritto in §3/§4 bug #1, rendendolo visibile per la prima volta invece di leggere silenziosamente una copia locale mai scritta.
- `HandleFrameRequest()`: aggiunta una riga di `LogInfo` diagnostica (`mCslTxNeighbor=... isParent=... GetIndirectMessage()=...`) — usata per buona parte del debugging di questa sessione, lasciata nel codice; da valutare se declassarla a `LogDebg` o rimuoverla prima di una PR (rumore nei log a livello Info).

### `src/core/thread/indirect_sender.hpp` / `.cpp`
Il file con più codice nuovo (438 righe). Riassunto per simmetria Child/Parent/Router (dettaglio completo già in §3 Tappe B/C/E):
- Fuso `PrepareFrameForChild`+`PrepareFrameForParent` → `PrepareFrameForCslNeighbor` generica (Tappa B; bug #2 sull'init di `error`).
- `HandleSentFrameToCslNeighbor` discrimina Child vs Parent vs Router invece di castare ciecamente (Tappa C).
- Router: `AddMessageForSleepyRouter`, `RemoveMessageFromSleepyRouter`, `FindQueuedMessageForSleepyRouter`, `UpdateIndirectMessage(Router&)`, `RequestMessageUpdate(Router&)`, `HandleSentFrameToCslRouter`, `ClearAllMessagesForSleepyRouter` (quest'ultima aggiunta per il bug #12) — mirror quasi 1:1 dell'equivalente Parent/Child, con le correzioni sul conteggio `mQueuedMessageCount` (bug #3/#4) e sulla decisione diretto/indiretto (bug #5, insieme al fix gemello in `mesh_forwarder.cpp`).
- Parent: `AddMessageForSleepyParent`, `RemoveMessageFromSleepyParent`, `FindQueuedMessageForSleepyParent`, `UpdateIndirectMessage(Parent&)`, `RequestMessageUpdate(Parent&)`, `HandleSentFrameToCslParent` — questi esistevano già prima dell'inizio di questa sessione (pipeline Child→Parent, Tappe A-D), inclusi qui per completezza dell'inventario.

### `src/core/thread/mesh_forwarder.hpp` / `.cpp`
- `RemoveMessageIfNoPendingTx()`: condizione estesa con `!aMessage.IsPendingForParent()` (pre-esistente) e **`!aMessage.IsPendingForAnyRouter()`** (bug #5, aggiunto in questa sessione) — la funzione che decide se un messaggio può essere liberato, ora consapevole di tutti e tre gli stati "pending" (diretto/child-mask/parent/router).
- `FinalizeMessageIndirectTxsToParent()` (nuova, pre-esistente a questa sessione): chiamata da `FinalizeAndRemoveMessage()`, pulisce lo stato "pending for parent" a fine trasmissione — **non esiste un equivalente esplicito per Router** in questo punto (la pulizia lato Router avviene invece dentro `HandleSentFrameToCslRouter` stesso, e ora anche in `ClearAllMessagesForSleepyRouter` per il caso di rimozione) — asimmetria non verificata come problematica, ma da tenere a mente.
- `HandleFrameRequest()`: contiene un blocco `#if 0` (il vecchio fix pragmatico storico con cui è iniziato il progetto, calcolo di `TxDelay` al volo per un frame diretto verso il parent — §6 punto 5) — **disabilitato ma non rimosso**, tenuto come riferimento storico/rete di sicurezza disattivabile.
- `mIndirectSender.Start()`: gate esteso da `#if OPENTHREAD_FTD` a `#if OPENTHREAD_FTD || OPENTHREAD_CONFIG_MAC_CSL_TRANSMITTER_ENABLE` — necessario perché un Child MTD ora usa `IndirectSender` per la pipeline verso il Parent sleepy, non solo un dispositivo FTD verso i suoi child.

### `src/core/thread/mesh_forwarder_ftd.cpp`
`SendMessage()`, entrambi i rami (dettaglio completo in §3 Tappa E):
- Ramo multicast: esteso da "solo Child" a includere anche ogni Router-peer CSL-sincronizzato (bug #10).
- Ramo unicast: cascata a 4 rami (Child sleepy / Child-di-un-Parent-sleepy / Router-peer-sleepy / diretto) invece del solo `message.SetDirectTransmission()` originale.
`UpdateIp6RouteFtd()`: aggiunta la conversione diretto→indiretto per un Router-peer quando il next-hop risolto coincide con la destinazione finale (bug #11) — stesso pattern già usato da OT stock per il caso ALOC→child sleepy, poche righe sopra nella stessa funzione.
**Bug #14:** tutti e tre i blocchi sopra (cascata unicast, loop multicast, conversione in `UpdateIp6RouteFtd()`) erano gated solo da `#if OPENTHREAD_FTD`, senza il più stretto `OPENTHREAD_CONFIG_MAC_CSL_TRANSMITTER_ENABLE` richiesto dalle funzioni che chiamano — corretto aggiungendo il gate mancante, con fallback `else { message.SetDirectTransmission(); }` nella cascata unicast (nessun fallback necessario per gli altri due, puramente additivi).

### `src/core/thread/mesh_forwarder_mtd.cpp`
Equivalente MTD di `SendMessage()` — cascata a 2 rami (Child-di-Parent-sleepy / diretto), niente Router (un dispositivo MTD non può esserlo). Pre-esistente a questa sessione (pipeline Child→Parent). **Bug #14:** stesso gate mancante (`OPENTHREAD_CONFIG_MAC_CSL_TRANSMITTER_ENABLE`) aggiunto, con lo stesso fallback a diretto.

### `src/core/thread/message_framer.cpp`
`PrepareMacHeaders()` — decide, per ogni frame in uscita, se allocare spazio per un CSL IE (`mAppendCslIe`) e forzare la versione 2015:
- Per frame broadcast, se il dispositivo è un Sleepy Router (`GetSleepyRouterCslPeriod() > 0`).
- Per frame unicast verso un Child, oltre al caso stock (child CSL-sincronizzato), anche incondizionatamente se il dispositivo stesso è un Sleepy Router — così un Router che ha sia Router-peer sia Child riceve comunque l'annuncio del proprio schedule su tutti i frame in uscita, non solo quelli scambiati con altri Router.
Questo è il livello che **alloca lo spazio** per il CSL IE; il contenuto (periodo/fase reali) viene scritto poi da `Mac::BeginTransmit()` (vedi sotto) — due stadi distinti, non ridondanti.

### `src/core/mac/mac_frame.hpp` / `.cpp`
Allargamento meccanico dei guard `#if OPENTHREAD_CONFIG_MAC_CSL_RECEIVER_ENABLE` a `|| OPENTHREAD_FTD` per `SetCslIe()`, `HasCslIe()`, `GetCslIe()`, il campo `mAppendCslIe`, e il bit FCF corrispondente — rende la scrittura/lettura del CSL IE disponibile a un FTD anche indipendentemente dal ruolo di CSL *receiver*, dato che un Router deve poter *scrivere* un CSL IE (lato trasmettitore del proprio annuncio) a prescindere. Un `// TODO: REENABLE OPENTHREAD_FTD MACRO` lasciato nel codice suggerisce che questa era pensata come modifica temporanea/da rivedere.
**Bug #14:** questo allargamento era già corretto in `mac_frame.hpp` (le dichiarazioni restano correttamente annidate dentro il guard esterno `OPENTHREAD_CONFIG_MAC_HEADER_IE_SUPPORT` preesistente), ma in `mac_frame.cpp` le **definizioni** di `SetCslIe`/`HasCslIe`/`GetCslIe` erano rimaste *fuori* da quel guard esterno — asimmetria dichiarazione/definizione, invisibile finché nessuna build con `HEADER_IE_SUPPORT=0` (Thread 1.1) era mai stata provata. Corretto aggiungendo `&& OPENTHREAD_CONFIG_MAC_HEADER_IE_SUPPORT` alle due condizioni in `mac_frame.cpp`.

### `src/core/mac/mac.hpp` / `.cpp`
Il secondo file con più logica nuova, tutto già descritto in dettaglio in §3 Tappa E/F e nel catalogo bug:
- Campo nuovo `mSleepyRouterPeriod` + `SetSleepyRouterCslPeriod()`/`GetSleepyRouterCslPeriod()`/`kDefaultSleepyRouterCslPeriod` — il canale "annuncio" (side-channel CSL-IE), distinto e indipendente dal campo stock `mCslPeriod` (canale "ricezione fisica") — la loro separazione, inizialmente fonte di confusione (bug #9's causa radice), è ora documentata esplicitamente nei commenti del codice.
- `BeginTransmit()`: patcha periodo/fase reali nel CSL IE già allocato da `MessageFramer`, per ogni frame dati in uscita quando `GetSleepyRouterCslPeriod() > 0`.
- `ProcessTransmitSecurity()`: guard esteso `|| OPENTHREAD_FTD` (stesso motivo di `mac_frame.*` sopra).
- `ProcessCsl()`: gestione del caso "il mittente è un Router-peer, non il mio Parent" (§3 Tappa E) — lookup esclusivo sul Parent prima (con `ExitNow()`), poi fallback su `NeighborTable::FindNeighbor()` pubblica + `RouterTable::Contains()` + cast (mai la `RouterTable::FindNeighbor()` privata).
- `UpdateIdleMode()`: `shouldSleep` ora considera anche `IsCslEnabled()`, non solo `mRxOnWhenIdle` (bug #9 — il fix più profondo della catena Tappa F).
- `UpdateCslParameters()`: separato esplicitamente il caso `IsChild()` (comportamento originale invariato) dal caso Router (nessun filtro peer, nessun `ScheduleChildUpdateRequest()` — bug #8).
- **Bug #14:** il blocco Router in `BeginTransmit()` era gated solo `#if OPENTHREAD_FTD`, ma chiama `frame->GetCslIe()` (tipo `CslIe*`), disponibile solo con `OPENTHREAD_CONFIG_MAC_HEADER_IE_SUPPORT` — corretto aggiungendo `&& OPENTHREAD_CONFIG_MAC_HEADER_IE_SUPPORT` al guard.

### `src/core/common/message.hpp`
Nuovi campi/metodi su `Message::Metadata`: `mPendingForParent` (bool, pre-esistente a questa sessione) e `mPendingRouterId : 6` (bitfield, shift-by-uno, aggiunto Tappa E) + i relativi accessor `IsPendingForRouter()`/`IsPendingForAnyRouter()`/`SetPendingForRouter()`/`ClearPendingForRouter()`. Design della codifica motivato per esteso in §3 Tappa E ("bit singolo vs bitmask" — scelto un router-id singolo con shift-by-uno, non un bitmask, perché un messaggio unicast ha sempre un solo prossimo salto).
**Bug #14:** il campo `mPendingRouterId` era gated `#if OPENTHREAD_CONFIG_MAC_CSL_TRANSMITTER_ENABLE`, ma i suoi 4 accessor no — su un build senza quella macro, gli accessor referenziavano un campo inesistente. Risolto rendendo il campo incondizionato, allineandolo a `mPendingForParent` (mai stato gated) invece di aggiungere il guard mancante agli accessor — scelta preferita perché evita di dover rincorrere ogni futuro chiamante con lo stesso gate, esattamente il pattern d'errore che questo bug ha esposto altrove (`mac.cpp`, `mac_frame.cpp`, `mesh_forwarder_ftd/mtd.cpp`).

### `src/core/mac/data_poll_sender.cpp`
Una riga: `CalculatePollPeriod()`, ramo `mRetxMode`, periodo di poll sovrascritto a un valore fisso (25000ms) indipendentemente dal calcolo precedente — tech debt pre-esistente (§6 punto 6 nel design log), non toccato in questa sessione, incluso qui solo per completezza dell'inventario dei file modificati.

### `src/core/config/mac.h`
`OPENTHREAD_CONFIG_MAC_DEFAULT_MAX_FRAME_RETRIES_DIRECT`: `15` → `5`. Riduce i tentativi di ritrasmissione diretta CSMA prima di dichiarare `NoAck` — probabile scelta per velocizzare i test manuali (fallimenti più rapidi da osservare nei log), non motivata esplicitamente altrove. **Da rivalutare** prima di una PR: un valore più basso di quello stock cambia il comportamento anche per traffico non legato a Sleepy Router.

### `src/core/config/mle.h`
`OPENTHREAD_CONFIG_MLE_CHILD_TIMEOUT_DEFAULT`: `240` → `20` secondi. Stessa categoria della voce sopra — accelera il timeout dei child per test più rapidi, **non specifico a Sleepy Router**, non motivato esplicitamente in nessuna conversazione registrata. Da rivalutare/rimuovere prima di una PR, dato che cambia un default globale non solo per lo scenario sleepy.

**Nota generale per la PR:** le ultime due voci (`mac.h`, `mle.h`) e la riga fissa in `data_poll_sender.cpp` sono tweak di comodo per i test manuali, non parte del design Sleepy Router — vanno isolati/rimossi (o spiegati separatamente) per non confondere un reviewer OpenThread sul perché un default globale sia cambiato.
