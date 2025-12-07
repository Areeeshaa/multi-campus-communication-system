Concurrency Handling in the Project

Concurrency was a core requirement of this system because the server must handle multiple campuses simultaneously, and each client must process different types of network events at the same time. The project uses POSIX threads (pthreads) to achieve this.

1. Server-Side Concurrency

The server uses multiple threads to manage different responsibilities:

• TCP Connection Handling
The server spawns a new thread for every incoming TCP client connection.
This thread is responsible for:

Authentication

Receiving messages from the client

Parsing protocol fields

Forwarding messages to the target campus

This allows multiple campuses to communicate at the same time without blocking each other.

• UDP Listener Thread
A dedicated thread continuously listens for incoming UDP heartbeat packets.
This ensures that heartbeat processing does not interfere with TCP message routing.

• Admin Console Loop
The server's admin console runs in the main thread, allowing the administrator to issue commands (list, broadcast, sendcmd) without being blocked by network activities.

• Shared Data Synchronization
The server maintains a global client table that stores TCP sockets, UDP addresses, campus names, and timestamps.
To avoid race conditions when multiple threads access this table, a pthread mutex lock (clients_lock) is used.
Every read/write operation is protected using:

pthread_mutex_lock(&clients_lock);
pthread_mutex_unlock(&clients_lock);


This ensures thread-safe updates and prevents data corruption.

2. Client-Side Concurrency

Each client runs multiple threads to handle separate tasks concurrently:

• TCP Receive Thread
Listens for routed messages or admin commands from the server.

• UDP Receive Thread
Receives server broadcast messages (e.g., announcement notifications).

• Heartbeat Thread
Periodically sends heartbeat packets to the server without affecting user input.

• Main Console Thread
Handles user input and outgoing messages.
Because the background threads run independently, the user can send messages while receiving broadcasts or routed messages.# multi-campus-communication-system
