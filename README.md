# Redis-Lite

A lightweight, Redis-inspired in-memory key-value store implemented in **C++**. Redis-Lite provides a TCP server, a command-line client, RESP-based communication, optional key expiration, and Append-Only File (AOF) persistence.

The project was built to explore the fundamentals behind key-value databases, network programming, protocol parsing, data structures, and persistence.

## Features

- **TCP server and client** for communicating over a network connection.
- **RESP protocol support** for encoding commands and parsing requests.
- **Command processing** through a server-side command dispatcher.
- **In-memory data storage** managed by a dedicated Store component.
- **Key expiration (TTL)** for time-limited entries.
- **AOF persistence** to record operations and restore data after a server restart.
- **C++ implementation** using standard libraries and socket programming.

> The exact set of supported commands and data types depends on the current implementation. See [Supported Commands](#supported-commands).

## Architecture

```text
                 Redis-Lite Client
                       |
                       | RESP command
                       | over TCP
                       v
              +-------------------+
              |    TCP Server     |
              +-------------------+
                       |
                       v
              +-------------------+
              |  RESP Parser      |
              | Parse request     |
              +-------------------+
                       |
                       v
              +-------------------+
              | Command Dispatcher|
              | Validate & route  |
              +-------------------+
                       |
                       v
              +-------------------+
              |      Store        |
              | In-memory data    |
              | TTL / persistence |
              +-------------------+
                       |
                       v
              +-------------------+
              |     AOF File      |
              |  aof.txt          |
              +-------------------+
```

### Request lifecycle

1. The client converts a command into the RESP format.
2. The command is sent to the server over TCP.
3. The server parses the incoming request.
4. The command dispatcher validates the command and its arguments.
5. The corresponding Store operation is executed.
6. The server encodes the result as a RESP response and sends it to the client.

## Project Structure

```text
redis-lite/
├── cli.cpp             # Command-line client
├── main.cpp            # Server entry point
├── server.cpp          # TCP server and request handling
├── server.h            # Server declarations
├── resp_parser.cpp     # RESP parsing and serialization
├── resp_parser.h       # RESP parser declarations
├── store.cpp           # In-memory storage and data operations
├── store.h             # Store declarations
├── aof.txt             # Runtime persistence file (if enabled)
├── README.md
└── .gitignore
```

## Requirements

- Linux or WSL (Windows Subsystem for Linux)
- A C++17-compatible compiler, such as `g++`
- Standard C++ library and POSIX socket support

## Build

Clone the repository:

```bash
git clone https://github.com/<your-username>/Redis_Lite.git
cd Redis_Lite
```

Compile the server:

```bash
g++ -std=c++17 main.cpp server.cpp store.cpp resp_parser.cpp -o server
```

Compile the client:

```bash
g++ -std=c++17 cli.cpp resp_parser.cpp -o cli
```

## Run

### 1. Start the server

Open a terminal in the project directory:

```bash
./server
```

Keep the server running.

### 2. Start the client

Open a second terminal in the same project directory:

```bash
./cli
```

You can now send commands through the client.

### 3. Try a basic request

```text
SET name Ronit
GET name
```

The expected values are:

```text
+OK
Ronit
```

The exact response formatting may vary depending on the implementation.

## Supported Commands

Add or remove commands here to match the current implementation.

| Command | Description | Example |
|---|---|---|
| `SET` | Store a value under a key | `SET name Ronit` |
| `GET` | Retrieve a value by key | `GET name` |
| `DEL` | Delete a key | `DEL name` |
| `SET ... EX` | Store a key with an expiration time | `SET session abc123 EX 60` |

### TTL example

```text
SET session abc123 EX 5
GET session
```

The value should be available before its expiration. After the expiration period, the key should no longer be returned.

## Persistence

Redis-Lite includes an Append-Only File (AOF) persistence mechanism.

The AOF file records operations so the server can restore persisted data when it starts again.

To test persistence:

1. Start the server.
2. Store a value:

   ```text
   SET persistent RedisLite
   ```

3. Stop the server.
4. Restart the server.
5. Retrieve the value:

   ```text
   GET persistent
   ```

If AOF replay is enabled and the operation was persisted successfully, the stored value should be restored.

## Concepts Explored

This project provides hands-on experience with:

- TCP sockets and client-server communication
- RESP protocol framing and parsing
- Command dispatching and request validation
- In-memory data structures
- Key expiration and TTL handling
- Append-only persistence and recovery
- C++ modular design and separation of concerns

## Future Improvements

Potential extensions include:

- More complete RESP parsing and protocol validation
- Improved handling of partial TCP reads and writes
- Additional Redis-style data types and commands
- Automated unit and integration tests
- Graceful server shutdown
- Configurable port and persistence-file path
- Improved error handling and client disconnect management

## Disclaimer

Redis-Lite is an educational project inspired by Redis concepts. It is not a drop-in replacement for Redis and is not intended for production use.

## Author

**Ronit Kumar Choudhary**

B.Tech, National Institute of Technology, Jamshedpur
