# SO

Projeto desenvolvido no âmbito da unidade curricular de Sistemas Operativos.

O objetivo do projeto é implementar um sistema cliente-servidor simples em C, composto por dois programas principais: `controller` e `runner`.

O `controller` é responsável por gerir os pedidos de execução, controlar o número máximo de comandos em execução ao mesmo tempo, aplicar uma política de escalonamento e manter um registo dos comandos executados. O `runner` é utilizado pelo utilizador para submeter comandos, consultar o estado do sistema ou pedir o encerramento do `controller`.

## Autores

* Rui Cruz — a104355
* João Carvalho — a104533

## Funcionalidades

* Comunicação entre processos através de FIFOs.
* Execução de comandos submetidos pelo `runner`.
* Controlo de paralelismo através do `controller`.
* Suporte para políticas de escalonamento `fifo` e `rr`.
* Consulta de comandos em execução e em espera.
* Encerramento controlado do `controller`.
* Registo persistente dos comandos executados.
* Suporte para operadores como `|`, `<`, `>` e `2>`.

## Tecnologias utilizadas

* C
* Makefile
* Linux/WSL
* Processos
* FIFOs
* Pipes
* Redirecionamento de input/output

## Estrutura do projeto

```text
.
├── include
│   ├── common.h
│   └── runner_exec.h
├── src
│   ├── common.c
│   ├── controller.c
│   ├── runner.c
│   └── runner_exec.c
├── scripts
├── tmp
├── Makefile
```

## Como compilar

A partir da raiz do projeto, executar:

```bash
make clean && make all
```

Este comando cria os executáveis na pasta `bin`.

## Como executar

Primeiro, iniciar o `controller`:

```bash
./bin/controller 1 fifo
```

O primeiro argumento indica o número máximo de comandos que podem executar em paralelo.
O segundo argumento indica a política de escalonamento, podendo ser `fifo` ou `rr`.

Noutro terminal, usar o `runner` para submeter comandos:

```bash
./bin/runner -e 1 echo hello
```

Consultar o estado do sistema:

```bash
./bin/runner -c
```

Encerrar o `controller`:

```bash
./bin/runner -s
```

## Exemplos

Executar um comando simples:

```bash
./bin/runner -e 1 echo hello
```

Executar comandos com redirecionamento:

```bash
./bin/runner -e 1 "grep root /etc/passwd | wc -l > tmp/out.txt"
```

Executar com redirecionamento de erros:

```bash
./bin/runner -e 2 "ls /nao-existe 2> tmp/err.txt"
```

Testar paralelismo com política Round-Robin:

```bash
./bin/controller 2 rr
```

```bash
./bin/runner -e 1 sleep 2 &
./bin/runner -e 2 sleep 2 &
./bin/runner -e 3 sleep 2 &
./bin/runner -e 4 sleep 2 &
wait
./bin/runner -s
```

## Logs

O `controller` guarda informação sobre os comandos executados no ficheiro:

```text
tmp/controller.log
```

Este log inclui dados como o utilizador, comando executado, tempo de espera, tempo de execução, política usada e código de saída.

## Testes

O projeto inclui scripts de teste na pasta `scripts`.

Exemplo:

```bash
bash scripts/test_requirements.sh
```

Também podem ser usados scripts adicionais para testar funcionalidades básicas, paralelismo e políticas de escalonamento.

## Estado do projeto

O projeto implementa a comunicação entre `runner` e `controller`, execução de comandos, escalonamento, paralelismo, redirecionamentos, pipes, logs e encerramento controlado do sistema.
