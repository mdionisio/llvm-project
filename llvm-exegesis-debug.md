# llvm-exegesis su target remoti via debugger — specifiche e piano

Stato: bozza di lavoro (non per upstream). Branch `exegesis-remote`, base
`main` a6b20abcaa0a (2026-10-28).

## 1. Obiettivo

Eseguire i benchmark di llvm-exegesis su un core **senza sistema operativo**
(microcontrollori Cortex-M, ma anche un core A-profile in bare-metal),
controllato da un debugger. Il codice si carica in RAM, il core lo esegue e i
cicli si leggono con un contatore hardware. Lo scopo è validare i modelli di
scheduling su core dove `perf_event_open` non esiste.

Fuori scopo, almeno all'inizio:
- trace ETM (resta un possibile complemento per la diagnosi);
- `ExecutorProcessControl` di ORC;
- i contatori di validazione;
- Windows.

## 2. Architettura

```
llvm-exegesis ──(stdin/stdout, protocollo testuale)──> helper ──> debugger/probe ──> core
   genera e assembla lo snippet                        (RSP, pyOCD, J-Link SDK, …)
   legge la descrizione del target
   calcola i cicli
```

Ogni parte ha una sola responsabilità:
- **descrizione del target** (YAML): indirizzi delle memorie, init opzionale,
  override opzionale della lettura del contatore;
- **target di exegesis** (C++): la sequenza di default per leggere il
  contatore, scelta da triple e CPU;
- **helper** (processo esterno persistente): solo il trasporto.

Non servono né linker script né startup code. Il debugger ferma il core, scrive
codice e dati in RAM, imposta i registri e fa `continue`. Lo snippet termina
tornando (`ret`/`bx lr`) alla "return pad", dove l'helper ha messo un
breakpoint del debugger (`Z1`, altrimenti `Z0`).

Non usiamo un `BRK`/`BKPT` scritto nel codice: su QEMU un `brk` eseguito dal
guest diventa un'eccezione del guest e non arriva al gdbstub, e su A-profile
il comportamento dipende da EDSCR.HDE. La return pad contiene invece `b .`:
se il breakpoint non scatta, il core gira su se stesso e il run va in timeout,
invece di eseguire memoria a caso.

## 3. Descrizione del target (YAML)

```yaml
version: 1
code:    { base: 0x40080000, size: 0x10000 }  # return pad + funzione snippet
scratch: { base: 0x40100000, size: 0x10000 }  # scratch memory, passata nel 1° registro argomento
stack:   { top:  0x40200000 }                 # valore iniziale di SP
results: 0x400ff000                           # area risultati: 2 x u64 LE (start, end)
timeout_ms: 2000
init:                                         # eseguito una volta, in ordine
  - monitor: "reset halt"                     # comando passato al debugger così com'è
  - write32: [0xE000EDFC, 0x01000000]         # DEMCR.TRCENA
  - write32: [0xE0001000, 0x00000001]         # DWT_CTRL.CYCCNTENA
recover:                                      # OPZIONALE: dopo un run fallito (fault o timeout)
  - monitor: "system_reset"
counter:                                      # OPZIONALE: contatore memory-mapped al posto di quello di default
  address: 0xE000E018                         # parola da 32 bit letta dal codice generato
  width: 24                                   # bit che contano (per il wrap)
  down: true                                  # conta all'indietro: cicli = start - end
  scale: 40                                   # cicli del core per conteggio
  setup:                                      # scritture fatte dal codice generato prima della 1ª lettura
    - write32: [0xE000E014, 0x00FFFFFF]
    - write32: [0xE000E010, 0x00000005]
```

Regole:
- `code`, `scratch`, `stack` e `results` non devono sovrapporsi: exegesis lo
  verifica.
- Senza `counter`, la sequenza di lettura la fornisce il target di exegesis.
  Se il target non la conosce per quella CPU, è un errore esplicito.
- Le scritture `write32` di `init` e `recover` passano dall'helper
  (`write`), le voci `monitor` passano dal comando `cmd`. Le scritture di
  `counter.setup` invece le fa il codice generato, a ogni run: su QEMU il
  debugger non riesce a scrivere nel System Control Space (§7.1).
- `counter` sostituisce la lettura di default del target con una `ldr` da 32
  bit all'indirizzo dato. Il target deve implementare
  `readRemoteMemoryCounter()` (oggi solo ARM; su AArch64 è un errore). Al
  posto dell'override `asm:` previsto nella bozza: basta per SysTick e per i
  timer dei SoC, e non serve un assembler nel percorso.
- `recover` serve su M-profile: dopo un fault il core resta nel HardFault
  handler, e il fault successivo lo manda in lockup.

## 4. Protocollo dell'helper (v1)

exegesis lancia l'helper una sola volta per tutta l'esecuzione (persistente) e
ci parla su stdin/stdout. Il protocollo è ASCII, una richiesta per riga e una
risposta per riga. Numeri in esadecimale con prefisso `0x`, dati come stringa
esadecimale continua. L'helper scrive i propri log su stderr, mai su stdout.

| richiesta | risposta | significato |
|---|---|---|
| `hello 1` | `ok 1 <nome-helper>` | negoziazione della versione |
| `write <addr> <hex>` | `ok` | scrive byte in memoria |
| `read <addr> <len>` | `ok <hex>` | legge `len` byte |
| `run <pc> <stop> <timeout_ms> <reg>=<val> …` | `ok halted <pc>` | imposta i registri, breakpoint su `<stop>`, `continue`, attende l'halt |
| `cmd <testo>` | `ok <hex-output>` | comando grezzo del debugger (monitor) |
| `quit` | `ok` | chiude |

- Qualsiasi errore si risponde con `err <messaggio>`. Per `run` scaduto:
  `err timeout`, e l'helper deve lasciare il core fermo.
- I registri usano i nomi di GDB (`x0`, `sp`, `pc`, `x30`/`lr`, `r0`…). Il
  mapping sui numeri del debugger è compito dell'helper (via RSP con
  `qXfer:features:read:target.xml`).
- exegesis considera il run riuscito solo se il PC di halt coincide con la
  return pad. Altrimenti lo snippet è andato in fault e il risultato viene
  scartato con un errore.

Helper di riferimento: `exegesis-gdb-remote-helper.py` (RSP su TCP: QEMU
`-s`, OpenOCD, server GDB di J-Link, `pyocd gdbserver`). Per i test lit:
`exegesis-mock-helper.py`, deterministico.

## 5. Esecuzione remota in llvm-exegesis

Nuove opzioni:
- `--execution-mode=remote`
- `--remote-helper="<comando>"`
- `--remote-target=<file.yaml>`

Flusso per ogni configurazione:
1. Assembla lo snippet come oggi (`assembleToStream`), con le sequenze del
   contatore aggiunte (punto 6).
2. Estrae i byte della funzione dall'oggetto (`getBenchmarkFunctionBytes`).
   Se ci sono rilocazioni è un errore: nel prototipo sono vietate.
3. Scrive return pad (`b .`) e funzione in `code`, azzera `results`.
4. `run <funzione> <return pad> <timeout> <arg0>=<scratch.base> sp=<stack.top> lr=<return pad | tag> <registri extra>`.
   `RemoteABI` dà il tag del bit 0 di LR e i registri extra: su M-profile
   `lr=<pad>|1` e `xpsr=0x1000000` (§6.2). Su AArch64 nessuno dei due.
5. Legge `results` e calcola `(end - start) mod 2^width` (con `counter.down`
   `start - end`), moltiplicato per `counter.scale`.
6. Se il run fallisce (fault o timeout) esegue i passi `recover`.

Il `FunctionExecutor` remoto implementa `runWithCounter` per il solo contatore
dei cicli. Con `--repetition-mode=middle-half-*` il costo fisso (lettura del
contatore, barriere, init dei registri) si cancella.

## 6. Contatore nel codice generato

Ordine nella funzione generata:

```
lettura contatore -> results[0]   (prima dell'init dei registri)
init dei registri dello snippet (setRegTo)
snippet ripetuto
lettura contatore -> results[1]
ret                                -> return pad (BKPT/BRK)
```

La lettura iniziale viene **prima** dell'init dei registri. Così i registri
temporanei che usa non sporcano i valori iniziali dello snippet e non serve
riservare registri. Il costo dell'init è costante e lo cancella middle-half.
L'unico vincolo è non usare come temporaneo il registro argomento della
scratch né SP.

Nuovi hook di `ExegesisTarget` (come implementati):
- `getRemoteABI()`: registri argomento/SP/LR, larghezza del contatore,
  return pad, tag di LR e registri extra (M-profile);
- `readRemoteCycleCounter(Address, Start)`: "abilitazione (solo Start) +
  barriera + lettura + store a `Address`", vuoto se non supportato;
- `readRemoteMemoryCounter(Address, Start, CounterAddress, Setup)`: lo
  stesso per il `counter:` del YAML. Se la sequenza richiesta è vuota,
  exegesis dà errore.

Default per architettura:

| core | lettura | note |
|---|---|---|
| AArch64 (A53, QEMU virt) | `isb; mrs xT, PMCCNTR_EL0` | setup: PMCR_EL0.E/C, PMCNTENSET_EL0[31]; 64 bit |
| ARMv7-M / v8-M (M3/M4/M7/M33) | `dsb; isb; ldr r12, [0xE0001004]` (DWT CYCCNT) | 32 bit; enable nel codice generato (§6.2) |
| M0/M0+ | nessuno: serve `counter:` | SysTick o un timer del SoC |

### 6.1 Scelte fatte nel prototipo

- **AArch64**: i temporanei sono X16 e X17. Il setup del contatore (CPACR_EL1,
  PMCR_EL0, PMCNTENSET_EL0, PMCCFILTR_EL0) viene ripetuto a ogni run dentro la
  lettura iniziale. Non serve una funzione di setup separata, e il costo è
  costante.
- **Scratch**: azzerata una sola volta all'avvio della sessione, non a ogni
  run: via sonda di debug l'azzeramento costerebbe più del run stesso.
- **Errori**: halt fuori dalla return pad e timeout diventano
  `SnippetRemoteFailure` (una `SnippetExecutionFailure`). Finiscono nel campo
  `error` del risultato e non fermano exegesis. Gli errori di trasporto, cioè
  un helper morto o una risposta malformata, restano fatali.
- **`--benchmark-phase` < measure**: la sessione si apre senza helper
  (`createOffline`) e il codice si assembla comunque con le letture del
  contatore, così si può ispezionare con `--dump-object-to-disk`.
- **Rilocazioni**: vietate. Se l'oggetto ne contiene, è un errore.

### 6.2 Target ARM (M-profile, F4)

`lib/ARM/Target.cpp`, attivo per le triple `thumb`/`thumbeb` (solo Thumb-2:
`decrementLoopCounterAndJump` non riceve la subtarget e non può scegliere
tra ARM e Thumb). Provato su Cortex-M7 e Cortex-M4.

- **setRegTo**: GPR con `movw`/`movt`; S con `vmov sN, r11`; D0-D15 tramite
  le due S; flag con `msr apsr_nzcvq, r11`; FPSCR (anche FPSCR_RM/NZCV) con
  `vmsr`. Il temporaneo è r11, che è anche il contatore del loop (`subs.w` +
  `bne`) e quindi non è mai un registro dello snippet: il contatore del loop
  viene caricato dopo il setup.
- **Operandi**: predicato sempre AL, `cc_out` vuoto (forma che non scrive i
  flag). Serve una piccola modifica generica: gli operandi `isPredicate()` e
  `isOptionalDef()` passano da `randomizeTargetMCOperand` (prima restavano
  non inizializzati). Nessun altro target ne ha.
- **Pass**: nessuna espansione di pseudo necessaria. Il ritorno è `tBX_RET`,
  espanso dall'AsmPrinter.
- **ABI remota**: `r0`, `sp`, `lr`; return pad `b .` Thumb (`0xE7FE`);
  `LinkRegisterTag = 1` (il ritorno `bx lr` deve restare in Thumb) e
  `ExtraRegisters = {xpsr=0x01000000}` (bit T): QEMU parte da reset con
  `xPSR = 0x40000000`, cioè T=0, e il primo `run` andrebbe in UsageFault
  INVSTATE e poi in lockup. Il breakpoint resta sull'indirizzo pari della
  return pad. L'helper usa `Z1,<pad>,2` (kind 2) quando `target.xml` ha
  `xpsr`.
- **Lettura**: `movw/movt r3, <contatore>; dsb sy; isb sy; ldr.w r12, [r3]`,
  poi `str` nella parola bassa dello slot e `0` in quella alta. Temporanei
  r3 e r12 (non r0, sp, lr). La prima lettura abilita anche CPACR CP10/CP11
  (FPU, come CPACR_EL1 su AArch64), DEMCR.TRCENA e DWT_CTRL.CYCCNTENA, con
  read-modify-write: non dipende da `init`, che su QEMU non funziona (§7.1).
  Costo costante, si cancella con middle-half.
- **Snippet**: da evitare r11, sp, lr, pc. r0, r3 e r12 si possono usare.

## 7. Insidie note da misurare

- **QEMU non è cycle-accurate**: serve solo per verificare che il percorso
  funzioni, i suoi numeri non contano.
- **A53 in bare-metal (Pi 3 via JTAG)**: con la MMU spenta i dati sono Device
  memory, quindi non in cache e lentissimi. Prima di confrontare con perf
  serve un init con mappa identità e cache accese.
- **M7**: I-cache e D-cache, TCM, branch predictor. Eseguire da ITCM/DTCM
  oppure accendere le cache nell'init.
- **Flash wait state**: mai eseguire da flash, solo da SRAM/TCM.
- **Interrupt**: disabilitati (PRIMASK su M, DAIF su A) dall'helper o
  dall'init.
- Da verificare: che il contatore non conti durante l'halt di debug.
- `--opcode-name` non controlla che l'opcode sia disponibile sulla CPU:
  `VADDD` su cortex-m4 (niente doppia precisione) dà `LLVM ERROR: ...
  Feature_HasDPVFP predicate(s) are not met`. Non è specifico del remoto.

### 7.1 QEMU mps2 (misurato con QEMU 8.2.2, 2026-11-25)

Macchine: `mps2-an500` (Cortex-M7, CPUID 0x411fc272) e `mps2-an386`
(Cortex-M4). Memoria usata: SSRAM1 a 0x0 (4 MiB) per vettori e codice,
SSRAM2/3 a 0x20000000 per scratch, risultati e stack. Su an500 0x21000000 non
è RAM (si rilegge 0).

- **DWT CYCCNT non è implementato**: vale sempre 0, anche abilitato dal
  guest. DEMCR e DWT_CTRL si rileggono a 0. Con la lettura di default
  exegesis misura quindi 0.
- **Le scritture del debugger nel System Control Space sono ignorate**:
  DEMCR, DWT_CTRL, SYST_RVR, SYST_CSR e CPACR scritti dal gdbstub si
  rileggono a 0, e il guest non vede il cambiamento (SYST_CSR = 0). Le
  letture invece funzionano (CPUID). Per questo l'abilitazione si fa nel
  codice generato.
- **SysTick funziona dal guest** e conta a 25 MHz (clock di sistema della
  board). Con `-icount shift=0` (1 istruzione = 1 ns): 20000 istruzioni = 500
  conteggi, cioè 40 istruzioni per conteggio. Da qui `counter:` con
  `scale: 40` in `qemu-mps2.yaml`.
- **Risultati** (middle-half-duplicate, uguali su M7 e M4): snippet `add.w`
  = 1; due add indipendenti = 1 per istruzione; `vmla.f32` = 1;
  middle-half-loop = 3 (add + subs + bne, come su AArch64); `--opcode-name`
  `t2ADDrr`, `t2MUL`, `t2SDIV`, `VMLAS`, `t2ADCrr` = 1; `VADDD` = 1 su M7.
  Con DWT (senza `counter:`) = 0.
- **Perché "circa 1"**: SysTick avanza ogni 40 istruzioni, quindi ogni misura
  ha ±40 istruzioni di quantizzazione. Con 10000 istruzioni i due run danno
  250 e 500 conteggi (minimo su 30 ripetizioni; i singoli run oscillano di 1
  conteggio): (20000 - 10000) / 10000 = 1 esatto. Con 12345: 308 e 617
  conteggi, (24680 - 12320) / 12345 = 1.00122. Su AArch64 PMCCNTR avanza a
  ogni istruzione e il risultato è sempre esattamente 1.
- **Fault**: con la tabella dei vettori vuota (tutta a 0) un fault va in
  lockup e QEMU termina (`qemu: fatal: Lockup: can't escalate 3 to
  HardFault`). `qemu-mps2.yaml` scrive con `init` un HardFault handler `b .`
  a 0x400 (vettori NMI e HardFault a 0x401): lo snippet va in timeout, che
  exegesis riporta come errore dello snippet. Poi `recover` fa
  `monitor system_reset` (il gdbstub di QEMU accetta i comandi HMP anche con
  `-monitor none`; la RAM resta). Verificato: due run di `udf.w #0` di
  seguito danno `the snippet did not stop within 2000 ms` e il run
  successivo misura di nuovo 1.
- **Velocità**: senza `TCP_NODELAY` nell'helper ogni scrittura di memoria
  aspettava un ACK ritardato (~54 ms per pacchetto): un run middle-half da
  10000 istruzioni durava 111 s, con `TCP_NODELAY` 1.05 s. Con 100000
  istruzioni (800 KB di codice per run) resta lento, ~10 s per run.

## 8. File del prototipo

| file | contenuto |
|---|---|
| `llvm/tools/llvm-exegesis/lib/RemoteExecution.{h,cpp}` | descrizione YAML, processo helper (pipe POSIX), sessione |
| `llvm/tools/llvm-exegesis/lib/Target.h` | `RemoteABI`, `getRemoteABI()`, `readRemoteCycleCounter()` |
| `llvm/tools/llvm-exegesis/lib/AArch64/Target.cpp` | implementazione AArch64 (PMCCNTR_EL0) |
| `llvm/tools/llvm-exegesis/lib/ARM/Target.cpp` | target ARM Thumb-2 M-profile (DWT CYCCNT, contatori memory-mapped) |
| `llvm/lib/Target/ARM/ARMPfmCounters.td` | binding di default con il contatore CYCLES (`-gen-exegesis`) |
| `llvm/tools/llvm-exegesis/lib/SnippetGenerator.cpp` | operandi predicato / optional def al target |
| `llvm/tools/llvm-exegesis/lib/Assembler.cpp` | letture del contatore attorno allo snippet |
| `llvm/tools/llvm-exegesis/lib/BenchmarkRunner.{h,cpp}` | `ExecutionModeE::Remote`, `RemoteFunctionExecutorImpl` |
| `llvm/tools/llvm-exegesis/lib/Error.{h,cpp}` | `SnippetRemoteFailure` |
| `llvm/tools/llvm-exegesis/llvm-exegesis.cpp` | `--execution-mode=remote`, `--remote-helper`, `--remote-target` |
| `llvm/utils/exegesis-remote/exegesis-gdb-remote-helper.py` | helper di riferimento (RSP) |
| `llvm/utils/exegesis-remote/exegesis-mock-helper.py` | helper finto per i test |
| `llvm/utils/exegesis-remote/qemu-aarch64-smoke.py` | smoke test F1 su QEMU (senza exegesis) |
| `llvm/utils/exegesis-remote/qemu-aarch64-virt.yaml` | descrizione per QEMU virt |
| `llvm/utils/exegesis-remote/qemu-mps2.yaml` | descrizione per QEMU mps2-an500/an386 (SysTick, handler, recover) |
| `llvm/test/tools/llvm-exegesis/remote/` | test lit (helper finto, girano su qualsiasi host) |

## 9. Piano

| fase | contenuto | hardware | verifica |
|---|---|---|---|
| F0 | worktree, build di exegesis con AArch64 e ARM | — | ✅ avviata |
| F1 | protocollo, helper RSP, helper mock, smoke test con QEMU aarch64 virt e uno snippet scritto a mano | QEMU | ✅ con `-icount shift=0` middle-half dà esattamente 2 istruzioni per iterazione |
| F2 | `--execution-mode=remote`: YAML, processo helper, executor, hook del contatore, AArch64 | QEMU | exegesis completo su QEMU; test lit col mock |
| F3 | Raspberry Pi 3 via JTAG e OpenOCD, init MMU/cache | Pi 3 | confronto con `a53-fir-validation` (perf) |
| F4 | target ARM Thumb (M-profile) in exegesis, DWT, `counter:` memory-mapped | QEMU mps2 e Nucleo | ✅ su QEMU (SysTick, 1 per istruzione a ±40 di quantizzazione); manca CYCCNT contro i modelli M4/M7 su Nucleo |
| F5 | RFC su Discourse con i numeri di F3/F4 | — | — |
| F6 | divisione in patch upstream: (a) protocollo, executor e mock; (b) contatore AArch64; (c) target ARM | — | — |

F1 e la parte del target ARM di F4 si possono portare avanti in parallelo.

## 10. Domande aperte per la RFC

- I pipe stdin/stdout verso un processo figlio non esistono in modo portabile
  in `llvm/Support` (`ExecuteNoWait` redirige solo su file). Pipe POSIX come
  nella subprocess mode, oppure `raw_socket_stream`?
- Dove mettere l'helper di riferimento (`llvm/utils/`?) e se serve un helper in
  C++.
- `FunctionExecutor` dedicato oppure `ExecutorProcessControl` di ORC (scartato
  per ora).
- Il nome della modalità (`remote`? `debugger`?).

## 11. Lavori upstream collegati (ricerca del 2026-11-25)

Nessuna discussione su exegesis per bare-metal o esecuzione remota, né su
GitHub né su Discourse. Discord non è stato verificato.
- [#121993](https://github.com/llvm/llvm-project/pull/121993) (aperta):
  serializza l'oggetto assemblato nel risultato per misurarlo in un secondo
  momento. Separa generazione e misura, ma non supporta ancora middle-half.
- [#131014](https://github.com/llvm/llvm-project/pull/131014) (integrata):
  scelta della CPU quando l'architettura nativa e il target non coincidono.
- [#132861](https://github.com/llvm/llvm-project/pull/132861) (aperta): timeout
  dell'executor in subprocess mode, un concetto simile al nostro
  `timeout_ms`.
- [Discourse: piattaforme supportate](https://discourse.llvm.org/t/clarification-on-platform-support-for-llvm-exegesis/70206):
  conferma che esiste solo AArch64, niente ARM a 32 bit.
