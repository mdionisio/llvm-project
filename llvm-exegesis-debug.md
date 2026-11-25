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
counter:                                      # OPZIONALE: override della lettura di default
  reg: r12                                    # registro in cui la sequenza lascia il valore
  width: 32                                   # bit del contatore (per il wrap)
  asm: |
    ldr r12, =0x40000424
    ldr r12, [r12]
```

Regole:
- `code`, `scratch`, `stack` e `results` non devono sovrapporsi: exegesis lo
  verifica.
- Senza `counter`, la sequenza di lettura la fornisce il target di exegesis.
  Se il target non la conosce per quella CPU, è un errore esplicito.
- Le scritture `write32` passano dall'helper (`write`), le voci `monitor`
  passano dal comando `cmd`.

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
4. `run <funzione> <return pad> <timeout> sp=<stack.top> lr=<return pad> <arg0>=<scratch.base>`.
5. Legge `results` e calcola `(end - start) mod 2^width`.

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

Nuovi hook di `ExegesisTarget`:
- `generateRemoteCounterRead(Slot, ResultsAddr)`: genera la sequenza "barriera +
  lettura + store a `ResultsAddr + 8*Slot`", oppure vuoto se il target non è
  supportato;
- `getRemoteCounterWidth()`;
- `generateRemoteCounterSetup()`: abilitazione una tantum, eseguita come
  funzione separata all'avvio (per esempio la PMU AArch64).

Default per architettura:

| core | lettura | note |
|---|---|---|
| AArch64 (A53, QEMU virt) | `isb; mrs xT, PMCCNTR_EL0` | setup: PMCR_EL0.E/C, PMCNTENSET_EL0[31]; 64 bit |
| ARMv7-M / v8-M (M3/M4/M33) | `ldr rT, [0xE0001004]` (DWT CYCCNT) | 32 bit; enable via `init` |
| Cortex-M7 | come sopra più `dsb; isb` | dual issue |
| M0/M0+ | nessuno: serve l'override `counter.asm` | SysTick o un timer del SoC |

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

## 8. File del prototipo

| file | contenuto |
|---|---|
| `llvm/tools/llvm-exegesis/lib/RemoteExecution.{h,cpp}` | descrizione YAML, processo helper (pipe POSIX), sessione |
| `llvm/tools/llvm-exegesis/lib/Target.h` | `RemoteABI`, `getRemoteABI()`, `readRemoteCycleCounter()` |
| `llvm/tools/llvm-exegesis/lib/AArch64/Target.cpp` | implementazione AArch64 (PMCCNTR_EL0) |
| `llvm/tools/llvm-exegesis/lib/Assembler.cpp` | letture del contatore attorno allo snippet |
| `llvm/tools/llvm-exegesis/lib/BenchmarkRunner.{h,cpp}` | `ExecutionModeE::Remote`, `RemoteFunctionExecutorImpl` |
| `llvm/tools/llvm-exegesis/lib/Error.{h,cpp}` | `SnippetRemoteFailure` |
| `llvm/tools/llvm-exegesis/llvm-exegesis.cpp` | `--execution-mode=remote`, `--remote-helper`, `--remote-target` |
| `llvm/utils/exegesis-remote/exegesis-gdb-remote-helper.py` | helper di riferimento (RSP) |
| `llvm/utils/exegesis-remote/exegesis-mock-helper.py` | helper finto per i test |
| `llvm/utils/exegesis-remote/qemu-aarch64-smoke.py` | smoke test F1 su QEMU (senza exegesis) |
| `llvm/utils/exegesis-remote/qemu-aarch64-virt.yaml` | descrizione per QEMU virt |
| `llvm/test/tools/llvm-exegesis/remote/` | test lit (helper finto, girano su qualsiasi host) |

## 9. Piano

| fase | contenuto | hardware | verifica |
|---|---|---|---|
| F0 | worktree, build di exegesis con AArch64 e ARM | — | ✅ avviata |
| F1 | protocollo, helper RSP, helper mock, smoke test con QEMU aarch64 virt e uno snippet scritto a mano | QEMU | ✅ con `-icount shift=0` middle-half dà esattamente 2 istruzioni per iterazione |
| F2 | `--execution-mode=remote`: YAML, processo helper, executor, hook del contatore, AArch64 | QEMU | exegesis completo su QEMU; test lit col mock |
| F3 | Raspberry Pi 3 via JTAG e OpenOCD, init MMU/cache | Pi 3 | confronto con `a53-fir-validation` (perf) |
| F4 | target ARM Thumb (M-profile) in exegesis, DWT, override `counter.asm` | QEMU mps2 e Nucleo | CYCCNT contro modelli M4/M7 |
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
